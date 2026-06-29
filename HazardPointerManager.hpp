
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
    static constexpr int MAX_THREADS = 128;

    struct HazardRecord
    {
        std::atomic<void*> pointer;

        HazardRecord() : pointer(nullptr) {}
    };

    HazardRecord records[MAX_THREADS];

    std::atomic<int> next_tid{0};
    inline static thread_local int tid = -1;

    HazardPointerManager() = default;

public:
    static HazardPointerManager& instance()
    {
        static HazardPointerManager hp;
        return hp;
    }

    // ------------------------------------------------------------
    // Register thread once → assigns a fixed slot
    // ------------------------------------------------------------
    void register_thread()
    {
        if (tid != -1)
            return;

        int id = next_tid.fetch_add(1, std::memory_order_relaxed);

        if (id >= MAX_THREADS)
            throw std::runtime_error("Too many threads for Hazard Pointers");

        tid = id;
    }

    // ------------------------------------------------------------
    // Publish hazard pointer
    // ------------------------------------------------------------
    void set_hazard(void* ptr)
    {
        register_thread();
        records[tid].pointer.store(ptr, std::memory_order_release);
    }

    // ------------------------------------------------------------
    // Clear hazard pointer
    // ------------------------------------------------------------
    void clear_hazard()
    {
        if (tid == -1) return;
        records[tid].pointer.store(nullptr, std::memory_order_release);
    }

    // ------------------------------------------------------------
    // Check if a pointer is currently protected
    // (used during reclamation scan)
    // ------------------------------------------------------------
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

// thread-local slot id
//inline thread_local int HazardPointerManager::tid = -1;
