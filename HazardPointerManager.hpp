
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
#pragma once

#include <atomic>
#include <thread>
#include <stdexcept>

class HazardPointerManager
{
private:
    //int enough for max thread 128
    static constexpr int MAX_THREADS = 128;
    std::atomic<int> next_tid{0};

    static constexpr size_t RETIRE_THRESHOLD = 256;
    

    struct HazardRecord
    {
        std::atomic<void*> pointer{nullptr};
    };

    HazardRecord records[MAX_THREADS];

    struct RetiredNode
    {
        void* ptr;
        void (*deleter)(void*);
    };

    inline static thread_local int tid = -1;
    inline static thread_local std::vector<RetiredNode> retired_list;

public:

    // ----------------------------
    // Init thread once.
    // Same as EBR::()
    // ----------------------------
    void init_thread()
    {
        if (tid != -1) return;

        int id = next_tid.fetch_add(1, std::memory_order_relaxed);
        if (id >= MAX_THREADS)
            throw std::runtime_error("Too many threads");

        tid = id;

        // same as EBR reserve()
        retired_list.reserve(256);
    }

    // ----------------------------
    // Same as EBR::enter_epoch()
    // ----------------------------
    void set_hazard(void* ptr)
    {
        records[tid].pointer.store(ptr, std::memory_order_release);
    }

    // ----------------------------
    // Same as EBR::leave_epoch()
    // ----------------------------
    void clear_hazard()
    {
        if (tid == -1) return;
        records[tid].pointer.store(nullptr, std::memory_order_release);
    }

    // ----------------------------
    // retirement (EBR-style)
    // ----------------------------
    template<typename T>
    void retire_node(T* node)
    {
        retired_list.push_back({
            node,
            [](void* p)
            {
                delete static_cast<T*>(p);
            }
        });

        if (retired_list.size() >= RETIRE_THRESHOLD)
        {
            reclaim();
        }
    }

    // ----------------------------
    // reclaim
    // ----------------------------
    void reclaim()
    {
        auto it = retired_list.begin();

        while (it != retired_list.end())
        {
            if (!is_hazard(it->ptr))
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

    // ----------------------------
    // hazard scan
    // ----------------------------
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
