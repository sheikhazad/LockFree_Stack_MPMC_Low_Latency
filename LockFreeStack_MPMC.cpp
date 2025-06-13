#include <atomic>
#include <memory>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#define _GNU_SOURCE  // Required for CPU affinity functions
#include <sched.h>   // Contains cpu_set_t definition
#include <pthread.h> // Required for pthread_setaffinity_np()
//#include <emmintrin.h> // Required for _mm_pause()

// Align nodes to cache lines to avoid false sharing
#ifndef hardware_destructive_interference_size
#define hardware_destructive_interference_size 64
#endif


constexpr int NUM_PRODUCERS = 4;
constexpr int NUM_CONSUMERS = 4;
constexpr int WORKLOAD = 1000;
constexpr int NUMA_NODE_0 = 0;  // Producers on NUMA node 0
constexpr int NUMA_NODE_1 = 1;  // Consumers on NUMA node 1
constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;

template <typename T>
class LockFreeStack {
private:
    struct alignas(CACHE_LINE_SIZE) Node {
        T data;
        std::atomic<Node*> next;
        explicit Node(T const& value) : data(value), next(nullptr) {}
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head{nullptr};  
    
public:
    void push(T const& value) {
        Node* new_node = new Node(value);// In HFT, use a memory pool
        
        //new_node->next = head.load(std::memory_order_relaxed);
        Node* expected_next = head.load(std::memory_order_relaxed);
        new_node->next.store(expected_next, std::memory_order_release);

        while (!head.compare_exchange_weak(expected_next, new_node, 
                std::memory_order_release, std::memory_order_relaxed)) {
            // Optional: Add brief pause (_mm_pause()) to reduce unnecessary CAS loop contention
            //_mm_pause();  // Use _mm_pause() for x86, or std::this_thread::yield() for portability
            //_mm_pause() is Lower overhead than std::this_thread::yield()
            std::this_thread::yield();  // Yield to reduce contention
        }
    }

    bool pop(T& out) {
        Node* old_head = head.load(std::memory_order_relaxed);
        if (old_head) __builtin_prefetch(old_head->next.load(std::memory_order_relaxed), 0, 1);

        while (old_head) {
            Node* next = old_head->next.load(std::memory_order_acquire);
            if (head.compare_exchange_weak(old_head, next, 
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                out = old_head->data;
                delete old_head;
                return true;
            }
        }
        return false;
    }

    // Fast empty check (relaxed, may be stale)
    bool empty() const {
        return head.load(std::memory_order_acquire) == nullptr;
    }

    ~LockFreeStack() {
        Node* current = head.exchange(nullptr, std::memory_order_acquire);
        while (current) {
           Node* next = current->next.load(std::memory_order_acquire);
           delete current;
           current = next;
       }
    }

    // Disable copy operations
    LockFreeStack(const LockFreeStack&) = delete;
    LockFreeStack& operator=(const LockFreeStack&) = delete;

    // Disable move operations
    LockFreeStack(LockFreeStack&&) = delete;
    //LockFreeStack(LockFreeStack&& other) noexcept : head(std::move(other.head)) { }        

    LockFreeStack& operator=(LockFreeStack&&) = delete;

    LockFreeStack() = default;  // Default constructor
    

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
            if (last) last->next.store(new_node, std::memory_order_release);
            last = new_node;
        }

        Node* expected_head = head.load(std::memory_order_relaxed);
        last->next.store(expected_head, std::memory_order_release);
        while (!head.compare_exchange_weak(expected_head, first, std::memory_order_release, std::memory_order_relaxed)) {
            //_mm_pause();  // Reduce contention
            std::this_thread::yield();
        }
    }
};

 /*Optional: NUMA-aware CPU pinning function
              Since HFT workloads often run on multi-socket machines, pinning producer and 
              consumer threads to dedicated cores can reduce cache-line contention.
              */
/* This function pins a thread to a specific core based on the thread index and NUMA node.
 * It is designed for Linux systems using pthreads.
 * For Windows, use SetThreadAffinityMask() instead.
 */
void pinThreadToCore(int threadIndex, int numaNode) {
    /*
    cpu_set_t cpuset; //1
    CPU_ZERO(&cpuset); //2

    // Assign cores in round-robin within the specified NUMA node
    // Calculate core ID based on thread index and NUMA node
    // For example, if NUMA_NODE_0 has cores 0-3 and NUMA_NODE_1 has cores 4-7,
    // this will assign producers to cores 0-3 and consumers to cores 4-7.
    // This is a simple example; in practice, you may want to use a more sophisticated mapping 

    int core_id = (threadIndex % NUM_PRODUCERS) + (numaNode * NUM_PRODUCERS); //3
    CPU_SET(core_id, &cpuset); //4
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); //5
    */
}

int main() {
    LockFreeStack<int> stack;
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back([i, &stack]() { 
            pinThreadToCore(i, NUMA_NODE_0);
            for (int j = 0; j < WORKLOAD; ++j) {
                stack.push(j);
            }
        });
    }

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back([i, &stack]() { 
            pinThreadToCore(i, NUMA_NODE_1);
            int value;
            while (stack.pop(value)) { }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return 0;
}
