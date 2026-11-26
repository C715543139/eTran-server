#include "scheduler.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <pthread.h>
#include <sched.h>

#include <runtime/cpu_topology.h>
#include <intf/intf_ebpf.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto sample_period = std::chrono::milliseconds(50);

struct Settings {
    const char *mode = std::getenv("ETRAN_SCHED_MODE");
    const char *qid = std::getenv("ETRAN_QID_STATS");
    const char *probe = std::getenv("ETRAN_SOFTIRQ_PROBE");
    bool shadow = mode && strcmp(mode, "shadow") == 0;
    bool probe_enabled = probe && strcmp(probe, "1") == 0;
    bool qid_enabled = shadow || probe_enabled || (qid && strcmp(qid, "1") == 0);
};

const Settings &settings()
{
    static const Settings config;
    return config;
}

struct SoftirqSample {
    bpf_softirq_stats total;
    __u64 rx_entry_ns;
    __u64 tx_entry_ns;
};

struct SoftirqProbe {
    bpf_object *object = nullptr;
    std::array<bpf_link *, 2> links{};
    int map_fd = -1;

    int attach()
    {
        auto *obj = bpf_object__open_file("eBPF/sched/softirq_monitor.o", nullptr);
        if (int err = libbpf_get_error(obj))
            return err;
        object = obj;
        if (int err = bpf_object__load(object))
            return err;
        const char *programs[] = {"track_softirq_entry", "track_softirq_exit"};
        const char *events[] = {"softirq_entry", "softirq_exit"};
        for (size_t i = 0; i < links.size(); ++i) {
            auto *program = bpf_object__find_program_by_name(object, programs[i]);
            if (!program)
                return -ENOENT;
            auto *link = bpf_program__attach_raw_tracepoint(program, events[i]);
            if (int err = libbpf_get_error(link))
                return err;
            links[i] = link;
        }
        map_fd = bpf_object__find_map_fd_by_name(object, "softirq_samples");
        return map_fd < 0 ? map_fd : 0;
    }

    ~SoftirqProbe()
    {
        for (auto *link : links)
            bpf_link__destroy(link);
        bpf_object__close(object);
    }
};

struct Registration {
    uint64_t generation = 0;
    int pid = 0;
    int slo = 0;
    int irq_cpu = -1;
    int app_cpu = -1;
};

enum class Profile { Unknown, Light, Heavy };

const char *profile_name(Profile state)
{
    switch (state) {
    case Profile::Unknown: return "UNKNOWN";
    case Profile::Light: return "LIGHT";
    case Profile::Heavy: return "HEAVY";
    }
    return "UNKNOWN";
}

struct QueueSample {
    uint64_t generation = 0;
    bpf_qid_stats previous{};
    Clock::time_point since{};
    Clock::time_point last{};
    Clock::time_point last_log{};
    Profile profile = Profile::Unknown;
    unsigned int high = 0, low = 0, idle = 0;
    double bps = 0, pps = 0;
};

std::array<Registration, MAX_NIC_QUEUES> registrations;
std::mutex registration_mutex;
std::thread sampler;
std::condition_variable sampler_wake;
std::mutex sampler_mutex;
bool stopping = false;
std::unique_ptr<SoftirqProbe> probe;

bpf_qid_stats sum_qid_stats(const std::vector<bpf_qid_stats> &values)
{
    bpf_qid_stats total{};
    for (const auto &cpu : values) {
        total.rx_xsk_pkts += cpu.rx_xsk_pkts;
        total.rx_xsk_bytes += cpu.rx_xsk_bytes;
        total.tx_direct_pkts += cpu.tx_direct_pkts;
        total.tx_direct_bytes += cpu.tx_direct_bytes;
        total.tw_enq_attempts += cpu.tw_enq_attempts;
        total.tw_sent_pkts += cpu.tw_sent_pkts;
        total.tw_sent_bytes += cpu.tw_sent_bytes;
        total.tw_drop_pkts += cpu.tw_drop_pkts;
    }
    return total;
}

void sample_queues(unsigned int queues, int fd, int cpus, int softirq_fd)
{
    std::array<QueueSample, MAX_NIC_QUEUES> queue_state{};
    std::vector<bpf_qid_stats> per_cpu(cpus);
    std::vector<SoftirqSample> softirq(softirq_fd >= 0 ? cpus : 0);
    auto last_probe = softirq;
    auto now = Clock::now();
    auto last_tick = now;
    bool probe_baseline = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(sampler_mutex);
            if (sampler_wake.wait_until(lock, now + sample_period, [] { return stopping; }))
                break;
        }
        now = Clock::now();
        const double tick_ns = std::chrono::duration<double, std::nano>(now - last_tick).count();
        last_tick = now;
        const __u32 zero = 0;
        const bool have_probe = softirq_fd >= 0 && !bpf_map_lookup_elem(softirq_fd, &zero, softirq.data());
        std::array<Registration, MAX_NIC_QUEUES> snapshot;
        {
            std::lock_guard<std::mutex> lock(registration_mutex);
            snapshot = registrations;
        }
        for (__u32 qid = 0; qid < queues && fd >= 0; ++qid) {
            const auto &app = snapshot[qid];
            auto &state = queue_state[qid];
            if (!app.pid)
                continue;
            if (bpf_map_lookup_elem(fd, &qid, per_cpu.data())) {
                state.profile = Profile::Unknown;
                state.last = {};
                continue;
            }
            const auto total = sum_qid_stats(per_cpu);
            if (state.generation != app.generation || state.last == Clock::time_point{}) {
                state = {};
                state.generation = app.generation;
                state.previous = total;
                state.last = state.since = state.last_log = now;
                continue;
            }

            const auto &prev = state.previous;
            const double hz = 1.0 / std::chrono::duration<double>(now - state.last).count();
            const auto bytes = total.rx_xsk_bytes - prev.rx_xsk_bytes +
                               total.tx_direct_bytes - prev.tx_direct_bytes +
                               total.tw_sent_bytes - prev.tw_sent_bytes;
            const auto packets = total.rx_xsk_pkts - prev.rx_xsk_pkts +
                                 total.tx_direct_pkts - prev.tx_direct_pkts +
                                 total.tw_sent_pkts - prev.tw_sent_pkts;
            state.bps = 0.6 * state.bps + 0.4 * (bytes * 8.0 * hz);
            state.pps = 0.6 * state.pps + 0.4 * (packets * hz);
            const bool paced = (total.tw_enq_attempts - prev.tw_enq_attempts) * hz > 1000 ||
                               (total.tw_sent_pkts - prev.tw_sent_pkts) * hz > 1000;
            const int cpu = app.irq_cpu;
            double rx = -1, tx = -1;
            if (have_probe && probe_baseline && cpu >= 0 && cpu < cpus) {
                rx = (softirq[cpu].total.net_rx_ns - last_probe[cpu].total.net_rx_ns) / tick_ns;
                tx = (softirq[cpu].total.net_tx_ns - last_probe[cpu].total.net_tx_ns) / tick_ns;
            }
            const bool only_irq = std::count_if(snapshot.begin(), snapshot.end(), [cpu](const auto &entry) {
                return entry.pid && entry.irq_cpu == cpu;
            }) == 1;
            const bool heavy = state.bps >= 3e9 || state.pps >= 350000 ||
                               (only_irq && rx + tx >= 0.35);
            const bool low = state.bps <= 1.5e9 && state.pps <= 200000 &&
                             (!have_probe || (rx >= 0 && rx + tx <= 0.20));
            const bool idle = state.bps < 1e6 && state.pps < 1000;
            const bool suspicious = idle && (total.tw_drop_pkts != prev.tw_drop_pkts || rx + tx > 0.20);
            state.high = heavy ? state.high + 1 : 0;
            state.low = low ? state.low + 1 : 0;
            state.idle = idle ? state.idle + 1 : 0;
            const auto before = state.profile;
            if (state.high >= 2)
                state.profile = Profile::Heavy;
            else if (suspicious || now - state.since < std::chrono::milliseconds(200))
                state.profile = Profile::Unknown;
            else if (state.profile == Profile::Heavy) {
                if (state.low >= 6)
                    state.profile = Profile::Light;
            } else if (!idle && !heavy) {
                state.profile = Profile::Light;
            }
            // Keep quiescent queues UNKNOWN until the additional progress sources are wired.
            if (idle && state.idle >= 10 && state.profile != Profile::Heavy)
                state.profile = Profile::Unknown;
            if (before != state.profile || now - state.last_log >= std::chrono::seconds(1)) {
                fprintf(stderr, "sched_sample qid=%u pid=%d slo=%d irq_cpu=%d app_cpu=%d profile=%s bps=%.0f pps=%.0f paced=%d net_rx=%.4f net_tx=%.4f rx_pkts=%llu tx_direct_pkts=%llu tw_attempts=%llu tw_submit=%llu tw_drop=%llu\n",
                        qid, app.pid, app.slo, cpu, app.app_cpu, profile_name(state.profile), state.bps, state.pps, paced, rx, tx,
                        total.rx_xsk_pkts, total.tx_direct_pkts, total.tw_enq_attempts, total.tw_sent_pkts, total.tw_drop_pkts);
                state.last_log = now;
            }
            state.previous = total;
            state.last = now;
        }
        if (have_probe)
            last_probe.swap(softirq);
        probe_baseline = have_probe;
    }
}
} // namespace

bool scheduler_collect_qid_stats()
{
    return settings().qid_enabled;
}

void scheduler_register_queue(unsigned int qid, int pid, int slo, int irq_cpu, int app_cpu)
{
    if (!scheduler_collect_qid_stats())
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto &entry = registrations[qid];
    entry = {entry.generation + 1, pid, slo, irq_cpu, app_cpu};
}

void scheduler_unregister_queue(unsigned int qid)
{
    if (!scheduler_collect_qid_stats())
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    registrations[qid].pid = 0;
}

int start_scheduler(const CpuTopology &topology, unsigned int queues, int qid_stats_fd)
{
    const auto &config = settings();
    if (config.mode && strcmp(config.mode, "off") && !config.shadow) {
        fprintf(stderr, "ETRAN_SCHED_MODE must be off or shadow during telemetry validation\n");
        return -EINVAL;
    }
    if (!config.qid_enabled && !config.probe_enabled)
        return 0;

    int cpu = topology.control;
    if (const char *requested = std::getenv("ETRAN_SCHED_CPU")) {
        char *end = nullptr;
        errno = 0;
        const long parsed = strtol(requested, &end, 10);
        if (errno || end == requested || *end || parsed < 0 || parsed >= CPU_SETSIZE ||
            std::find(topology.online.begin(), topology.online.end(), parsed) == topology.online.end() ||
            std::find(topology.pool.begin(), topology.pool.end(), parsed) != topology.pool.end() ||
            parsed == topology.control) {
            fprintf(stderr, "ETRAN_SCHED_CPU must be online and outside the data and control CPU sets\n");
            return -EINVAL;
        }
        cpu = static_cast<int>(parsed);
    } else {
        fprintf(stderr, "Scheduler sampling shares control-plane CPU %d\n", cpu);
    }

    const int possible = libbpf_num_possible_cpus();
    if (possible < 1)
        return -EINVAL;
    if (config.probe_enabled) {
        auto candidate = std::make_unique<SoftirqProbe>();
        if (int err = candidate->attach()) {
            fprintf(stderr, "Cannot attach softirq probe: %s\n", strerror(-err));
            return err;
        }
        probe = std::move(candidate);
    }
    {
        std::lock_guard<std::mutex> lock(sampler_mutex);
        stopping = false;
    }
    try {
        sampler = std::thread([=] {
            sample_queues(queues, config.qid_enabled ? qid_stats_fd : -1, possible, probe ? probe->map_fd : -1);
        });
    } catch (const std::system_error &err) {
        probe.reset();
        return -err.code().value();
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (int err = pthread_setaffinity_np(sampler.native_handle(), sizeof(cpuset), &cpuset)) {
        stop_scheduler();
        return -err;
    }
    fprintf(stderr, "sched_start mode=%s cpu=%d qid=%d softirq=%d\n",
            config.shadow ? "shadow" : "off", cpu, config.qid_enabled, config.probe_enabled);
    return 0;
}

void stop_scheduler()
{
    {
        std::lock_guard<std::mutex> lock(sampler_mutex);
        stopping = true;
    }
    sampler_wake.notify_all();
    if (sampler.joinable())
        sampler.join();
    probe.reset();
}
