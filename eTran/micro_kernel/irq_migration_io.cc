#include "irq_migration_io.h"
#include "nic.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using StatIndices = std::array<int, 2>;
constexpr const char *counters[] = {"aff_handoff_done", "aff_handoff_timeout"};

int read_cpu(const std::string &file)
{
    std::ifstream input(file);
    std::string mask;
    if (!std::getline(input, mask))
        return -1;
    int cpu = -1;
    const auto end = mask.data() + mask.size();
    const auto parsed = std::from_chars(mask.data(), end, cpu);
    return parsed.ec == std::errc{} && parsed.ptr == end ? cpu : -1;
}

int ethtool_ioctl(int fd, const std::string &ifname, void *data)
{
    struct ifreq request{};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", ifname.c_str());
    request.ifr_data = static_cast<char *>(data);
    return ioctl(fd, SIOCETHTOOL, &request) < 0 ? -errno : 0;
}

// The management worker is the sole caller. NIC channel count is fixed while it runs.
struct ChannelStats {
    unsigned int count = 0;
    std::vector<StatIndices> indices;
    std::vector<uint64_t> data;

    int load_names(int fd, const eTranNIC &nic)
    {
        ethtool_drvinfo version{};
        version.cmd = ETHTOOL_GDRVINFO;
        if (const int rc = ethtool_ioctl(fd, nic._if_name, &version))
            return rc;
        if (!version.n_stats)
            return -ENODATA;

        const size_t size = sizeof(ethtool_gstrings) + static_cast<size_t>(version.n_stats) * ETH_GSTRING_LEN;
        std::vector<uint64_t> names((size + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        auto *strings = reinterpret_cast<ethtool_gstrings *>(names.data());
        strings->cmd = ETHTOOL_GSTRINGS;
        strings->string_set = ETH_SS_STATS;
        strings->len = version.n_stats;
        if (const int rc = ethtool_ioctl(fd, nic._if_name, strings))
            return rc;

        indices.assign(nic._num_queues, StatIndices{-1, -1});
        for (unsigned int qid = 0; qid < nic._num_queues; ++qid) {
            const auto prefix = "ch" + std::to_string(qid) + '_';
            for (size_t column = 0; column < std::size(counters); ++column) {
                const auto name = prefix + counters[column];
                for (unsigned int i = 0; i < version.n_stats; ++i) {
                    const auto *entry = reinterpret_cast<const char *>(strings->data + i * ETH_GSTRING_LEN);
                    if (strncmp(entry, name.c_str(), ETH_GSTRING_LEN) == 0) {
                        indices[qid][column] = i;
                        break;
                    }
                }
            }
        }
        count = version.n_stats;
        data.resize((sizeof(ethtool_stats) + sizeof(uint64_t) * count + sizeof(uint64_t) - 1) /
                    sizeof(uint64_t));
        return 0;
    }

    int read(int fd, const eTranNIC &nic, unsigned int qid, IrqHandoffSample &sample)
    {
        if (indices.empty())
            if (const int rc = load_names(fd, nic))
                return rc;
        const auto indices_for_queue = indices[qid];
        if (indices_for_queue[0] < 0 || indices_for_queue[1] < 0)
            return -EOPNOTSUPP;
        auto *stats = reinterpret_cast<ethtool_stats *>(data.data());
        stats->cmd = ETHTOOL_GSTATS;
        stats->n_stats = count;
        if (const int rc = ethtool_ioctl(fd, nic._if_name, stats))
            return rc;
        sample.done = stats->data[indices_for_queue[0]];
        sample.timeouts = stats->data[indices_for_queue[1]];
        return 0;
    }
};

bool read_irq_cpu_counts(int irq, int source_cpu, int target_cpu,
                         uint64_t &source_count, uint64_t &target_count)
{
    if (irq < 0 || source_cpu < 0 || target_cpu < 0)
        return false;
    std::ifstream interrupts("/proc/interrupts");
    std::string line, token;
    if (!std::getline(interrupts, line))
        return false;

    std::istringstream header(line);
    std::vector<int> columns;
    while (header >> token) {
        if (token.compare(0, 3, "CPU") != 0)
            continue;
        int cpu = -1;
        const auto start = token.data() + 3;
        const auto end = token.data() + token.size();
        const auto parsed = std::from_chars(start, end, cpu);
        if (parsed.ec != std::errc{} || parsed.ptr != end || cpu < 0)
            return false;
        columns.push_back(cpu);
    }
    if (columns.empty())
        return false;

    const std::string target_label = std::to_string(irq) + ':';
    while (std::getline(interrupts, line)) {
        std::istringstream row(line);
        if (!(row >> token) || token != target_label)
            continue;
        bool found_source = false;
        bool found_target = false;
        for (int cpu : columns) {
            uint64_t value = 0;
            if (!(row >> value))
                return false;
            if (cpu == source_cpu) {
                source_count = value;
                found_source = true;
            }
            if (cpu == target_cpu) {
                target_count = value;
                found_target = true;
            }
        }
        return found_source && found_target;
    }
    return false;
}

} // namespace

int sample_irq_handoff(const eTranNIC &nic, unsigned int qid, int source_cpu, int target_cpu,
                       bool read_counters, IrqHandoffSample &sample)
{
    sample = {};
    const int irq = nic.get_queue_irq(qid);
    if (irq < 0)
        return irq;
    const auto path = "/proc/irq/" + std::to_string(irq) + '/';
    sample.configured = read_cpu(path + "smp_affinity_list");
    sample.effective = read_cpu(path + "effective_affinity_list");
    if (!read_counters)
        return 0;

    const int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return -errno;
    static ChannelStats channel_stats;
    const int error = channel_stats.read(socket_fd, nic, qid, sample);
    close(socket_fd);
    if (!error)
        sample.have_irq_counts = read_irq_cpu_counts(irq, source_cpu, target_cpu,
                                                     sample.source_irqs, sample.target_irqs);
    return error;
}
