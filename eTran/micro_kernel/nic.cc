#include "nic.h"

#include <ifaddrs.h>
#include <string>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>

#include <netinet/in.h>

#include <utils/utils.h>

static int enable_napi_polling(std::string if_name)
{
    std::string cmd1 = "echo 200 > /sys/class/net/" + if_name + "/gro_flush_timeout";
    std::string cmd2 = "echo 100 > /sys/class/net/" + if_name + "/napi_defer_hard_irqs";
    if (!exec_cmd(cmd1) || !exec_cmd(cmd2))
    {
        fprintf(stderr, "Failed to enable napi polling\n");
        return -1;
    }
    return 0;
}

static void disable_napi_polling(std::string if_name)
{
    std::string cmd1 = "echo 0 > /sys/class/net/" + if_name + "/gro_flush_timeout";
    std::string cmd2 = "echo 0 > /sys/class/net/" + if_name + "/napi_defer_hard_irqs";
    if (!exec_cmd(cmd1) || !exec_cmd(cmd2))
    {
        fprintf(stderr, "Failed to enable napi polling\n");
        exit(EXIT_FAILURE);
    }
    printf("disable napi polling\n");
}

std::string eTranNIC::queue_irq_name(unsigned int qid) const
{
    return "mlx5_comp" + std::to_string(qid + 1) + "@pci:" + pcie_name;
}

std::array<int, MAX_NIC_QUEUES> eTranNIC::read_queue_irqs() const
{
    std::array<int, MAX_NIC_QUEUES> irqs{};
    std::array<std::string, MAX_NIC_QUEUES> names;
    for (unsigned int qid = 0; qid < _num_queues; ++qid)
        names[qid] = queue_irq_name(qid);

    std::ifstream interrupts("/proc/interrupts");
    std::string line;
    while (std::getline(interrupts, line)) {
        const size_t name_end = line.find_last_not_of(" \t");
        if (name_end == std::string::npos)
            continue;
        const size_t name_start = line.find_last_of(" \t", name_end);
        if (name_start == std::string::npos)
            continue;
        const auto end = names.begin() + _num_queues;
        const auto name = std::find(names.begin(), end,
                                    line.substr(name_start + 1, name_end - name_start));
        if (name == end)
            continue;
        std::istringstream fields(line);
        int irq;
        char colon;
        if (fields >> irq >> colon && irq > 0 && colon == ':')
            irqs[name - names.begin()] = irq;
    }
    return irqs;
}

int eTranNIC::get_queue_irq(unsigned int qid) const
{
    if (qid >= _num_queues)
        return -EINVAL;

    const int cached_irq = _queue_irqs[qid];
    if (cached_irq > 0) {
        std::ifstream actions("/sys/kernel/irq/" + std::to_string(cached_irq) + "/actions");
        std::string name;
        if (std::getline(actions, name) && name == queue_irq_name(qid))
            return cached_irq;
    }

    const int irq = read_queue_irqs()[qid];
    return irq > 0 ? irq : -ENOENT;
}

int eTranNIC::set_queue_cpu(unsigned int qid, int cpu) const
{
    const auto &online = _cpu_topology.online;
    if (!std::binary_search(online.begin(), online.end(), cpu))
        return -EINVAL;
    const int irq = get_queue_irq(qid);
    if (irq < 0)
        return irq;

    const std::string path = "/proc/irq/" + std::to_string(irq) + "/smp_affinity_list";
    FILE *file = fopen(path.c_str(), "w");
    if (!file)
        return -errno;
    int written = fprintf(file, "%d\n", cpu);
    int closed = fclose(file);
    return written < 0 || closed != 0 ? -EIO : 0;
}

static int check_nic(std::string if_name, unsigned int num_queues, std::string &pcie_name)
{
    std::string cmd;
    std::string res;

    cmd = "ethtool -i " + if_name + " | grep 'bus-info' | awk '{print $2}'";
    pcie_name.clear();
    exec_cmd(cmd, pcie_name);
    if (pcie_name.empty())
    {
        fprintf(stderr, "Failed to get PCIE device name for %s\n", if_name.c_str());
        return -1;
    }
    pcie_name.erase(std::remove(pcie_name.begin(), pcie_name.end(), '\n'), pcie_name.end());

    cmd = "ethtool -l " + if_name + " | grep 'Combined' | awk 'NR==1 {print $2}'";
    res.clear();
    exec_cmd(cmd, res);
    unsigned int nic_queues = std::stoi(res);
    if (num_queues > nic_queues)
    {
        fprintf(stderr, "Number of queues is greater than NIC queues (%u > %d)\n", num_queues, nic_queues);
        return -1;
    }
    return 0;
}

/* Get the local IP address of the interface */
static uint32_t getIPAddress(const std::string &if_name)
{
    struct ifaddrs *ifaddr, *ifa;
    uint32_t ip_address = 0; // Default to 0, which could be considered an error value (0.0.0.0).

    if (getifaddrs(&ifaddr) == -1)
    {
        fprintf(stderr, "getifaddrs");
        return ip_address;
    }

    // Walk through the linked list of interfaces
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr)
            continue;

        // Check for IPv4 family
        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            if (if_name == ifa->ifa_name)
            {
                struct sockaddr_in *ipv4 = (struct sockaddr_in *)ifa->ifa_addr;
                ip_address = ntohl(ipv4->sin_addr.s_addr); // Network to host long
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return ip_address;
}

int eTranNIC::create_nic(void)
{
    std::string cmd;
    std::string res;
    if (check_nic(_if_name, _num_queues, pcie_name))
        return -1;

    /* set the number of NIC queues */
    cmd = "ethtool -L " + _if_name + " combined " + std::to_string(_num_queues);
    if (!exec_cmd(cmd))
    {
        fprintf(stderr, "Failed to configure NIC queues\n");
        return -1;
    }

    /* set the queue length */
    cmd = "ethtool -G " + _if_name + " rx " + std::to_string(_queue_len) + " tx " + std::to_string(_queue_len);
    if (!exec_cmd(cmd))
    {
        fprintf(stderr, "Failed to configure NIC queues\n");
        return -1;
    }

    _queue_irqs = read_queue_irqs();

    if (_intr_affinity)
    {
        /* disable irqbalance */
        cmd = "if pgrep -x irqbalance > /dev/null; then killall irqbalance; fi";
        if (!exec_cmd(cmd))
        {
            fprintf(stderr, "Failed to kill irqbalance\n");
            return -1;
        }
        /* set NIC interrupt affinity */
        const auto &pool = _cpu_topology.pool;
        for (unsigned int qid = 0; qid < _num_queues; ++qid) {
            if (int err = set_queue_cpu(qid, pool[qid % pool.size()])) {
                fprintf(stderr, "Failed to set affinity for queue %u: %s\n",
                        qid, strerror(-err));
                return -1;
            }
        }
    }

    if (!_coalescing)
    {
        /* disable NIC coalescing */
        cmd = "ethtool -C " + _if_name + " adaptive-rx off rx-usecs 5 rx-frames 1";
        if (!exec_cmd(cmd))
        {
            fprintf(stderr, "Failed to configure NIC intrs\n");
            return -1;
        }
    }

    if (_napi_polling)
    {
        if (enable_napi_polling(_if_name))
            return -1;
    }

    _local_ip = getIPAddress(_if_name);
    if (_local_ip == 0)
    {
        fprintf(stderr, "Failed to get local IP address\n");
        return -1;
    }

    return 0;
}

void eTranNIC::destroy_nic(void)
{
    if (_napi_polling)
        disable_napi_polling(_if_name);
}
