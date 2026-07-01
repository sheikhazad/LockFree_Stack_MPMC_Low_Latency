
#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <thread>

/*
    Simplified Fraser-style EBR (Epoch Based Reclamation)

    Key idea:
    - Threads announce which epoch they are working in
    - Retired nodes are stored with an epoch tag
    - Memory is freed only when ALL threads have moved past that epoch

Each thread does:
enter_epoch()
  -> work on shared structure
  -> retire nodes
leave_epoch()

Reclaimer does:
-find oldest active thread epoch
-free everything older than that
*/

class EBRManager
{
private:
    static constexpr int MAX_THREADS = 128;
    //How long we delay reclamation (helps avoid race edge cases)
    static constexpr uint64_t RETIRE_DELAY = 2;

    std::atomic<int> next_tid{0};
    //Global epoch (advanced during reclamation)
    std::atomic<uint64_t> global_epoch{0};

    // ----------------------------
    // Thread state tracking
    // ----------------------------
    struct ThreadState
    {
        std::atomic<uint64_t> epoch;
        std::atomic<bool> active;

        ThreadState() : epoch(0), active(false) {}
    };

    ThreadState threads[MAX_THREADS];

    // ----------------------------
    // Retired node entry
    // ----------------------------
    struct RetiredNode
    {
        void* ptr;
        uint64_t epoch;
        void (*deleter)(void*);
    };

    // Thread-local storage
    //INLINE THREAD_LOCAL (no out-of-class definition needed)
    inline static thread_local int tid = -1;
    inline static thread_local std::vector<RetiredNode> retired_list;

public:

    // ----------------------------
    // Register thread once.
    // Same as HP::init_thread()
    // ----------------------------
    void init_thread()
    {
        if (tid != -1) 
            return;

        int id = next_tid.fetch_add(1, std::memory_order_relaxed);
        if (id >= MAX_THREADS)
            throw std::runtime_error("Too many threads");

        tid = id;
        
        retired_list.reserve(256);
    }


    // ----------------------------
    // Enter critical region
    // ----------------------------
    void enter_epoch()
    {
        uint64_t e = global_epoch.load(std::memory_order_acquire);

        /*relaxed epoch store:
        ✔ just data update
        ✔ no ordering needed

        release active store so that other thread which reclaim sees it:
        ✔ publishes thread state
        ✔ guarantees epoch visibility to reclaimer
        ✔ prevents stale epoch observation*/

        threads[tid].epoch.store(e, std::memory_order_relaxed);
        threads[tid].active.store(true, std::memory_order_release);

    }

    // ----------------------------
    // Leave critical region
    // ----------------------------
    void leave_epoch()
    {
        threads[tid].active.store(false, std::memory_order_release);
    }

    // ----------------------------
    // Retire a node (NOT freed immediately)
    // ----------------------------
    template<typename T>
    void retire_node(T* node)
    {
        retired_list.push_back({
            node,
            global_epoch.load(std::memory_order_relaxed),
            [](void* p) { delete static_cast<T*>(p); }
        });

        // Batch cleanup trigger
        if (retired_list.size() >= 256)
        {
            advance_epoch();
            reclaim();
        }
    }

private:
 
    // Advance epoch (keeps system moving forward)
    void advance_epoch()
    {
        global_epoch.fetch_add(1, std::memory_order_relaxed);
    }

    // ----------------------------
    // Reclaim safe memory
    // ----------------------------
    void reclaim()
    {
        uint64_t cur_epoch = global_epoch.load(std::memory_order_acquire);
        uint64_t oldest_active_thread_epoch = cur_epoch;

        // Find oldest epoch among all active threads
        for (int i = 0; i < MAX_THREADS; ++i)
        {
            if (threads[i].active.load(std::memory_order_relaxed))
            {
                uint64_t e = threads[i].epoch.load(std::memory_order_acquire);
                if (e < oldest_active_thread_epoch)
                    oldest_active_thread_epoch = e;
            }
        }

        // Safe to reclaim anything sufficiently older than
        // the oldest active thread's epoch
        uint64_t safe_epoch =
            (oldest_active_thread_epoch > RETIRE_DELAY) ? (oldest_active_thread_epoch - RETIRE_DELAY) : 0;

        //Reclaim only nodes whose retire epoch is older than
        //(oldest active thread's epoch - RETIRE_DELAY).
        //RETIRE_DELAY provides an extra safety buffer before deletion.
        auto it = retired_list.begin();

        while (it != retired_list.end())
        {
            if (it->epoch <= safe_epoch)
            {
                it->deleter(it->ptr);
                it = retired_list.erase(it);
            }
            else
            {
                ++it;
            }
        }

    }
};

// ----------------------------
// Thread-local definitions
// ----------------------------
//thread_local int EBRManager::tid = -1;
//thread_local std::vector<EBRManager::RetiredNode> EBRManager::retired_list;
