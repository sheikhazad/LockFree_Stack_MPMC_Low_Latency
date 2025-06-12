#include <atomic>
#include <memory>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <mutex>

// Align nodes to cache lines to avoid false sharing
#ifndef hardware_destructive_interference_size
#define hardware_destructive_interference_size 64
#endif
constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;


template <typename T>
class LockFreeStack {

private:
    struct alignas(CACHE_LINE_SIZE) Node {
        T data;          // Store data directly (avoid shared_ptr overhead)
        Node* next;

        explicit Node(T const& value) : data(value), next(nullptr) {}
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head{nullptr};  
    std::mutex cleanup_mutex;  // Mutex for cleanup (non-critical path)
    
public:
    
    void push(T const& value) {
        Node* new_node = new Node(value);  // In HFT, use a memory pool
        new_node->next = head.load(std::memory_order_relaxed);

        while (!head.compare_exchange_weak(
            new_node->next,
            new_node,
            std::memory_order_release,
            std::memory_order_relaxed
        )) {
            // Optional: Add brief pause (_mm_pause()) for contention
        }
    }

    // Pop with no shared_ptr overhead (returns T directly via optional)
    bool pop(T& out) {
        Node* old_head = head.load(std::memory_order_relaxed);
        if (old_head) __builtin_prefetch(old_head->next, 0, 1);  // Optional: GCC/Clang prefetch
        
        while (old_head != nullptr) {
            if (head.compare_exchange_weak(
                old_head,
                old_head->next,
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
        //Add a mutex (non-critical path)
        std::lock_guard<std::mutex> lock(cleanup_mutex);
        Node* current = head.load(std::memory_order_relaxed);
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


int main() {
    LockFreeStack<int> stack;

    // Producer (market data feed)
    auto producer = [&stack]() {
        for (int i = 0; i < 1000; ++i) {
            stack.push(i);  // In HFT, batch pushes are better
        }
    };

    // Consumer (matching engine)
    auto consumer = [&stack]() {
        int value;
        while (stack.pop(value)) {  // No shared_ptr overhead
            // Process order (e.g., match trades)
        }
    };

    // Run 4 producers and 4 consumers (MPMC test)
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(producer);
        threads.emplace_back(consumer);
    }
    for (auto& t : threads) t.join();

    return 0;
}
