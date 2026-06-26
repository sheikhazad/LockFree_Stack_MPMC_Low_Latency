#pragma once
#include <new> // std::hardware_destructive_interference_size

#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_destructive_interference_size;
#else
    static constexpr size_t hardware_destructive_interference_size = 64;
#endif

constexpr int NUM_PRODUCERS = 4;
constexpr int NUM_CONSUMERS = 4;
constexpr int WORKLOAD = 1000;
constexpr int NUMA_NODE_0 = 0;
constexpr int NUMA_NODE_1 = 1;

constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;
