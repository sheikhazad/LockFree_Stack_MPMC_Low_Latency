#include "LockFreeTeiberMPMCStack.hpp"
#include "LockFreeTeiberMPMCStack_ABA.hpp"
#include "LockFreeTeiberMPMCStack_EBR.hpp"
#include "LockFreeTeiberMPMCStack_HazardPointer.hpp"


 /*Optional: NUMA-aware CPU pinning function
              Since HFT workloads often run on multi-socket machines, pinning producer and 
              consumer threads to dedicated cores can reduce cache-line contention.
              */
/* This function pins a thread to a specific core based on the thread index and NUMA node.
 * It is designed for Linux systems using pthreads.
 * For Windows, use SetThreadAffinityMask() instead.
 */
void pinThreadToCore(int threadIndex, int numaNode) {
    /*
    cpu_set_t cpuset; //1
    CPU_ZERO(&cpuset); //2

    // Assign cores in round-robin within the specified NUMA node
    // Calculate core ID based on thread index and NUMA node
    // For example, if NUMA_NODE_0 has cores 0-3 and NUMA_NODE_1 has cores 4-7,
    // this will assign producers to cores 0-3 and consumers to cores 4-7.
    // This is a simple example; in practice, you may want to use a more sophisticated mapping 

    int cores_per_node = std::thread::hardware_concurrency() //3.1;
    int core_id = (threadIndex % cores_per_node) + (numaNode * cores_per_node); //3.2
    // Ensure core_id is within valid range
    assert(core_id < std::thread::hardware_concurrency()); //3.3
    CPU_SET(core_id, &cpuset); //4
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); //5
    */
}

int main() {
    LockFreeTeiberMPMCStack<int> stack;
    std::vector<std::thread> threads;
    threads.reserve(NUM_PRODUCERS + NUM_CONSUMERS);

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back([i, &stack]() { 
            pinThreadToCore(i, NUMA_NODE_0);
            for (int j = 0; j < WORKLOAD; ++j) {
                stack.push(j);
            }
        });
    }

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back([i, &stack]() { 
            pinThreadToCore(i, NUMA_NODE_1);
            int value;
            while (stack.pop(value)) { }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return 0;
}
