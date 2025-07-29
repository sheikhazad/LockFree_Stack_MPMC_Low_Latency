#include <atomic>
#include <vector>
#include <thread>
#include <algorithm>
#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>

// Constants
constexpr size_t CACHE_LINE_SIZE = 64;
constexpr int NUM_PRODUCERS = 4;
constexpr int NUM_CONSUMERS = 4;
constexpr int WORKLOAD = 1000;
constexpr int EPOCH_ADVANCE_INTERVAL = 1'000'000;

// Epoch Management
static std::atomic<int> global_epoch{0};
static thread_local int thread_epoch = 0;
static thread_local size_t operation_count = 0;

template <typename T>
class LockFreeStack {
private:
    struct alignas(CACHE_LINE_SIZE) Node {
        T data;
        std::atomic<Node*> next;
        int retirement_epoch;
        explicit Node(const T& val) : data(val), next(nullptr), retirement_epoch(-1) {}
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head{nullptr};
    static thread_local std::vector<Node*> deferred_deletion_list;

    // Lock-free memory pool
    class NodePool {
        std::atomic<Node*> free_list{nullptr};
    public:
        Node* allocate(T val) {
            Node* node = free_list.load(std::memory_order_relaxed);

            //while (node && !free_list.compare_exchange_weak(node, node->next, std::memory_order_acq_rel)) {}
            //Above while look() is wrong becaue:
            //node->next is read outside the CAS (Compare-And-Swap) loop, 
            //but another thread could modify it after the read but before the CAS succeeds.
            //This can lead to corrupted memory if node->next changes unexpectedly.

            //return node ? node : new Node(val);
            while(node)
            {
                Node* next = node->next.load(std::memory_order_relaxed);
                if(free_list.compare_exchange_weak(node, next, 
                    std::memory_order_release,  // On success
                    std::memory_order_acquire)) // On failure
                {
                    //Reset before returning the node, preventing stale data.
                    node->data = val; 
                    node->next.store(nullptr, std::memory_order_relaxed);
                    node->retirement_epoch = -1;
                    return node;
                }
            }
            return new Node(val); // Fallback if no free node available
        }

        void deallocate(Node* node) {
            Node* old = free_list.load(std::memory_order_relaxed);
            do {
                node->next.store(old, std::memory_order_relaxed);
            } while (!free_list.compare_exchange_weak(old, node, std::memory_order_release, std::memory_order_relaxed));
        }
    };
    
    static NodePool pool;

    void advance_epoch() {
        operation_count++;
        if (operation_count % EPOCH_ADVANCE_INTERVAL == 0 || deferred_deletion_list.size() > 100) {
            global_epoch.fetch_add(1, std::memory_order_release);
            reclaim_memory();
        }
    }

public:
    //For memory order explanation: See LockFreeStack_MPMC.cpp
    void push(T value) {
        Node* new_node = pool.allocate(value);
        Node* expected_head = head.load(std::memory_order_relaxed);
        unsigned backoff = 1;

        //while(expected_head) ==> wont enter loop if the stack is empty (head == nullptr)
        while (true)
        {
            new_node->next.store(expected_head, std::memory_order_relaxed);
            
            //Below CAS() release ensures all prior writes in this thread—including new_node->next.store()—are visible to 
            //other threads that load with memory_order_acquire once below CAS succeeds. 
            //So, previous writes do not need memory_order_release
            if (head.compare_exchange_weak(expected_head, new_node, std::memory_order_release, std::memory_order_acquire)) 
            {
                //new_node->retirement_epoch = thread_epoch;
                //deferred_deletion_list.push_back(new_node);
                break; // Successfully pushed the new node
                
            }
            for (unsigned i = 0; i < backoff; ++i) {
                std::this_thread::yield();
            }
            backoff = std::min(backoff * 2, 1024u);
        }
        advance_epoch();
    }

    bool pop(T& out) {
        thread_epoch = global_epoch.load(std::memory_order_acquire);
        Node* old_head = head.load(std::memory_order_relaxed);
        
        if (old_head) {
            __builtin_prefetch(old_head, 0, 3);
            __builtin_prefetch(old_head->next, 0, 1);
        }

        unsigned backoff = 1;
        while (old_head) 
        {
            //Acuire here is necessary because CAS() below checks only old_head but not its contents like next pointer.
            //If we do not use acquire here, then other threads may modify old_head->next
            //after we read old_head but before we call CAS() below, which means we may read a stale next pointer.
            Node* new_head = old_head->next.load(std::memory_order_acquire);  
    
            //Push() published with memory_order_release, so pop() need to use memory_order_acq + rel because:
            //new_head needs to be published to other threads           
            if (head.compare_exchange_weak(old_head, new_head, 
                std::memory_order_acq_rel, 
                std::memory_order_acquire)) //On failure, we need to acquire the latest head
            {
                out = old_head->data;
                old_head->retirement_epoch = thread_epoch;
                deferred_deletion_list.push_back(old_head);
                advance_epoch();
                return true;
            }
            for (unsigned i = 0; i < backoff; ++i) 
                std::this_thread::yield();
            backoff = std::min(backoff * 2, 1024u);
        }
        return false;
    }

    void reclaim_memory() {
        int current_epoch = global_epoch.load(std::memory_order_acquire);
        auto it = deferred_deletion_list.begin();
        while (it != deferred_deletion_list.end()) {
            if ((*it)->retirement_epoch < current_epoch - 1) {
                pool.deallocate(*it);
                it = deferred_deletion_list.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool empty() const {
        return head.load(std::memory_order_acquire) == nullptr;
    }

    ~LockFreeStack() {
        reclaim_memory();
    }
};

// Static member definitions
template <typename T>
thread_local std::vector<typename LockFreeStack<T>::Node*> LockFreeStack<T>::deferred_deletion_list;

template <typename T>
typename LockFreeStack<T>::NodePool LockFreeStack<T>::pool;

// NUMA pinning (Linux)
void pin_thread(int thread_idx, int numa_node) {
    /*
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int cores_per_node = std::thread::hardware_concurrency() / 2;
    int core_id = (thread_idx % cores_per_node) + (numa_node * cores_per_node);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    */
}

int main() {
    LockFreeStack<int> stack;
    std::vector<std::thread> producers, consumers;

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        producers.emplace_back([i, &stack] {
            pin_thread(i, 0);
            for (int j = 0; j < WORKLOAD; ++j) {
                stack.push(j);
            }
        });
    }

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        consumers.emplace_back([i, &stack] {
            pin_thread(i, 1);
            int val;
            while (stack.pop(val)) {}
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    return 0;
}
