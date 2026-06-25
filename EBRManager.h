#pragma once

#include <atomic>
#include <vector>
#include <functional>
#include <cstdint>

//EBR = Epoch Based Reclamation
class EBRManager
{
private:

    static constexpr int MAX_THREADS = 128;
    static constexpr size_t RECLAIM_THRESHOLD = 64;

    struct ThreadEpoch
    {
        std::atomic<bool> active{false};
        std::atomic<uint64_t> local_epoch{0};
    };

    struct RetiredNode
    {
        void* ptr;
        uint64_t retire_epoch;
        std::function<void(void*)> deleter;
    };

    std::atomic<uint64_t> global_epoch{0};

    ThreadEpoch thread_epochs[MAX_THREADS];

    std::atomic<int> next_thread_id{0};

    static thread_local int thread_id;

    static thread_local std::vector<RetiredNode> retired_nodes;

public:

    EBRManager() = default;

    int register_thread()
    {
        if(thread_id == -1)
        {
            thread_id =
                next_thread_id.fetch_add(1,
                                         std::memory_order_relaxed);
        }

        return thread_id;
    }

    void enter_epoch()
    {
        register_thread();

        uint64_t epoch =
            global_epoch.load(std::memory_order_acquire);

        thread_epochs[thread_id].local_epoch.store(
            epoch,
            std::memory_order_relaxed);

        thread_epochs[thread_id].active.store(
            true,
            std::memory_order_release);
    }

    void leave_epoch()
    {
        thread_epochs[thread_id].active.store(
            false,
            std::memory_order_release);
    }

    template<typename Node>
    void retire_node(Node* node)
    {
        retired_nodes.push_back(
        {
            node,
            global_epoch.load(std::memory_order_relaxed),
            [](void* p)
            {
                delete static_cast<Node*>(p);
            }
        });

        if(retired_nodes.size() >= RECLAIM_THRESHOLD)
        {
            try_reclaim();
        }
    }

private:

    void try_reclaim()
    {
        uint64_t current_epoch =
            global_epoch.load(std::memory_order_acquire);

        bool can_advance = true;

        for(int i = 0; i < MAX_THREADS; ++i)
        {
            if(thread_epochs[i].active.load(
                    std::memory_order_acquire))
            {
                if(thread_epochs[i].local_epoch.load(
                        std::memory_order_acquire)
                    != current_epoch)
                {
                    can_advance = false;
                    break;
                }
            }
        }

        if(can_advance)
        {
            global_epoch.fetch_add(
                1,
                std::memory_order_acq_rel);
        }

        uint64_t safe_epoch =
            global_epoch.load(std::memory_order_acquire);

        auto it = retired_nodes.begin();

        while(it != retired_nodes.end())
        {
            if(it->retire_epoch + 2 < safe_epoch)
            {
                it->deleter(it->ptr);

                it = retired_nodes.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
};

thread_local int EBRManager::thread_id = -1;

thread_local std::vector<EBRManager::RetiredNode>
    EBRManager::retired_nodes;
