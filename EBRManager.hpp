
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
*/

class EBRManager
{
private:
    static constexpr int MAX_THREADS = 128;

    // How long we delay reclamation (helps avoid race edge cases)
    static constexpr uint64_t RETIRE_DELAY = 2;

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

    // Global epoch (advanced during reclamation)
    std::atomic<uint64_t> global_epoch{0};

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
    static thread_local int tid;
    static thread_local std::vector<RetiredNode> retired_list;

    std::atomic<int> next_tid{0};

public:

    // ----------------------------
    // Register thread once
    // ----------------------------
    int register_thread()
    {
        if (tid != -1)
            return tid;

        int id = next_tid.fetch_add(1, std::memory_order_relaxed);

        if (id >= MAX_THREADS)
            std::abort(); // too many threads

        tid = id;
        return tid;
    }

    // ----------------------------
    // Enter critical region
    // ----------------------------
    void enter_epoch()
    {
        int id = register_thread();

        uint64_t e = global_epoch.load(std::memory_order_acquire);

        threads[id].epoch.store(e, std::memory_order_relaxed);
        threads[id].active.store(true, std::memory_order_release);
    }

    // ----------------------------
    // Leave critical region
    // ----------------------------
    void leave_epoch()
    {
        int id = register_thread();
        threads[id].active.store(false, std::memory_order_release);
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
        if (retired_list.size() >= 64)
        {
            reclaim();
        }
    }

private:

    // ----------------------------
    // Reclaim safe memory
    // ----------------------------
    void reclaim()
    {
        uint64_t cur_epoch = global_epoch.load(std::memory_order_acquire);

        uint64_t min_epoch = cur_epoch;

        // Find oldest active thread epoch
        for (int i = 0; i < MAX_THREADS; ++i)
        {
            if (threads[i].active.load(std::memory_order_acquire))
            {
                uint64_t e = threads[i].epoch.load(std::memory_order_acquire);
                if (e < min_epoch)
                    min_epoch = e;
            }
        }

        // Safe epoch threshold
        uint64_t safe_epoch =
            (min_epoch > RETIRE_DELAY) ? (min_epoch - RETIRE_DELAY) : 0;

        // Reclaim loop
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

        // Advance epoch (keeps system moving forward)
        global_epoch.store(cur_epoch + 1, std::memory_order_release);
    }
};

// ----------------------------
// Thread-local definitions
// ----------------------------
thread_local int EBRManager::tid = -1;
thread_local std::vector<EBRManager::RetiredNode> EBRManager::retired_list;
