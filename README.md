# Lock-free-stack_Ultra_Low_Latency
Lock-free-stack using Ultra_Low_Latency techniques 

**For Ultra faster version, switch to branch Faster_With_Hazard_Pointer**

**Explanations:**

1. Before Each Method

push(T const& value)
Pushes a new value onto the stack in a lock-free manner.
Uses CAS (Compare-And-Swap) to ensure atomicity.
pop()
Removes and returns the top value from the stack.
Returns nullptr if the stack is empty.
Uses CAS to avoid race conditions.
empty()
Checks if the stack is empty (non-blocking, but not strictly thread-safe for precise checks).
2. Against Complex Logic

CAS Loop in push and pop
Ensures atomic updates by retrying if another thread modifies head.
Uses compare_exchange_weak for efficiency (may fail spuriously on some architectures).
Memory Ordering (std::memory_order)
std::memory_order_release (in push) ensures changes are visible to other threads.
std::memory_order_acq_rel (in pop) ensures proper synchronization.
3. Against Hard-Coded Values

No magic numbers → All synchronization is handled via std::memory_order.
No fixed-size buffers → Dynamic node allocation.

**ABA Problem Mitigation**

This implementation uses std::shared_ptr to avoid the ABA problem (where a node is freed and reallocated between a thread's read and CAS). For even lower latency, hazard pointers or epoch-based reclamation can be used instead.

