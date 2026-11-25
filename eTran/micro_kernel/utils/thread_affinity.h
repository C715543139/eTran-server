#pragma once

#include <sys/types.h>

int set_process_thread_affinity(pid_t pid, int cpu);
