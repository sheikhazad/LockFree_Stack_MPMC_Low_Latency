#include <atomic>
#include <iostream>
#include <thread>
#include <optional>
#include <cstdlib>
#include <exception>

// Portable CPU pause instruction for spin loops
// Purpose: Reduces power consumption and contention during CAS retries
// Note: Uses architecture-specific instructions for best performance
inline void cpu_pause() {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");  // x86: ~5 cycle latency
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");  // ARM: ~10 cycle latency
#else
    std::this_thread::yield();           // Fallback (higher latency)
#endif
}

// Lock-free stack implementation optimized for high-concurrency scenarios
// Features:
// - Atomic operations with proper memory ordering
// - Graceful shutdown capability
// - Exception safety
template <typename T>
class LockFreeStack {
private:
    // Node structure for stack elements
    // Note: Uses direct data storage for maximum performance
    struct Node {
        T data;       // Payload data (stored by value)
        Node* next;   // Pointer to next node

        // Constructor initializes node with given value
        // Marked explicit to prevent implicit conversions
        explicit Node(const T& value) : data(value), next(nullptr) {}
        
        // Custom aligned allocation to prevent false sharing
        // Alignment set to 64 bytes (common cache line size)
        static void* operator new(size_t size) {
            // Note: aligned_alloc requires size to be multiple of alignment
            if (void* ptr = std::aligned_alloc(64, size)) {
                return ptr;
            }
            throw std::bad_alloc();  // Proper error handling
        }
        
        // Matching deallocator for custom new
        static void operator delete(void* ptr) {
            std::free(ptr);  // Free aligned memory
        }
    };

    // Atomic head pointer aligned to cache line boundary
    // Prevents false sharing between cores accessing the stack
    alignas(64) std::atomic<Node*> head{nullptr};

    // Shutdown flag for graceful termination
    // Atomic to ensure visibility across threads
    std::atomic<bool> shutdown_flag{false};

public:
    // Destructor ensures proper cleanup of remaining nodes
    // Also signals threads to terminate via shutdown_flag
    ~LockFreeStack() {
        shutdown_flag.store(true, std::memory_order_release);
        while (pop()); // Drain remaining nodes
    }

    // Pushes a new value onto the stack
    // Thread-safe and lock-free using CAS operation
    // Complexity: O(1) amortized
    void push(const T& value) {
        // Early exit if shutdown requested
        if (shutdown_flag.load(std::memory_order_acquire)) {
            return;
        }
        
        // Allocate new node (would use memory pool in production)
        Node* new_node = new Node(value);

        // Initialize new node's next pointer
        new_node->next = head.load(std::memory_order_relaxed);

        // CAS loop to update head atomically
        while (!head.compare_exchange_weak(
            new_node->next,
            new_node,
            std::memory_order_release,  // Success synchronization
            std::memory_order_relaxed   // Failure case
        )) {
            // Check for shutdown during retry
            if (shutdown_flag.load(std::memory_order_acquire)) {
                delete new_node;
                return;
            }
            cpu_pause();  // Reduce contention
        }
    }

    // Pops the top value from the stack
    // Returns std::nullopt if stack is empty or shutting down
    // Thread-safe and lock-free
    // Complexity: O(1) amortized
    std::optional<T> pop() {
        Node* old_head = head.load(std::memory_order_relaxed);
        
        while (old_head) {
            // Check for shutdown request
            if (shutdown_flag.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            
            Node* next = old_head->next;
            // Strong CAS to prevent spurious failures
            if (head.compare_exchange_strong(
                old_head,
                next,
                std::memory_order_acq_rel,  // Full barrier on success
                std::memory_order_relaxed   // Relaxed on failure
            )) {
                // Extract data and cleanup node
                T result = std::move(old_head->data);  // Move for efficiency
                delete old_head;
                return result;
            }
            cpu_pause();  // Reduce contention
            old_head = head.load(std::memory_order_relaxed);
        }
        return std::nullopt;
    }

    // Checks if stack appears empty
    // Note: Result may be immediately outdated in concurrent environment
    // Complexity: O(1)
    bool empty() const {
        return head.load(std::memory_order_relaxed) == nullptr;
    }
};

// Example usage with proper error handling
int main() {
    try {
        LockFreeStack<int> stack;
        
        // Producer thread: pushes values onto stack
        auto producer = [&stack]() {
            try {
                for (int i = 0; i < 10; ++i) {
                    stack.push(i);
                    std::cout << "Pushed: " << i << std::endl;
                }
            } catch (...) {
                std::cerr << "Producer thread error" << std::endl;
            }
        };
        
        // Consumer thread: pops values from stack
        auto consumer = [&stack]() {
            try {
                while (true) {
                    if (auto val = stack.pop()) {
                        std::cout << "Popped: " << *val << std::endl;
                    } else if (stack.empty()) {
                        break;  // Exit when stack is empty
                    }
                    cpu_pause();
                }
            } catch (...) {
                std::cerr << "Consumer thread error" << std::endl;
            }
        };
        
        std::thread producer_thread(producer);
        std::thread consumer_thread(consumer);
        
        // Proper thread cleanup
        producer_thread.join();
        consumer_thread.join();
        
    } catch (const std::exception& e) {
        std::cerr << "Main error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown error" << std::endl;
        return 1;
    }
    
    return 0;
}
