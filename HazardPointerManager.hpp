#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <unordered_set>

/*************************WHY EBR FASTER THAN HAZARD POINTER FOR MEMORY RELCAMATION********
EBR (Epoch-Based Reclamation) is usually faster than Hazard Pointers because it removes the 
per-operation bookkeeping cost that hazard pointers pay for. The difference is basically:
*Hazard Pointers = “protect every pointer you touch”
*EBR = “prove you are not using old memory anymore, then bulk-delete safely”

🧠 1. The core idea difference
🟡 Hazard Pointers (HP):
Every thread does this:
1.1. Before dereferencing a pointer:
     hp.set_hazard(ptr);
1.2. Other threads must scan ALL hazard records before deleting anything.

So HP is:
per-load protection
per-pointer announcement
global scanning for reclamation

🟢 EBR (Epoch-Based Reclamation):
Threads do:
1. Enter critical section → publish “I am in epoch X”
2. Use memory freely
3. Leave → mark “I am out”
4. Retire nodes into epoch buckets
5. Reclaim only when ALL threads have moved forward

So EBR is:
per-section tracking (not per-pointer)
batch reclamation
no scanning of pointers

⚡ 2. Why EBR is faster
🔥 (1) No per-pointer atomic writes
Hazard Pointer:
Every pop does:
hp.set_hazard(node);   // atomic write
hp.clear_hazard();     // atomic write
    
So, at least 2 atomic operations per pop
often more under contention

EBR:
Usually:
enter_epoch();
pop();
leave_epoch();

So, 1–2 atomic operations per batch of operations
NOT per pointer
👉 This is the biggest win.

(2) No global scanning
Hazard Pointers:
Before reclaiming:
scan ALL hazard pointers from all threads

Cost: O(num_threads)

every reclamation cycle

If 64 threads:
-64 * loads per reclaim batch
-happens frequently under load

EBR:
No scanning at all.
Instead:
just track minimum epoch of active threads

Cost: O(1)
(or small bounded checks)

📦 (3) Batch reclamation (cache-friendly)
Hazard Pointer:
Deletes happen:
-irregular
-scattered
-per-node
👉 bad cache locality

EBR:
Nodes are:
-grouped by epoch
-freed in batches
👉 great cache locality
👉 fewer cache misses
👉 better branch prediction

🔁 (4) Less synchronization traffic
Hazard Pointer:
Every thread constantly updates:
-hazard slots
-shared memory
So, high cache line bouncing

EBR:
Threads mostly:
-update epoch counter occasionally
-no constant shared writes
👉 far lower cache contention

🧮 3. Complexity comparison
| Feature            | Hazard Pointer    | EBR           |
| ------------------ | ----------------- | ------------- |
| Per operation cost | High              | Low           |
| Memory writes      | frequent          | rare          |
| Reclamation cost   | O(N threads) scan | O(1) check    |
| Deletion style     | immediate-safe    | batch delayed |
| Cache behavior     | poor              | excellent     |

⚠️ 4. Why HP is still used
Hazard pointers win in:
-fine-grained safety
-real-time correctness guarantees
-simpler reasoning per pointer
-no epoch coordination needed
So HP is:
👉 safer for complex pointer graphs    

🚀 5. Why EBR dominates in HFT / low-latency systems
In systems like:
-order books
-MPMC queues
-market data pipelines

We want:
✔ minimal atomic ops
✔ minimal cache bouncing
✔ predictable latency
✔ batch cleanup

So EBR gives:
👉 much lower tail latency (p99/p999)
👉 higher throughput under load

🧠 Simple intuition
Hazard Pointer: “I will protect every single pointer I touch.”
Expensive but precise.

EBR: “I will tell you when I stop using old memory. Until then, don’t delete anything.”
Cheap but delayed.

🔥 One-line summary
EBR is faster because it:
replaces per-pointer protection + global scanning
with per-thread epoch tracking + batch reclamation
*******************************************************/

//Safe memory reclamation using Hazard Pointers
/*
In pop():
Without hazard pointers:

Thread A reads old_head
Thread B pops + deletes old_head
Thread A crashes → use-after-free

With hazard pointers:

Thread A says: "I am using old_head"
Thread B sees hazard → does NOT delete => Safe
*/
class HazardPointerManager
{
private:
    static constexpr int MAX_THREADS = 128;

    struct HazardRecord
    {
        std::atomic<std::thread::id> tid;
        std::atomic<void*> pointer;

        HazardRecord() : tid(), pointer(nullptr) {}
    };

    HazardRecord records[MAX_THREADS];

    HazardPointerManager() = default;

public:
    static HazardPointerManager& instance()
    {
        static HazardPointerManager hp;
        return hp;
    }

    void register_thread()
    {
        std::thread::id this_id = std::this_thread::get_id();

        for (int i = 0; i < MAX_THREADS; ++i)
        {
            std::thread::id empty;
            if (records[i].tid.compare_exchange_strong(empty, this_id,
                    std::memory_order_acq_rel))
            {
                return;
            }
        }

        throw std::runtime_error("Too many threads for Hazard Pointers");
    }

    void set_hazard(void* ptr)
    {
        std::thread::id this_id = std::this_thread::get_id();

        for (int i = 0; i < MAX_THREADS; ++i)
        {
            if (records[i].tid.load(std::memory_order_acquire) == this_id)
            {
                records[i].pointer.store(ptr, std::memory_order_release);
                return;
            }
        }
    }

    void clear_hazard()
    {
        std::thread::id this_id = std::this_thread::get_id();

        for (int i = 0; i < MAX_THREADS; ++i)
        {
            if (records[i].tid.load(std::memory_order_acquire) == this_id)
            {
                records[i].pointer.store(nullptr, std::memory_order_release);
                return;
            }
        }
    }

    bool is_hazard(void* ptr)
    {
        for (int i = 0; i < MAX_THREADS; ++i)
        {
            if (records[i].pointer.load(std::memory_order_acquire) == ptr)
                return true;
        }
        return false;
    }
};
