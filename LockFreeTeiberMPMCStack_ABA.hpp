
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
#include <type_traits> //For std::is_trivially_copyable_v<TaggedPtrABA>

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


template <typename T>
class LockFreeTeiberMPMCStackABA {
private:
    struct alignas(CACHE_LINE_SIZE) Node 
    {
        T data;
        std::atomic<Node*> next;
        explicit Node(T const& value) : data(value), next(nullptr) {}
    };

    //This is minimum enhancement for ABA with changes/addition from ABA-1 to ABA-9
    //ABA-1: Addition
    struct alignas(16) TaggedPtrABA
    {
        Node* ptr;
        uint64_t tag;
    
        TaggedPtrABA(Node* p = nullptr, uint64_t t = 0)
            : ptr(p), tag(t)
        {}
    };

    //None of these static_assert is required but for defensive programming only. 
    //You can ignore these
    static_assert(sizeof(TaggedPtrABA) == 16);
    static_assert(alignof(TaggedPtrABA) == 16);
    static_assert(std::is_trivially_copyable_v<TaggedPtrABA>);
    static_assert(std::atomic<TaggedPtrABA>::is_always_lock_free);

    //assert() is wrong in class scope
    //assert(_head.is_lock_free());

    //ABA-2: Replace Node* with TaggedPtrABA
    //alignas(CACHE_LINE_SIZE) std::atomic<Node*> _head{nullptr};  
    alignas(CACHE_LINE_SIZE) std::atomic<TaggedPtrABA> _head{ TaggedPtrABA{nullptr, 0} };
    
public:
    LockFreeTeiberMPMCStackABA(const LockFreeTeiberMPMCStackABA&) = delete;
    LockFreeTeiberMPMCStackABA& operator=(const LockFreeTeiberMPMCStackABA&) = delete;
    LockFreeTeiberMPMCStackABA(LockFreeTeiberMPMCStackABA&&) = delete;
    //LockFreeTeiberMPMCStackABA(LockFreeTeiberMPMCStackABA&& other) noexcept : _head(std::move(other._head)) { }        
    LockFreeTeiberMPMCStackABA& operator=(LockFreeTeiberMPMCStackABA&&) = delete;

    LockFreeTeiberMPMCStackABA() = default;
    
    //:::TIPS: All memory_order_relaxed except CAS success = memory_order_release ::::::
    void push(T const& value) {
        Node* new_node = new Node(value);
        
        //ABA-3: Replace Node* with TaggedPtrABA
        //Node* expected_head = _head.load(std::memory_order_relaxed); //(A)
        TaggedPtrABA expected_head = _head.load(std::memory_order_relaxed);

      
        //while(expected_head) ==> wont enter loop if the stack is empty (_head == nullptr)
        while(true){
          
            //ABA-4: Replace Node* with TaggedPtrABA
            //new_node->next.store(expected_head, std::memory_order_relaxed); //(B)
            new_node->next.store(expected_head.ptr, std::memory_order_relaxed);

          
           //ABA-5: Replace Node* with TaggedPtrABA
            /*
            if(_head.compare_exchange_weak(expected_head, new_node, 
                    std::memory_order_release, 
                   std::memory_order_relaxed) ) 
          */
            TaggedPtrABA desired(
                new_node,
                expected_head.tag + 1
            );
            
            if(_head.compare_exchange_weak(expected_head, desired,
                                       std::memory_order_release,
                                       std::memory_order_relaxed))
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

        while (true) 
        {
            //ABA-6: Replace Node* with TaggedPtrABA
            //Node* old_head = _head.load(std::memory_order_acquire); 
            TaggedPtrABA old_head = _head.load(std::memory_order_acquire);   

            //ABA-7: Replace Node* with Node* inside TaggedPtrABA
            //if (!old_head) 
            if (!old_head.ptr) 
              return false;           
          
            //ABA-8: Replace Node* with TaggedPtrABA
            //Node* new_head = old_head->next.load(std::memory_order_relaxed); //(E-1)
            Node* next = old_head.ptr->next.load(std::memory_order_relaxed);
            TaggedPtrABA new_head(
                next,
                old_head.tag + 1
            );

            if (_head.compare_exchange_weak(old_head, new_head, 
                    std::memory_order_acquire, 
                    std::memory_order_relaxed)) 
            {

              
                //ABA-9: Replace Node* with Node* inside TaggedPtrABA
                //out = old_head->data;
                out = old_head.ptr->data;
              
              
                //delete old_head.ptr; //We cant delete now as other threads might have references to it. 
                return true;
             }

           CPU_RELAX();
        }
        return false;
    }

    // Fast empty check (relaxed, may be stale)
    bool empty() const {
        //ABA-10:
        //return _head.load(std::memory_order_acquire) == nullptr;
        return _head.load(std::memory_order_acquire).ptr == nullptr;
    }

    //Single threaded when all other threads have joined and stopped using stack. So, memory_order_relaxed
    ~LockFreeTeiberMPMCStackABA() {
        //ABA-11:
        //Node* current = _head.exchange(nullptr, std::memory_order_relaxed);
        TaggedPtrABA cur = _head.exchange(TaggedPtrABA{nullptr, 0}, std::memory_order_relaxed);
        Node* current = cur.ptr;
      
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
        
            Node* expected_head = _head.load(std::memory_order_relaxed);
        
            while (true)
            {
                // Attach existing stack after our chain
                last->next.store(expected_head, std::memory_order_relaxed);
        
                if (_head.compare_exchange_weak(
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
