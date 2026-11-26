#pragma once

struct CpuTopology;

bool scheduler_collect_qid_stats();
int start_scheduler(const CpuTopology &topology, unsigned int queues, int qid_stats_fd);
void stop_scheduler();
void scheduler_register_queue(unsigned int qid, int pid, int slo, int irq_cpu, int app_cpu);
void scheduler_unregister_queue(unsigned int qid);
