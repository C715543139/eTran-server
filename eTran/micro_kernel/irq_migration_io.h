#pragma once

#include <cstdint>

class eTranNIC;

struct IrqHandoffSample {
    int configured = -1;
    int effective = -1;
    uint64_t done = 0;
    uint64_t timeouts = 0;
    uint64_t source_irqs = 0;
    uint64_t target_irqs = 0;
    bool have_irq_counts = false;
};

int sample_irq_handoff(const eTranNIC &nic, unsigned int qid, int source_cpu, int target_cpu,
                       bool read_counters, IrqHandoffSample &sample);
