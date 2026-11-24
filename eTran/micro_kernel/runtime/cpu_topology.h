#pragma once

#include <vector>

struct CpuTopology {
    std::vector<int> online;
    std::vector<int> pool;
    int control;
};

bool read_cpu_topology(CpuTopology &topology);
