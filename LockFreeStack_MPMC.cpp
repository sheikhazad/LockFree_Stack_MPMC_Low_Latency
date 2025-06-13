#include <atomic>
#include <memory>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
//#include <immintrin.h> // Required for _mm_pause
#define _GNU_SOURCE  // Required for CPU affinity functions
#include <sched.h>   // Contains cpu_set_t definition
#include <pthread.h> // Required for pthread_setaffinity_np()


// Align nodes to cache lines to avoid false sharing
#ifndef hardware_destructive_interference_size
#define hardware_destructive_interference_size 64
#endif

constexpr int NUM_PRODUCERS = 4;
constexpr int NUM_CONSUMERS = 4;
constexpr int WORKLOAD = 1000;

// NUMA node assignments
constexpr int NUMA_NODE_0 = 0;  // Producers on NUMA node 0
constexpr int NUMA_NODE_1 = 1;  // Consumers on NUMA node 1

constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;


template <typename T>
class LockFreeStack {

private:
    struct alignas(CACHE_LINE_SIZE) Node {
        T data;          // Store data directly (avoid shared_ptr overhead)
        std::atomic<Node*> next; //Two threads modifying next concurrently leads to data races or even segmentation faults.
        explicit Node(T const& value) : data(value), next(nullptr) {}
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head{nullptr};  
    
public:
    
    void push(T const& value) {
        Node* new_node = new Node(value);  // In HFT, use a memory pool
        //new_node->next = head.load(std::memory_order_relaxed);
        Node* expected_next = head.load(std::memory_order_relaxed);  // Load atomically
        new_node->next.store(expected_next, std::memory_order_release);  // Store atomically

        while (!head.compare_exchange_weak(
            expected_next,
            new_node,
            std::memory_order_release,
            std::memory_order_relaxed
        )) {
            // Optional: Add brief pause (_mm_pause()) to reduce unnecessary CAS loop contention
            //_mm_pause();  // Use _mm_pause() for x86, or std::this_thread::yield() for portability
            std::this_thread::yield(); // Yield to allow other threads to progress
        }
    }

    // Pop with no shared_ptr overhead (returns T directly via optional)
    bool pop(T& out) {
        Node* old_head = head.load(std::memory_order_relaxed);
        // Optional: Prefetch node to improve cache locality
        if (old_head){  
             __builtin_prefetch(old_head,0,1); //Accessing a pointer in if() doesn't necessarily load its referenced data, so prefetch it
             //__builtin_prefetch(old_head->next, 0, 1);  
             __builtin_prefetch(old_head->next.load(std::memory_order_relaxed), 0, 1);  // ✅ Safe atomic load
  
        }
        
        while (old_head != nullptr) {
            Node* next = old_head->next.load(std::memory_order_acquire);  // ✅ Explicitly load atomic `next`

            if (head.compare_exchange_weak(
                old_head,
                next,
                std::memory_order_acq_rel,
                std::memory_order_relaxed
            )) {
                out = old_head->data;  // Copy data before deletion
                delete old_head;
                return true;
            }
            
        }
        return false;  // Stack empty
    }

    // Fast empty check (relaxed, may be stale)
    bool empty() const {
        return head.load(std::memory_order_relaxed) == nullptr;
    }

    ~LockFreeStack() {
        Node* current = head.exchange(nullptr, std::memory_order_acquire);
        while (current) {
           Node* next = current->next;
           delete current;
           current = next;
       }
    }


    // Disable copy operations
    LockFreeStack(const LockFreeStack&) = delete;
    LockFreeStack& operator=(const LockFreeStack&) = delete;

    // Disable move operations
    LockFreeStack(LockFreeStack&&) = delete;
    LockFreeStack& operator=(LockFreeStack&&) = delete;

    LockFreeStack() = default;  // Default constructor
    //LockFreeStack(LockFreeStack&& other) noexcept : head(std::move(other.head)) { }        


    //Optional:
    // Bulk push for better performance in high-throughput scenarios
    // This is useful in high-frequency trading (HFT) where batch processing is common
    // It allows pushing multiple values at once, reducing contention and improving throughput.
    // In HFT, this can be used to push a batch of market data updates or orders.
    // This method is not thread-safe
    // It is designed for scenarios where the caller can guarantee that no other threads
    // are modifying the stack while this method is called.
    void push_bulk(const std::vector<T>& values) {
        Node* first = nullptr;
        Node* last = nullptr;
        for (const auto& v : values) {
            Node* new_node = new Node(v);
            if (!first) first = new_node;
            if (last) last->next = new_node;
            last = new_node;
        }
        last->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(last->next, first, std::memory_order_release, std::memory_order_relaxed)) {}
    }
};


 /*Optional: NUMA-aware CPU pinning function
              Since HFT workloads often run on multi-socket machines, pinning producer and 
              consumer threads to dedicated cores can reduce cache-line contention.
              */
// This function pins a thread to a specific core within a NUMA node
void pinThreadToCore(int threadIndex, int numaNode) {
   

        /* Available for Linux systems, for windows use SetThreadAffinityMask()
        // Set CPU affinity to bind thread to a specific core
        // This is optional and can be adjusted based on the system's architecture
        // This example uses a round-robin assignment based on thread index 
        */
    // Create a CPU set to hold the core assignment
    // Note: NUMA_NODE_0 and NUMA_NODE_1 are defined as constants for producer and consumer threads
    //cpu_set_t cpuset;  //1
    //CPU_ZERO(&cpuset); //2

    // Assign cores in round-robin within the specified NUMA node
    // Calculate core ID based on thread index and NUMA node
    // For example, if NUMA_NODE_0 has cores 0-3 and NUMA_NODE_1 has cores 4-7,
    // this will assign producers to cores 0-3 and consumers to cores 4-7.
    // This is a simple example; in practice, you may want to use a more sophisticated mapping    
    //int core_id = (threadIndex % std::thread::hardware_concurrency()) + (numaNode * (NUM_PRODUCERS + NUM_CONSUMERS));
    //CPU_SET(core_id, &cpuset); //3
    // Set the thread affinity to the specified core
    //pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);//4
}

int main() {

    LockFreeStack<int> stack;
    std::vector<std::thread> threads;

    // Producers
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back([i, &stack]() { 
            
           pinThreadToCore(i, NUMA_NODE_0); // Assign producer to NUMA node 0

            for (int j = 0; j < WORKLOAD; ++j) {
                stack.push(j);  // Push data (batch processing can be added)
            }
        });
    }

    // Consumers
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back([i, &stack]() { 

            pinThreadToCore(i, NUMA_NODE_1); // Assign consumer to NUMA node 1

            int value;
            while (stack.pop(value)) {  // Process data
            }
        });
    }

    // Join threads
    for (auto& t : threads) {
        t.join();
    }

    return 0;
}
