
#pragma once

#include <atomic>
#include <memory>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#define _GNU_SOURCE  // Required for CPU affinity functions
#include <sched.h>   // Contains cpu_set_t definition
#include <pthread.h> // Required for pthread_setaffinity_np()

//#include <immintrin.h> // Required for _mm_pause()
#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #define CPU_RELAX() _mm_pause()

#elif defined(__aarch64__) || defined(__arm64__)
    #include <arm_acle.h>
    #define CPU_RELAX() __yield()

#else
    #define CPU_RELAX() std::this_thread::yield()
#endif

#include "EBRManager.hpp" //For Epoch Based Reclamation (EBR)

// Align nodes to cache lines to avoid false sharing
#ifndef hardware_destructive_interference_size
#define hardware_destructive_interference_size 64
#endif


constexpr int NUM_PRODUCERS = 4;
constexpr int NUM_CONSUMERS = 4;
constexpr int WORKLOAD = 1000;
constexpr int NUMA_NODE_0 = 0;  // Producers on NUMA node 0
constexpr int NUMA_NODE_1 = 1;  // Consumers on NUMA node 1
constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;

///Lock-Free Treiber Stack MPMC with EBR (Epoch Based Reclamation)
template <typename T>
class LockFreeTeiberMPMCStackEBR {
private:

    //EBR-1: 
    EBRManager ebr;
    

    struct alignas(CACHE_LINE_SIZE) Node 
    {
        T data;
        std::atomic<Node*> next;
        explicit Node(T const& value) : data(value), next(nullptr) {}
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head{nullptr};  
    
public:
    LockFreeTeiberMPMCStackEBR(const LockFreeTeiberMPMCStackEBR&) = delete;
    LockFreeTeiberMPMCStackEBR& operator=(const LockFreeTeiberMPMCStackEBR&) = delete;
    LockFreeTeiberMPMCStackEBR(LockFreeTeiberMPMCStackEBR&&) = delete;
    //LockFreeTeiberMPMCStackEBR(LockFreeTeiberMPMCStackEBR&& other) noexcept : head(std::move(other.head)) { }        
    LockFreeTeiberMPMCStackEBR& operator=(LockFreeTeiberMPMCStackEBR&&) = delete;

    LockFreeTeiberMPMCStackEBR() = default;  // Default constructor
    
    //:::TIPS: All memory_order_relaxed except CAS success = memory_order_release ::::::
    void push(T const& value) 
    {
        //EBR-2:
        ebr.register_thread();
        
        Node* new_node = new Node(value);// In HFT, use a memory pool        
        Node* expected_head = head.load(std::memory_order_relaxed); //(A)

        //while(expected_head) ==> wont enter loop if the stack is empty (head == nullptr)
        while(true)
        {
            new_node->next.store(expected_head, std::memory_order_relaxed); //(B)
            if(head.compare_exchange_weak(expected_head, new_node, 
                    std::memory_order_release, 
                    std::memory_order_relaxed) ) 
            {
                break; 
            }       
            // Optional: Add brief pause (_mm_pause()) to reduce unnecessary CAS loop contention
            /*
            #ifdef __x86_64__
            _mm_pause();  // Lower latency than yield()
            #else
            std::this_thread::yield();
            #endif  // Yield to reduce contention
            */
            CPU_RELAX();
 
            // expected_head is updated here on every failure
            // loop retries with the new value
        }
    }
  
    //:::TIPS: acquire->relaxed->acquire->relaxed ::::::
    //Flow: enter_epoch() -> pop() -> retire_node() -> leave_epoch()
    bool pop(T& out) {

        //EBR-3:
        ebr.enter_epoch(); // internally calls register_thread()
        
        while (true) {   
            
            Node* old_head = head.load(std::memory_order_acquire);
            if (!old_head) 
            {
                //EBR-4:
                //return false;           
                ebr.leave_epoch();
                return false;
            }
          
            Node* new_head = old_head->next.load(std::memory_order_relaxed); //(E-1)
            if (head.compare_exchange_weak(old_head, new_head, 
                    std::memory_order_acq_rel, 
                    std::memory_order_relaxed)) 
             {
                out = old_head->data;

                 //EBR-5:  
                 //delete old_head;
                 ebr.retire_node(old_head);

                //EBR-6:
                 //return true;
                 ebr.leave_epoch(); //NEVER access old_head after leave_epoch()
                 return true;      
             }
        }
        return false; //Unreachable code, no need for ebr.leave_epoch();
    }

    // Fast empty check (relaxed, may be stale)
    bool empty() const {
        return head.load(std::memory_order_acquire) == nullptr;
    }

    //Single threaded when all other threads have joined and stopped using stack. So, memory_order_relaxed
    ~LockFreeTeiberMPMCStackEBR() {
        Node* current = head.exchange(nullptr, std::memory_order_relaxed);
        while (current) {
           Node* next = current->next.load(std::memory_order_relaxed);
           delete current;
           current = next;
       }
    }
    
    void push_bulk_thread_unsafe(const std::vector<T>& values)
    {
            if (values.empty())
                return;
        
            Node* first = new Node(values[0]);
            Node* last  = first;
        
            for (size_t i = 1; i < values.size(); ++i)
            {
                Node* new_node = new Node(values[i]);
        
                // Stack order:
                // values[0] will be popped first
                last->next.store(new_node, std::memory_order_relaxed);
                //last = last->next.load(std::memory_order_relaxed); 
                last = new_node; //Faster, no need to load again like last = last->next.load(..)
            }
        
            Node* expected_head = head.load(std::memory_order_relaxed);
        
            while (true)
            {
                // Attach existing stack after our chain
                last->next.store(expected_head, std::memory_order_relaxed);
        
                if (head.compare_exchange_weak(
                        expected_head,
                        first,
                        std::memory_order_release,
                        std::memory_order_relaxed))
                {
                    break;
                }
        
                CPU_RELAX();
            }
   } 

    
};
