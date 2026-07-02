
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

#include "HazardPointerManager.hpp"
#include "Constants.hpp"

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


///Lock-Free Treiber Stack MPMC 
template <typename T>
class LockFreeTreiberMPMCStackHazardPointer {
private: 

    //Hazard Pointer-1:
    HazardPointerManager hp;

    struct alignas(CACHE_LINE_SIZE) Node 
    {
        T data;
        std::atomic<Node*> next;
        explicit Node(T const& value) : data(value), next(nullptr) {}
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head{nullptr};  

    
public:
    LockFreeTreiberMPMCStackHazardPointer(const LockFreeTreiberMPMCStackHazardPointer&) = delete;
    LockFreeTreiberMPMCStackHazardPointer& operator=(const LockFreeTreiberMPMCStackHazardPointer&) = delete;
    LockFreeTreiberMPMCStackHazardPointer(LockFreeTreiberMPMCStackHazardPointer&&) = delete;
    //LockFreeTreiberMPMCStackHazardPointer(LockFreeTreiberMPMCStackHazardPointer&& other) noexcept : head(std::move(other.head)) { }        
    LockFreeTreiberMPMCStackHazardPointer& operator=(LockFreeTreiberMPMCStackHazardPointer&&) = delete;

    LockFreeTreiberMPMCStackHazardPointer() = default;  // Default constructor
    
    //:::TIPS: All memory_order_relaxed except CAS success = memory_order_release ::::::
    void push(T const& value) { 
        
        Node* new_node = new Node(value);// In HFT, use a memory pool
        Node* expected_head = head.load(std::memory_order_relaxed); //(A)

        //while(expected_head) ==> wont enter loop if the stack is empty (head == nullptr)
        while(true){
            
            new_node->next.store(expected_head, std::memory_order_relaxed); //(B)
          
            if(head.compare_exchange_weak(expected_head, new_node, 
                    std::memory_order_release, // (C) => (C) will push (A) & (B) above to memory.
                                               //A->B->C will be visible to other threads which use acquire to read//Successful CAS will release the new_node                
                    std::memory_order_relaxed) //1. On CAS failure, expected_head is atomically updated
                                               //   with the current value of head.
                                               //2. No acquire semantics are required here because
                                               //   push() never dereferences expected_head or reads
                                               //   data stored inside the node.
                                               //3. We only need the latest head pointer value so that
                                               //   the next iteration can rebuild:
                                               //       new_node->next = expected_head
                                               //   and retry the CAS.
                                               //4. Using memory_order_relaxed avoids unnecessary
                                               //   synchronization overhead while preserving correctness.
                  ) 
            {
                break; // Successfully pushed the new node
                //Correctness is only required at the instant a CAS succeeds.
                //Everything before that is speculative and may be thrown away.
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

    /*
    read head
      ↓
    set_hazard(head)
      ↓
    CAS attempt
      ↓
    success → retire
    failure → retry
    */
    //:::TIPS: acquire->relaxed->acquire->relaxed ::::::
    bool pop(T& out) {
        
        hp.init_thread();   // once per pop invocation (or per thread)
        
        while (true) {   
            
            Node* old_head = head.load(std::memory_order_acquire);
            if (!old_head) 
              return false; 

            // Hazard Pointer-2:
            // publish hazard BEFORE using old_head -> hazard must be set BEFORE any dereference becomes “unsafe window”
            // So, publish immediately after loading old_head.
            hp.set_hazard(old_head); //Same as ebr.enter_epoch()
          
            Node* new_head = old_head->next.load(std::memory_order_relaxed); //(E-1)
          
            if (head.compare_exchange_weak(old_head, new_head, 
                    std::memory_order_acq_rel, 
                    std::memory_order_relaxed)) 
             {
                out = old_head->data;

                //delete old_head
                // Hazard Pointer-3:
                //Instead of delete, retire old_head
                // Safe memory reclamation
                hp.retire_node(old_head);
               
                return true;
             }
          
          // Hazard Pointer-4:
          //Clear hazard in all exit paths
          hp.clear_hazard(); //Same as ebr.leave_epoch()
          
        }
        return false;
    }

    // Fast empty check (relaxed, may be stale)
    bool empty() const {
        return head.load(std::memory_order_acquire) == nullptr;
    }

    //Single threaded when all other threads have joined and stopped using stack. So, memory_order_relaxed
    ~LockFreeTreiberMPMCStackHazardPointer() {
        Node* current = head.exchange(nullptr, std::memory_order_relaxed);
        while (current) {
           Node* next = current->next.load(std::memory_order_relaxed);
           delete current;
           current = next;
       }
    }

    //Optional:
    // Bulk push for better performance in high-throughput scenarios
    // This is useful in high-frequency trading (HFT) where batch processing is common
    // It allows pushing multiple values at once, reducing contention and improving throughput.
    // In HFT, this can be used to push a batch of market data updates or orders.
    // This method is not thread-safe
    // It is designed for scenarios where the caller can guarantee that no other threads
    // are modifying the stack while this method is called.
    
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
