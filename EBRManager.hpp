#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <limits>

class EBRManager
{
private:
    static constexpr int MAX_THREADS = 128;
    static constexpr uint64_t RETIRE_DELAY = 2;

    struct ThreadState
    {
        std::atomic<uint64_t> epoch{0};
        std::atomic<bool> active{false};
    };

    struct RetiredNode
    {
        void* ptr;
        uint64_t retire_epoch;
        void (*deleter)(void*);
    };

    std::atomic<uint64_t> global_epoch{0};
    ThreadState threads[MAX_THREADS];

    static thread_local int tid;
    static thread_local std::vector<RetiredNode> retired;

    std::atomic<int> next_tid{0};

public:

    int register_thread()
    {
        if (thread_id != -1)
            return thread_id;
    
        int id = next_tid.fetch_add(1, std::memory_order_relaxed);
    
        if (id >= MAX_THREADS)
            std::abort();
    
        thread_id = id;
        return id;
    }

    void enter_epoch()
    {
        int id = register_thread();

        uint64_t e = global_epoch.load(std::memory_order_acquire);

        threads[id].epoch.store(e, std::memory_order_relaxed);
        threads[id].active.store(true, std::memory_order_release);
    }

    void leave_epoch()
    {
        int id = register_thread();
        threads[id].active.store(false, std::memory_order_release);
    }

    template<typename T>
    void retire_node(T* node)
    {
        retired.push_back({
            node,
            global_epoch.load(std::memory_order_relaxed),
            [](void* p) { delete static_cast<T*>(p); }
        });

        if (retired.size() >= 64)
        {
            reclaim();
        }
    }

private:

    void reclaim()
    {
        uint64_t cur = global_epoch.load(std::memory_order_acquire);

        uint64_t min_epoch = cur;

        // find oldest active thread epoch
        for (int i = 0; i < MAX_THREADS; ++i)
        {
            if (threads[i].active.load(std::memory_order_acquire))
            {
                uint64_t e = threads[i].epoch.load(std::memory_order_acquire);
                if (e < min_epoch)
                    min_epoch = e;
            }
        }

        // safe epoch = everything older than min_epoch - RETIRE_DELAY
        uint64_t safe_epoch = (min_epoch > RETIRE_DELAY)
                                ? (min_epoch - RETIRE_DELAY)
                                : 0;

        auto it = retired.begin();

        while (it != retired.end())
        {
            if (it->retire_epoch <= safe_epoch)
            {
                it->deleter(it->ptr);
                it = retired.erase(it);
            }
            else
            {
                ++it;
            }
        }

        global_epoch.store(cur + 1, std::memory_order_release);
    }
};

thread_local int EBRManager::tid = -1;
thread_local std::vector<EBRManager::RetiredNode> EBRManager::retired;
