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

#include "Constants.hpp"


//Lock-Free Treiber Stack MPMC 
template <typename T>
class LockFreeTeiberMPMCStack {
private:
    struct alignas(CACHE_LINE_SIZE) Node 
    {
        T data;
        std::atomic<Node*> next;
        explicit Node(T const& value) : data(value), next(nullptr) {}
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head{nullptr};  
    
public:
    LockFreeTeiberMPMCStack(const LockFreeTeiberMPMCStack&) = delete;
    LockFreeTeiberMPMCStack& operator=(const LockFreeTeiberMPMCStack&) = delete;
    LockFreeTeiberMPMCStack(LockFreeTeiberMPMCStack&&) = delete;
    //LockFreeTeiberMPMCStack(LockFreeTeiberMPMCStack&& other) noexcept : head(std::move(other.head)) { }        
    LockFreeTeiberMPMCStack& operator=(LockFreeTeiberMPMCStack&&) = delete;

    LockFreeTeiberMPMCStack() = default;  // Default constructor
    
    //:::TIPS: All memory_order_relaxed except CAS success = memory_order_release ::::::
    void push(T const& value) {
        Node* new_node = new Node(value);// In HFT, use a memory pool
        
        //1.We only need a snapshot of head(expected_head) here.
        //If another thread changes head in between load() the CAS,
        //compare_exchange_weak() will fail and update expected_head 
        //with the current correct head value for the next iteration 
        //Load() doesnt need to be inside while() as CAS will update stale value with correct value

        //We never dereference expected_head in push().
        // It is only used as the expected value for CAS,so relaxed ordering is sufficient.
        Node* expected_head = head.load(std::memory_order_relaxed); //(A)

        //while(expected_head) ==> wont enter loop if the stack is empty (head == nullptr)
        while(true){
            //2. This store doesn’t need memory_order_release because it’s not publishing shared data yet.
            //It just builds the pointer chain for the stack. 
            //At this point, new_node isn’t visible to other threads. 
            //It will be visible only after successful CAS.
            //Only the CAS needs to carry release semantics to ensure visibility of all writes to the node 
            //(especially node->data = value) before publication.
            //new_node->next = head.load(std::memory_order_relaxed); 
            new_node->next.store(expected_head, std::memory_order_relaxed); //(B)
        
            //The compare_exchange_weak operation will try to set head to new_node, but only if head is still expected_next.
            //If head has changed (another thread has pushed a new node), expected_head will be updated with the new head value,
            //and the loop will retry with the new expected_head.
            //This ensures that the stack remains lock-free and allows multiple threads to push concurrently without blocking
            //The loop will continue until the head is successfully updated to point to new_node.
    
            //3. memory_order_release ensures all prior writes in this thread—including 
            //new_node->next.store() and any non-atomic variable —are visible to other threads that load with memory_order_acquire 
            //once below CAS succeeds. 
            //So, previous writes do not need memory_order_release
            
            ///std::memory_order_acquire means-all the changes other threads did before their release operation
            //(on the same atomic variable) (published with memory_order_release)—are visible to me after this line. 
            //It becomes synchronization barrier—nothing after this line in current 
            //thread can be reordered to run before it.  
            /*
            Thread A (Writer)        Thread B (Reader)
            Write X ────────┐        ┌─── "Show me everything before the release!"
            Write Y         │        │
            [release] Store ─────────┘
                │        │
                ▼        ▼
            Thread B's `acquire` sees X, Y
            */
            /*********************Example*********************************
            int data;  // Non-atomic
            std::atomic<bool> ready{false};

           // Thread A (Publisher):
           data = 42;  // (1) Non-atomic write
           ready.store(true, std::memory_order_release);  // (2) Publish

           // Thread B (Consumer):
           if (ready.load(std::memory_order_acquire)) {  // (3) Syncs-with (2)
                assert(data == 42);  // (4) Guaranteed
           }**********************************************************/

            if(head.compare_exchange_weak(expected_head, new_node, 
                    std::memory_order_release, // (C) =>// (C) publishes the changes made in (A) and (B).
                                               //publishes:
                                                //* new_node->data
                                                //* new_node->next
                                                 // After this release CAS succeeds, any thread that later
                                                 // reads head with memory_order_acquire is guaranteed to see above writes.
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
  
    //:::TIPS: acquire->relaxed->acquire->relaxed ::::::
    bool pop(T& out) {

       while (true) {   
        
        //1. memory_order:
        //1.1. We can use std::memory_order_relaxed and CAS will give correct old_head but if start with relaxed load, we are almost guranteeing that
        //   our first CAS attempt will fail. Failing a CAS is expensive.
        //1.2. Also we are dereferencing old_head to get old_head->next. 
        //   Without acquire we may get pointer but not content it points to (the next value), leading to a crash or garbage data.
        //   As we access old_head->next, we should pair with push()'s CAS i.e. Any writes before successful push()'s CAS 
        //   (including data & next) are visible after this acquire
        //   So, whole point for using memory_order_acquire is accessing valid old_head->next not just old_head
 
            Node* old_head = head.load(std::memory_order_acquire);
            if (!old_head) 
              return false; 

            //This is not safe without hazard protection.
            //Another thread can pop and delete old_head
            //if (old_head) __builtin_prefetch(old_head->next.load(std::memory_order_relaxed), 0, 1);
         
            
            //C++ standard says: If operation A happens-before B, and B happens-before C, then A happens-before C.
            // The next pointer was written before the release CAS in push(),
            // so once we acquire the head pointer here, we are guaranteed to
            // observe a fully initialized node (including next and data).
            //->next is fully initialised BEFORE the release CAS that published new_node in Push(). 
            //Once pop() does acquire on head (at (D) above or on CAS success below[CAS gurantees that we get correct old_head]), 
            //it is guranteed to see correct value of next.
            // No additional acquire is needed for next because it is already
            // safely published by the release operation in push().
            Node* new_head = old_head->next.load(std::memory_order_relaxed); //(E-1)

            // If CAS succeeds, we have removed old_head from the stack.
            // acquire ensures we see the node contents (data and next)
            // published by push()'s release CAS.
            if (head.compare_exchange_weak(old_head, new_head, 
                    std::memory_order_acquire, //(F-a) On Success: acquire: Need to acquire and see node->data released by other
                                               //release: We are "removing" not publishing node->data or fresh data for other threads, 
                                               //so no need for memory_order_release
                    std::memory_order_relaxed)) //(F-b) // On failure, CAS updates old_head with the current head value,
                                                        // allowing the loop to retry with the latest state.
             {
                out = old_head->data;
                //delete old_head; //We cant delete now as other threads might have references to it. 
                // NOTE:
                // This implementation intentionally does NOT reclaim nodes.
                // Therefore:
                //   - ABA protection is not yet implemented.
                //   - Nodes are never deleted after pop().
                //   - Memory usage grows indefinitely.
                //   - This avoids use-after-free while the lock-free algorithm
                //     is being developed incrementally.
                //
                // Proper production implementation requires:
                //   1. ABA protection (tagged pointers)
                //   2. Memory reclamation (hazard pointers / Epoch-Based Reclamation (EBR) )
                return true;
             }
        }
        return false;
    }

    // Fast empty check (relaxed, may be stale)
    bool empty() const {
        return head.load(std::memory_order_acquire) == nullptr;
    }

    //Single threaded when all other threads have joined and stopped using stack. So, memory_order_relaxed
    ~LockFreeTeiberMPMCStack() {
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
