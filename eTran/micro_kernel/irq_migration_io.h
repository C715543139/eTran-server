#pragma once

#include <cstdint>

class eTranNIC;

struct IrqHandoffSample {
    int configured = -1;
    int effective = -1;
    uint64_t done = 0;
    uint64_t timeouts = 0;
};

int sample_irq_handoff(const eTranNIC &nic, unsigned int qid, bool read_counters, IrqHandoffSample &sample);
