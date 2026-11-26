#pragma once

#include <stdint.h>

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
int start_scheduler(const CpuTopology &topology, unsigned int queues, int napi_fd, int tw_fd,
                    int tw_pending_fd, const bpf_cc_map_user *cc);
void stop_scheduler();
void scheduler_register_queue(unsigned int qid, int pid, int slo, int irq_cpu, int app_cpu);
void scheduler_unregister_queue(unsigned int qid);
void scheduler_register_flow(unsigned int cc_idx, unsigned int qid);
void scheduler_unregister_flow(unsigned int cc_idx);
void scheduler_register_worker(unsigned int qid, int pid, int tid);
void scheduler_update_app_cpu(unsigned int qid, int pid, int cpu);
void scheduler_update_irq_cpu(unsigned int qid, int pid, int cpu);
void scheduler_register_worker_slot(uint64_t generation, unsigned int slot, int cpu, int slo);
void scheduler_unregister_workers(uint64_t generation);
