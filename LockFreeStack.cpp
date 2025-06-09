#include <atomic>
#include <iostream>
#include <thread>
#include <optional>
#include <cstdlib>
#include <exception>

inline void cpu_pause() {
    #if defined(__x86_64__) || defined(__i386__)
        asm volatile("pause" ::: "memory");
    #elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
    #else
        std::this_thread::yield();
    #endif
}

template <typename T>
class LockFreeStack {
private:
    struct Node {
        T data;
        Node* next;
        
        explicit Node(const T& value) : data(value), next(nullptr) {}
        
        static void* operator new(size_t size) {
            if (void* ptr = std::aligned_alloc(64, size)) {
                return ptr;
            }
            throw std::bad_alloc();
        }
        
        static void operator delete(void* ptr) {
            std::free(ptr);
        }
    };

    alignas(64) std::atomic<Node*> head{nullptr};
    std::atomic<bool> shutdown_flag{false};

public:
    ~LockFreeStack() {
        shutdown_flag.store(true, std::memory_order_release);
        while (pop()); // Drain stack
    }

    void push(const T& value) {
        if (shutdown_flag.load(std::memory_order_acquire)) {
            return;
        }
        
        Node* new_node = new Node(value);
        new_node->next = head.load(std::memory_order_relaxed);
        
        while (!head.compare_exchange_weak(
            new_node->next,
            new_node,
            std::memory_order_release,
            std::memory_order_relaxed
        )) {
            if (shutdown_flag.load(std::memory_order_acquire)) {
                delete new_node;
                return;
            }
            cpu_pause();
        }
    }

    std::optional<T> pop() {
        Node* old_head = head.load(std::memory_order_relaxed);
        
        while (old_head) {
            if (shutdown_flag.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            
            Node* next = old_head->next;
            if (head.compare_exchange_strong(
                old_head,
                next,
                std::memory_order_acq_rel,
                std::memory_order_relaxed
            )) {
                T result = std::move(old_head->data);
                delete old_head;
                return result;
            }
            cpu_pause();
            old_head = head.load(std::memory_order_relaxed);
        }
        return std::nullopt;
    }

    bool empty() const {
        return head.load(std::memory_order_relaxed) == nullptr;
    }
};

int main() {
    try {
        LockFreeStack<int> stack;
        
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
        
        auto consumer = [&stack]() {
            try {
                while (true) {
                    if (auto val = stack.pop()) {
                        std::cout << "Popped: " << *val << std::endl;
                    } else if (stack.empty()) {
                        break;
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
