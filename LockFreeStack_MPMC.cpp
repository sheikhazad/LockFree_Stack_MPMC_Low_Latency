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
        
        //1. No need for strcit memory order here, expected_node can hold stale value. 
        //Consitency with correct value will be achieved by the CAS loop (compare_exchange_weak operation)
        //CAS will be failed if head is changed by another thread, and expected_next will be updated with the new head value.
        //new_node->next = head.load(std::memory_order_relaxed); 
        Node* expected_head = head.load(std::memory_order_relaxed); //(A)

        //while(expected_head) ==> wont enter loop if the stack is empty (head == nullptr)
        while(true){
            //2. This store doesn’t need memory_order_release because it’s not publishing shared data yet.
            //It just builds the pointer chain for the stack. 
            //At this point, new_node isn’t visible to other threads. 
            //It will be visible only after successful CAS.
            //Only the CAS needs to carry release semantics to ensure visibility of all writes to the node 
            //(especially node->data = value) before publication.
            new_node->next.store(expected_head, std::memory_order_relaxed); //(B)
        
            //The compare_exchange_weak operation will try to set head to new_node, but only if head is still expected_next.
            //If head has changed (another thread has pushed a new node), expected_head will be updated with the new head value,
            //and the loop will retry with the new expected_head.
            //This ensures that the stack remains lock-free and allows multiple threads to push concurrently without blocking
            //The loop will continue until the head is successfully updated to point to new_node.
    
            //3. memory_order_release ensures all prior writes in this thread—including 
            //new_node->next.store() and any non-atomic variable —are visible to other threads that load with memory_order_acquire 
            //once below CAS succeeds. 
            //So, previous writes do not need memory_order_release
            
            ///std::memory_order_acquire means-all the changes other threads did before their release operation
            //(on the same atomic variable) (published with memory_order_release)—are visible to me after this line. 
            //It becomes synchronization barrier—nothing after this line in current 
            //thread can be reordered to run before it.  
            /*
            Thread A (Writer)        Thread B (Reader)
            Write X ────────┐        ┌─── "Show me everything before the release!"
            Write Y         │        │
            [release] Store ─────────┘
                │        │
                ▼        ▼
            Thread B's `acquire` sees X, Y
            */
            /*********************Example*********************************
            int data;  // Non-atomic
            std::atomic<bool> ready{false};

           // Thread A (Publisher):
           data = 42;  // (1) Non-atomic write
           ready.store(true, std::memory_order_release);  // (2) Publish

           // Thread B (Consumer):
           if (ready.load(std::memory_order_acquire)) {  // (3) Syncs-with (2)
                assert(data == 42);  // (4) Guaranteed
           }**********************************************************/

            if(head.compare_exchange_weak(expected_head, new_node, 
                    std::memory_order_release, // (C) => (C) will push (A) & (B) above to memory.
                                               //A->B->C will be visible to other threads which use acquire to read//Successful CAS will release the new_node                
                    std::memory_order_acquire) //Failed CAS will acquire the expected_next, 
                                               //which is the current head published with memory_order_release by other thread.
                  ) 
            {
                break; // Successfully pushed the new node
            }       
            // Optional: Add brief pause (_mm_pause()) to reduce unnecessary CAS loop contention
            #ifdef __x86_64__
            _mm_pause();  // Lower latency than yield()
            #else
            std::this_thread::yield();
            #endif  // Yield to reduce contention
        }
    }

    bool pop(T& out) {
        
        Node* old_head = head.load(std::memory_order_acquire); //(D) => (D) synchronise with (C) in push()
        if (old_head) __builtin_prefetch(old_head->next.load(std::memory_order_relaxed), 0, 1);

        while (old_head) {           
            //C++ standard says: If operation A happens-before B, and B happens-before C, then A happens-before C.
            //next pointer(B) was written before (C) in push() and so guranteed to see it after synchrnised by (D)
            //We will still see A->B->C once synchronised by (D)
            //So, no need for memory_order_acquire to load next pointer in our case.
            //Node* new_head = old_head->next.load(std::memory_order_relaxed); //(E-1)
            
            //However,if you are not sure 100% that next pointer was updated only before release operation
            //and next pointer may be updated after release operation, 
            //then use memory_order_acquire for safety - to avoid stale next pointer. 
            Node* new_head = old_head->next.load(std::memory_order_acquire); //(E-2)

            //While the initial acquire (D) guarantees visibility of old_head,CAS is the moment ownership is claimed. 
            //That’s where we detach the node from shared memory and begin thread-local access. 
            //Without an acquire on CAS success,we risk the compiler speculating reads (like old_head->data) before the CAS is confirmed.
            //Compiler might transform this:
            /*********
            if (cas(..., release)) {
                out = old_head->data;
            }
            // Into:
            auto tmp = old_head->data;  // Speculative read!
            if (cas(..., release)) {
                out = tmp;  // Uses potentially invalid data
            }*********/
            //So, we need acquire in 3rd argument. Also release as we are removing node, saving then publishing.
            //So, we use acquire + release = memory_order_acq_rel on Success
            if (head.compare_exchange_weak(old_head, new_head, 
                    std::memory_order_acq_rel, //(F-a) On Success 
                    std::memory_order_acquire)) //(F-b) On failure, update old_head with latest correct head
             {
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
    
    void push_bulk_thread_safe(const std::vector<T>& values) {

        //concurrent bulk inserts could cause inconsistencies.
        //Fix: Use per-thread batching queues to avoid race conditions:
        static thread_local std::vector<Node*> local_batch;
        if (local_batch.empty()) {
            local_batch.reserve(values.size());
        }

        for (const auto& v : values) {
            local_batch.emplace_back(new Node(v));
        }
        
        Node* first = local_batch.front();
        Node* last = local_batch.back();
        Node* expected_head = head.load(std::memory_order_relaxed);
        
        last->next.store(expected_head, std::memory_order_release);
        while (!head.compare_exchange_weak(expected_head, first, std::memory_order_release, std::memory_order_relaxed)) {
            // Optional: Add brief pause (_mm_pause()) to reduce unnecessary CAS loop contention
            #ifdef __x86_64__
            _mm_pause();  // Lower latency than yield()
            #else
            std::this_thread::yield();
            #endif  // Yield to reduce contention
        }
        // Clear the local batch after successful push
        local_batch.clear();
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

    int cores_per_node = std::thread::hardware_concurrency() //3.1;
    int core_id = (threadIndex % cores_per_node) + (numaNode * cores_per_node); //3.2
    // Ensure core_id is within valid range
    assert(core_id < std::thread::hardware_concurrency()); //3.3
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
