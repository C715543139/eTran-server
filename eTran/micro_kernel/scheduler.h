#pragma once

#include <stdint.h>
#include <array>
#include <optional>
#include <intf/capacity.h>

struct CpuTopology;
struct bpf_cc_map_user;

bool scheduler_collect_qid_stats();
bool scheduler_queue_is_light(unsigned int qid);

struct SchedulerIrqProgress {
    bool fresh;
    bool idle;
    bool quiet;
};

SchedulerIrqProgress scheduler_irq_progress(unsigned int qid, int source, int target);
void scheduler_track_irq_handoff(unsigned int qid, bool moving);

struct SchedulerPlacement {
    struct Core {
        int app = 0, irq = 0, latency = 0;
        bool quarantined = false;
    };
    std::array<Core, MAX_CPU> cores{};
    bool busy = false;
};

struct SchedulerAction {
    enum class Kind { Worker, Irq, Degraded } kind;
    enum class Reason { R1, R2, R3, R4, Merge, Expand, Fold, Capacity } reason;
    uint64_t generation;
    unsigned int slot;
    int pid, source, target;
    uint64_t worker_start = 0;
};

std::optional<SchedulerAction> scheduler_exchange_placement(const SchedulerPlacement &placement);
bool scheduler_can_merge_irq(unsigned int qid, int cpu);
bool scheduler_request_admission_fold();
int start_scheduler(const CpuTopology &topology, unsigned int queues, int napi_fd, int tw_fd,
                    int tw_pending_fd, const bpf_cc_map_user *cc);
void stop_scheduler();
void scheduler_register_queue(unsigned int qid, int pid, int slo, int irq_cpu, int app_cpu,
                              uint64_t owner, bool single_worker_queue, bool fixed_irq);
void scheduler_unregister_queue(unsigned int qid);
void scheduler_register_flow(unsigned int cc_idx, unsigned int qid);
void scheduler_unregister_flow(unsigned int cc_idx);
void scheduler_register_worker(unsigned int qid, int pid, int tid);
void scheduler_update_app_cpu(unsigned int qid, int pid, int cpu);
void scheduler_update_irq_cpu(unsigned int qid, int pid, int cpu);
void scheduler_register_worker_slot(uint64_t generation, unsigned int slot, int cpu, int slo,
                                    int pid, uint64_t start_time, bool fixed);
void scheduler_unregister_workers(uint64_t generation);
