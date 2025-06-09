Optimizations Applied:

**Hazard Pointers**
Added basic hazard pointer protection to solve ABA problem
Replaced shared_ptr with direct storage + hazard pointers

**Memory Optimization**
Custom aligned allocation (64-byte cache line alignment)
Removed shared_ptr overhead for data storage

**Low-Latency Techniques**
Added _mm_pause() in spin loops to reduce contention
Used std::optional for cleaner empty stack handling
Move semantics for data extraction

**Cache Optimization**
Proper cache line alignment for head pointer
Reduced false sharing potential

**Thread Safety**
Maintained all atomic operations with appropriate memory orders
Kept lock-free guarantees
