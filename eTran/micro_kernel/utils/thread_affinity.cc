#include "thread_affinity.h"

#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <limits>
#include <memory>
#include <sched.h>
#include <string>
#include <vector>

struct thread_affinity {
    pid_t tid;
    cpu_set_t previous;
};

int set_process_thread_affinity(pid_t pid, int cpu)
{
    if (pid <= 0 || cpu < 0 || cpu >= CPU_SETSIZE)
        return -EINVAL;

    const std::string tasks = "/proc/" + std::to_string(pid) + "/task";
    const auto close_directory = [](DIR *directory) { closedir(directory); };
    std::unique_ptr<DIR, decltype(close_directory)> directory(opendir(tasks.c_str()),
                                                             close_directory);
    if (!directory)
        return errno == ENOENT ? -ESRCH : -errno;

    std::vector<thread_affinity> threads;
    for (;;) {
        errno = 0;
        const auto *entry = readdir(directory.get());
        if (!entry) {
            if (errno)
                return -errno;
            break;
        }

        char *end;
        const long number = std::strtol(entry->d_name, &end, 10);
        if (*end || number <= 0 || number > std::numeric_limits<pid_t>::max())
            continue;

        thread_affinity thread{static_cast<pid_t>(number), {}};
        if (sched_getaffinity(thread.tid, sizeof(thread.previous), &thread.previous) == 0)
            threads.push_back(thread);
        else if (errno != ESRCH)
            return -errno;
    }

    cpu_set_t target;
    CPU_ZERO(&target);
    CPU_SET(cpu, &target);
    std::vector<thread_affinity> updated;
    updated.reserve(threads.size());
    for (const auto &thread : threads) {
        if (sched_setaffinity(thread.tid, sizeof(target), &target) == 0) {
            updated.push_back(thread);
        } else if (errno != ESRCH) {
            const int error = -errno;
            bool restored = true;
            for (const auto &moved : updated) {
                if (sched_setaffinity(moved.tid, sizeof(moved.previous), &moved.previous) != 0 &&
                    errno != ESRCH)
                    restored = false;
            }
            return restored ? error : -EIO;
        }
    }
    return updated.empty() ? -ESRCH : 0;
}
