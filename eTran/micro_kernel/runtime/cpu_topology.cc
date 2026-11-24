#include "cpu_topology.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <intf/capacity.h>

static bool parse_cpu(const char *&next, int limit, int &cpu)
{
    if (*next < '0' || *next > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long id = std::strtoul(next, &end, 10);
    if (errno || id > static_cast<unsigned long>(limit))
        return false;
    cpu = static_cast<int>(id);
    next = end;
    return true;
}

static bool parse_cpu_list(const std::string &list, int limit, std::vector<int> &cpus)
{
    const char *next = list.c_str();
    do {
        int first, last;
        if (!parse_cpu(next, limit, first))
            return false;
        last = first;
        if (*next == '-') {
            ++next;
            if (!parse_cpu(next, limit, last) || last < first)
                return false;
        }
        for (int cpu = first; cpu <= last; ++cpu)
            cpus.push_back(cpu);
        if (*next == '\0')
            break;
        if (*next != ',')
            return false;
        ++next;
    } while (true);

    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    return true;
}

static bool read_cpu_list(const char *path, int limit, std::vector<int> &cpus)
{
    std::ifstream file(path);
    std::string list;
    if (std::getline(file, list) && parse_cpu_list(list, limit, cpus))
        return true;
    fprintf(stderr, "Cannot read CPU IDs up to %d from %s\n", limit, path);
    return false;
}

bool read_cpu_topology(CpuTopology &topology)
{
    std::vector<int> possible;
    if (!read_cpu_list("/sys/devices/system/cpu/possible", INT_MAX - 1, possible) ||
        !read_cpu_list("/sys/devices/system/cpu/online", MAX_CPU - 1, topology.online))
        return false;
    if (!std::includes(possible.begin(), possible.end(), topology.online.begin(), topology.online.end())) {
        fprintf(stderr, "Online CPU list contains IDs outside the possible CPU list\n");
        return false;
    }

    topology.control = topology.online.back();
    if (const char *control = std::getenv("ETRAN_CP_CPU")) {
        std::vector<int> cpus;
        if (!parse_cpu_list(control, MAX_CPU - 1, cpus) || cpus.size() != 1 ||
            !std::binary_search(topology.online.begin(), topology.online.end(), cpus.front())) {
            fprintf(stderr, "ETRAN_CP_CPU must select one online CPU\n");
            return false;
        }
        topology.control = cpus.front();
    }

    if (const char *pool = std::getenv("ETRAN_CPU_POOL")) {
        if (!parse_cpu_list(pool, MAX_CPU - 1, topology.pool) ||
            !std::includes(topology.online.begin(), topology.online.end(),
                           topology.pool.begin(), topology.pool.end())) {
            fprintf(stderr, "ETRAN_CPU_POOL must be a list of online CPUs\n");
            return false;
        }
    } else {
        topology.pool = topology.online;
    }
    auto control = std::lower_bound(topology.pool.begin(), topology.pool.end(), topology.control);
    if (control != topology.pool.end() && *control == topology.control)
        topology.pool.erase(control);
    if (topology.pool.empty()) {
        fprintf(stderr, "No data-plane CPUs are available after reserving CPU %d\n", topology.control);
        return false;
    }
    return true;
}
