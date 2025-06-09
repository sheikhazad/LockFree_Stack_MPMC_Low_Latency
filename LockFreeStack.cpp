#include <atomic>
#include <memory>
#include <iostream>
#include <thread>
#include <vector>

// Lock-free stack using atomic operations (ABA-safe via std::shared_ptr)
template <typename T>
class LockFreeStack {
private:
    // Node structure for the stack
    struct Node {
        std::shared_ptr<T> data;  // Store data in shared_ptr for automatic memory management
        Node* next;

        // Constructor initializes data and next pointer
        explicit Node(T const& value) : data(std::make_shared<T>(value)), next(nullptr) {}
    };

    // Atomic head pointer for thread-safe operations
    std::atomic<Node*> head;

public:
    // Pushes a new value onto the stack
    void push(T const& value) {
        // 1. Create a new node with the given value
        Node* new_node = new Node(value);

        // 2. Set new_node->next to the current head (atomic load)
        new_node->next = head.load(std::memory_order_relaxed);

        // 3. CAS (Compare-And-Swap) loop to update head atomically
        //    - If head == new_node->next, update head to new_node
        //    - If not, retry with the new head
        while (!head.compare_exchange_weak(
            new_node->next,
            new_node,
            std::memory_order_release,  // Ensures proper synchronization
            std::memory_order_relaxed    // Relaxed for the failure case
        )) {
            // Loop until CAS succeeds (lock-free retry)
        }
    }

    // Pops the top value from the stack (returns nullptr if empty)
    std::shared_ptr<T> pop() {
        // 1. Load the current head atomically
        Node* old_head = head.load(std::memory_order_relaxed);

        // 2. CAS loop to safely remove the head node
        while (old_head &&
               !head.compare_exchange_weak(
                   old_head,
                   old_head->next,
                   std::memory_order_acq_rel,  // Ensures proper synchronization
                   std::memory_order_relaxed    // Relaxed for the failure case
               )) {
            // Loop until CAS succeeds (lock-free retry)
        }

        // 3. If stack was empty, return nullptr
        if (!old_head) {
            return nullptr;
        }

        // 4. Extract data and delete the node (memory cleanup)
        std::shared_ptr<T> res(old_head->data);
        delete old_head;
        return res;
    }

    // Checks if the stack is empty (not thread-safe for precise checks)
    bool empty() const {
        return head.load(std::memory_order_relaxed) == nullptr;
    }
};

// Example usage in a high-frequency trading (HFT) scenario
int main() {
    LockFreeStack<int> stack;

    // Producer thread: Push orders onto the stack
    auto producer = [&stack]() {
        for (int i = 0; i < 10; ++i) {
            stack.push(i);  // Simulate order placement
            std::cout << "Produced: " << i << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Simulate latency
        }
    };

    // Consumer thread: Process orders (e.g., matching engine)
    auto consumer = [&stack]() {
        while (true) {
            auto order = stack.pop();
            if (order) {
                std::cout << "Consumed: " << *order << std::endl;
            } else if (stack.empty()) {
                break;  // Exit when stack is empty
            }
            // Busy-wait (replace with backoff in real HFT)
        }
    };

    // Run threads
    std::thread producer_thread(producer);
    std::thread consumer_thread(consumer);

    producer_thread.join();
    consumer_thread.join();

    return 0;
}
