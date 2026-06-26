
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
#include <vector>
#include <thread>
#include <unordered_set>

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
