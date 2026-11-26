#include "scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <pthread.h>
#include <sched.h>

#include <runtime/cpu_topology.h>
#include <intf/intf_ebpf.h>
#include <intf/intf.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto sample_period = std::chrono::milliseconds(50);

struct Settings {
    const char *mode = std::getenv("ETRAN_SCHED_MODE");
    const char *qid = std::getenv("ETRAN_QID_STATS");
    const char *probe = std::getenv("ETRAN_SOFTIRQ_PROBE");
    const char *verbose_flag = std::getenv("ETRAN_SCHED_VERBOSE");
    const char *sample_flag = std::getenv("ETRAN_QID_STATS_SAMPLE");
    bool shadow = mode && strcmp(mode, "shadow") == 0;
    bool admit_only = mode && strcmp(mode, "admit_only") == 0;
    bool sample = !sample_flag || strcmp(sample_flag, "0") != 0;
    bool probe_enabled = probe && strcmp(probe, "1") == 0;
    bool qid_enabled = shadow || admit_only || (qid && strcmp(qid, "1") == 0);
    bool verbose = shadow || (verbose_flag && strcmp(verbose_flag, "1") == 0);
};

struct Thresholds {
    double b_high = 3e9, b_low = 1.5e9;
    double p_high = 350000, p_low = 200000;
    double s_high = 0.65, s_low = 0.55;

    bool load()
    {
        const char *names[] = {"ETRAN_SCHED_B_HIGH", "ETRAN_SCHED_B_LOW", "ETRAN_SCHED_P_HIGH",
                               "ETRAN_SCHED_P_LOW", "ETRAN_SCHED_S_HIGH", "ETRAN_SCHED_S_LOW"};
        double *values[] = {&b_high, &b_low, &p_high, &p_low, &s_high, &s_low};
        for (size_t i = 0; i < std::size(names); ++i) {
            const char *text = std::getenv(names[i]);
            if (!text)
                continue;
            char *end = nullptr;
            errno = 0;
            double value = strtod(text, &end);
            if (errno || end == text || *end || !std::isfinite(value) || value < 0) {
                fprintf(stderr, "Invalid scheduler threshold %s\n", names[i]);
                return false;
            }
            *values[i] = value;
        }
        return b_high > b_low && p_high > p_low && s_high > s_low && s_high <= 1;
    }
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
    int tid = 0;
};

enum class Profile { Unknown, Idle, Light, Heavy };
std::array<Profile, MAX_NIC_QUEUES> last_profiles{};

const char *profile_name(Profile state)
{
    switch (state) {
    case Profile::Unknown: return "UNKNOWN";
    case Profile::Idle: return "IDLE";
    case Profile::Light: return "LIGHT";
    case Profile::Heavy: return "HEAVY";
    }
    return "UNKNOWN";
}

struct QueueSample {
    uint64_t generation = 0;
    bpf_qid_napi_stats napi{};
    bpf_qid_tw_stats wheel{};
    Clock::time_point since{};
    Clock::time_point last{};
    Clock::time_point last_log{};
    Profile profile = Profile::Unknown;
    unsigned int high = 0, low = 0, idle = 0;
    double bps = 0, pps = 0;
    bool sampled = false, paced = false, drops = false;
    bool noisy = false;
    int tid = 0;
    uint64_t last_wait = 0;
    bool have_wait = false;
    double wait_ratio = -1;
};

std::array<Registration, MAX_NIC_QUEUES> registrations;
struct WorkerSlot {
    uint64_t generation;
    unsigned int slot;
    int cpu, slo;
};
std::vector<WorkerSlot> workers;
std::mutex registration_mutex;
std::thread sampler;
std::condition_variable sampler_wake;
std::mutex sampler_mutex;
bool stopping = false;
std::unique_ptr<SoftirqProbe> probe;

// The control plane publishes a fresh token before a reused CC slot can be sampled.
// Zero in the low word means unregistered. The high word is the slot generation.
std::array<std::atomic<uint64_t>, MAX_TCP_FLOWS> flow_slots{};

struct FlowSample {
    uint64_t token = 0;
    uint32_t ack = 0;
    unsigned quiet = 0;
};

struct QueueProgress {
    bool inflight = false, stalled = false;
};

void sample_progress(const bpf_cc_map_user &cc, std::vector<FlowSample> &flows,
                     std::array<QueueProgress, MAX_NIC_QUEUES> &progress)
{
    for (size_t i = 0; i < flows.size(); ++i) {
        uint64_t token = flow_slots[i].load(std::memory_order_acquire);
        unsigned qid = static_cast<uint32_t>(token);
        if (!qid)
            continue;
        const auto &entry = cc.entry[i];
        const auto ack = __atomic_load_n(&entry.cnt_rx_ack_bytes, __ATOMIC_RELAXED);
        const bool inflight = __atomic_load_n(&entry.txp, __ATOMIC_RELAXED) != 0;
        if (flow_slots[i].load(std::memory_order_acquire) != token)
            continue;
        auto &sample = flows[i];
        sample.quiet = sample.token == token && inflight && sample.ack == ack
                       ? std::min(sample.quiet + 1, 2U) : 0;
        sample.token = token;
        sample.ack = ack;
        auto &queue = progress[qid - 1];
        queue.inflight |= inflight;
        queue.stalled |= sample.quiet >= 2;
    }
}

bpf_qid_tw_stats sum_tw_stats(const std::vector<bpf_qid_tw_stats> &values)
{
    bpf_qid_tw_stats total{};
    for (const auto &cpu : values) {
        total.tw_sent_pkts += cpu.tw_sent_pkts;
        total.tw_sent_bytes += cpu.tw_sent_bytes;
        total.tw_drop_pkts += cpu.tw_drop_pkts;
    }
    return total;
}

bool read_softirq_counts(std::vector<uint64_t> &total)
{
    std::ifstream file("/proc/softirqs");
    std::string line, column;
    if (!std::getline(file, line))
        return false;
    std::istringstream header(line);
    std::vector<unsigned int> ids;
    while (header >> column) {
        if (column.compare(0, 3, "CPU"))
            return false;
        char *end = nullptr;
        const auto cpu = strtoul(column.c_str() + 3, &end, 10);
        if (*end || cpu >= total.size())
            return false;
        ids.push_back(cpu);
    }
    std::fill(total.begin(), total.end(), 0);
    int vectors = 0;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        row >> column;
        if (column != "NET_RX:" && column != "NET_TX:")
            continue;
        for (auto cpu : ids) {
            uint64_t value;
            if (!(row >> value))
                return false;
            total[cpu] += value;
        }
        ++vectors;
    }
    return !ids.empty() && vectors == 2;
}

struct CoreSample {
    double rx = 0, tx = 0;
    bool valid_probe = false;
    int queues = 0;
    double weights = 0;

    double irq() const { return rx + tx; }
};

double load_weight(const QueueSample &sample)
{
    return sample.pps + sample.bps / (1500 * 8);
}

enum Redline : unsigned int { R1 = 1, R2 = 2, R3 = 4, R4 = 8 };

void report_shadow(const std::array<Registration, MAX_NIC_QUEUES> &snapshot,
                   const std::array<QueueSample, MAX_NIC_QUEUES> &samples,
                   const std::vector<WorkerSlot> &slots, const std::vector<int> &pool)
{
    static std::array<unsigned, MAX_CPU> last{};
    static std::array<unsigned, MAX_CPU> reported{};
    std::array<unsigned, MAX_CPU> worker_counts{};
    for (const auto &worker : slots)
        ++worker_counts[worker.cpu];

    int spare = -1;
    for (int cpu : pool) {
        if (!worker_counts[cpu] &&
            std::none_of(snapshot.begin(), snapshot.end(), [cpu](const auto &queue) {
                return queue.pid && queue.irq_cpu == cpu;
            })) {
            spare = cpu;
            break;
        }
    }

    constexpr unsigned rules[] = {R1, R2, R3, R4};
    for (int cpu : pool) {
        unsigned flags = worker_counts[cpu] > 1 ? R3 : 0U;
        for (size_t qid = 0; qid < snapshot.size(); ++qid) {
            const auto &queue = snapshot[qid];
            const auto &sample = samples[qid];
            if (!queue.pid || queue.irq_cpu != cpu || !sample.sampled)
                continue;
            if (worker_counts[cpu] &&
                (sample.profile == Profile::Heavy ||
                 (sample.profile == Profile::Unknown && sample.noisy)))
                flags |= R1;
            if (sample.profile == Profile::Light &&
                std::any_of(slots.begin(), slots.end(), [cpu](const auto &worker) {
                    return worker.cpu == cpu && worker.slo == THROUGHPUT;
                }))
                flags |= R2;
            if (sample.profile == Profile::Heavy &&
                std::any_of(snapshot.begin(), snapshot.end(), [&](const auto &other) {
                    return &other != &queue && other.pid && other.irq_cpu == cpu && other.slo == LATENCY;
                }))
                flags |= R4;
        }
        const unsigned confirmed = flags & last[cpu];
        const unsigned added = confirmed & ~reported[cpu];
        for (size_t i = 0; i < std::size(rules); ++i) {
            if (added & rules[i])
                fprintf(stderr, "sched_decision mode=shadow rule=R%zu cpu=%d free_cpu=%d\n",
                        i + 1, cpu, spare);
        }
        last[cpu] = flags;
        reported[cpu] = (reported[cpu] & flags) | confirmed;
    }
}

void update_wait(const Registration &app, QueueSample &state, double hz)
{
    if (app.tid != state.tid) {
        state.tid = app.tid;
        state.have_wait = false;
    }
    state.wait_ratio = -1;
    if (!app.tid)
        return;
    const auto path = "/proc/" + std::to_string(app.pid) + "/task/" +
                      std::to_string(app.tid) + "/schedstat";
    std::ifstream file(path);
    uint64_t run, wait;
    if (!(file >> run >> wait)) {
        state.have_wait = false;
        return;
    }
    if (state.have_wait && wait >= state.last_wait)
        state.wait_ratio = (wait - state.last_wait) * hz * 1e-9;
    state.last_wait = wait;
    state.have_wait = true;
}

void sample_queues(unsigned int queues, int napi_fd, int tw_fd, int pending_fd,
                   int cpus, int softirq_fd, const bpf_cc_map_user &cc, Thresholds limits,
                   const std::vector<int> &pool)
{
    std::array<QueueSample, MAX_NIC_QUEUES> queue_state{};
    std::array<bpf_qid_napi_stats, MAX_NIC_QUEUES> napi{};
    std::array<__u32, MAX_CPU> pending{};
    std::vector<FlowSample> flows(MAX_TCP_FLOWS);
    std::vector<bpf_qid_tw_stats> per_cpu(cpus);
    std::vector<SoftirqSample> softirq(softirq_fd >= 0 ? cpus : 0), last_probe = softirq;
    std::vector<CoreSample> cores(cpus);
    std::vector<uint64_t> counts(cpus), last_counts(cpus);
    auto now = Clock::now();
    bool probe_baseline = false, counts_baseline = false;
    std::vector<WorkerSlot> worker_snapshot;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(sampler_mutex);
            if (sampler_wake.wait_until(lock, now + sample_period, [] { return stopping; }))
                break;
        }
        const auto last_tick = now;
        now = Clock::now();
        const double tick_ns = std::chrono::duration<double, std::nano>(now - last_tick).count();
        const __u32 zero = 0;
        const bool have_probe = softirq_fd >= 0 && !bpf_map_lookup_elem(softirq_fd, &zero, softirq.data());
        const bool have_counts = softirq_fd < 0 && read_softirq_counts(counts);
        const bool have_napi = !bpf_map_lookup_elem(napi_fd, &zero, napi.data());
        const bool have_pending = !bpf_map_lookup_elem(pending_fd, &zero, pending.data());
        for (int cpu = 0; cpu < cpus; ++cpu) {
            auto &core = cores[cpu];
            if (have_probe && probe_baseline) {
                double rx = (softirq[cpu].total.net_rx_ns - last_probe[cpu].total.net_rx_ns) / tick_ns;
                double tx = (softirq[cpu].total.net_tx_ns - last_probe[cpu].total.net_tx_ns) / tick_ns;
                const double scale = std::max(1.0, rx + tx);
                core.rx = core.valid_probe ? 0.6 * core.rx + 0.4 * rx / scale : rx / scale;
                core.tx = core.valid_probe ? 0.6 * core.tx + 0.4 * tx / scale : tx / scale;
                core.valid_probe = true;
            } else {
                core.valid_probe = false;
            }
            core.queues = 0;
            core.weights = 0;
        }

        std::array<Registration, MAX_NIC_QUEUES> snapshot;
        {
            std::lock_guard<std::mutex> lock(registration_mutex);
            snapshot = registrations;
            if (settings().shadow)
                worker_snapshot = workers;
        }
        std::array<QueueProgress, MAX_NIC_QUEUES> progress{};
        sample_progress(cc, flows, progress);
        for (__u32 qid = 0; qid < queues; ++qid) {
            const auto &app = snapshot[qid];
            auto &state = queue_state[qid];
            state.sampled = false;
            if (!app.pid)
                continue;
            if (!have_napi || bpf_map_lookup_elem(tw_fd, &qid, per_cpu.data())) {
                state.profile = Profile::Unknown;
                state.last = {};
                continue;
            }
            const auto wheel = sum_tw_stats(per_cpu);
            const auto &current = napi[qid];
            if (state.generation != app.generation || state.last == Clock::time_point{}) {
                state = {};
                state.generation = app.generation;
                state.napi = current;
                state.wheel = wheel;
                state.last = state.since = state.last_log = now;
                continue;
            }

            const double hz = 1.0 / std::chrono::duration<double>(now - state.last).count();
            const auto bytes = current.rx_xsk_bytes - state.napi.rx_xsk_bytes +
                               current.tx_direct_bytes - state.napi.tx_direct_bytes +
                               wheel.tw_sent_bytes - state.wheel.tw_sent_bytes;
            const auto packets = current.rx_xsk_pkts - state.napi.rx_xsk_pkts +
                                 current.tx_direct_pkts - state.napi.tx_direct_pkts +
                                 wheel.tw_sent_pkts - state.wheel.tw_sent_pkts;
            state.bps = 0.6 * state.bps + 0.4 * (bytes * 8.0 * hz);
            state.pps = 0.6 * state.pps + 0.4 * (packets * hz);
            state.paced = (current.tw_enq_attempts - state.napi.tw_enq_attempts) * hz > 1000 ||
                          (wheel.tw_sent_pkts - state.wheel.tw_sent_pkts) * hz > 1000;
            state.drops = wheel.tw_drop_pkts != state.wheel.tw_drop_pkts;
            update_wait(app, state, hz);
            state.last = now;
            state.napi = current;
            state.wheel = wheel;
            state.sampled = true;
            auto &core = cores[app.irq_cpu];
            ++core.queues;
            core.weights += load_weight(state);
        }

        for (__u32 qid = 0; qid < queues; ++qid) {
            const auto &app = snapshot[qid];
            auto &state = queue_state[qid];
            if (!state.sampled)
                continue;
            const auto &core = cores[app.irq_cpu];
            const bool valid_probe = core.valid_probe;
            const double share = core.weights > 0 ?
                                 core.irq() * load_weight(state) / core.weights : 0;
            const bool heavy = state.bps >= limits.b_high || state.pps >= limits.p_high ||
                               (valid_probe && (core.queues == 1 ? core.irq() : share) >= limits.s_high);
            const bool low = state.bps <= limits.b_low && state.pps <= limits.p_low &&
                             (softirq_fd < 0 || (valid_probe && (core.queues == 1 ? core.irq() : share) <= limits.s_low));
            const bool idle = state.bps < 1e6 && state.pps < 1000;
            const bool valid_counts = have_counts && counts_baseline;
            const bool noisy = (valid_probe && core.irq() > 0.20) ||
                               (valid_counts && (counts[app.irq_cpu] - last_counts[app.irq_cpu]) * 1e9 / tick_ns > 5000);
            state.noisy = noisy;
            const bool blocked = state.drops || progress[qid].stalled ||
                                 (idle && (!have_pending || pending[app.irq_cpu] || noisy ||
                                           (!valid_probe && !valid_counts)));
            state.high = heavy ? std::min(state.high + 1, 2U) : 0;
            state.low = low && !blocked ? std::min(state.low + 1, 6U) : 0;
            state.idle = idle && !blocked && !progress[qid].inflight ? std::min(state.idle + 1, 10U) : 0;
            const auto before = state.profile;
            if (now - state.since < std::chrono::milliseconds(200) || (idle && blocked))
                state.profile = Profile::Unknown;
            else if (state.high >= 2)
                state.profile = Profile::Heavy;
            else if (state.idle >= 10)
                state.profile = Profile::Idle;
            else if (state.profile == Profile::Heavy) {
                if (state.low >= 6)
                    state.profile = Profile::Light;
            } else if (!idle && !heavy) {
                state.profile = Profile::Light;
            }

            if (settings().verbose && (before != state.profile || now - state.last_log >= std::chrono::seconds(1))) {
                fprintf(stderr, "sched_sample qid=%u pid=%d slo=%d irq_cpu=%d app_cpu=%d profile=%s bps=%.0f pps=%.0f paced=%d net_rx=%.4f net_tx=%.4f irq_share=%.4f wait_ratio=%.4f tid=%d inflight=%d stalled=%d rx_pkts=%llu tx_direct_pkts=%llu tw_attempts=%llu tw_submit=%llu tw_drop=%llu\n",
                        qid, app.pid, app.slo, app.irq_cpu, app.app_cpu, profile_name(state.profile),
                        state.bps, state.pps, state.paced, valid_probe ? core.rx : -1,
                        valid_probe ? core.tx : -1, valid_probe ? share : -1, state.wait_ratio,
                        state.tid, progress[qid].inflight, progress[qid].stalled,
                        state.napi.rx_xsk_pkts, state.napi.tx_direct_pkts, state.napi.tw_enq_attempts,
                        state.wheel.tw_sent_pkts, state.wheel.tw_drop_pkts);
                state.last_log = now;
            }
        }
        if (settings().shadow)
            report_shadow(snapshot, queue_state, worker_snapshot, pool);
        {
            std::lock_guard<std::mutex> lock(registration_mutex);
            for (unsigned qid = 0; qid < queues; ++qid)
                if (snapshot[qid].generation == registrations[qid].generation &&
                    snapshot[qid].pid && registrations[qid].pid)
                    last_profiles[qid] = queue_state[qid].sampled && !queue_state[qid].drops && !progress[qid].stalled
                                         ? queue_state[qid].profile : Profile::Unknown;
        }
        if (have_probe)
            last_probe.swap(softirq);
        if (have_counts)
            last_counts.swap(counts);
        probe_baseline = have_probe;
        counts_baseline = have_counts;
    }
}
} // namespace

bool scheduler_collect_qid_stats()
{
    return settings().qid_enabled;
}

bool scheduler_queue_is_light(unsigned int qid)
{
    std::lock_guard<std::mutex> lock(registration_mutex);
    return registrations[qid].pid && (last_profiles[qid] == Profile::Light || last_profiles[qid] == Profile::Idle);
}

void scheduler_register_queue(unsigned int qid, int pid, int slo, int irq_cpu, int app_cpu)
{
    if (!scheduler_collect_qid_stats())
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto &entry = registrations[qid];
    entry = {entry.generation + 1, pid, slo, irq_cpu, app_cpu};
    last_profiles[qid] = Profile::Unknown;
}

void scheduler_unregister_queue(unsigned int qid)
{
    if (!scheduler_collect_qid_stats())
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    registrations[qid].pid = 0;
}

void scheduler_register_flow(unsigned int cc_idx, unsigned int qid)
{
    if (!scheduler_collect_qid_stats() || qid >= MAX_NIC_QUEUES)
        return;
    auto &slot = flow_slots[cc_idx];
    const uint64_t generation = (slot.load(std::memory_order_relaxed) >> 32) + 1;
    slot.store((generation << 32) | (qid + 1), std::memory_order_release);
}

void scheduler_unregister_flow(unsigned int cc_idx)
{
    if (scheduler_collect_qid_stats())
        flow_slots[cc_idx].fetch_and(0xffffffff00000000ULL, std::memory_order_release);
}

void scheduler_register_worker(unsigned int qid, int pid, int tid)
{
    if (!scheduler_collect_qid_stats())
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto &entry = registrations[qid];
    if (entry.pid == pid)
        entry.tid = tid;
}

void scheduler_register_worker_slot(uint64_t generation, unsigned int slot, int cpu, int slo)
{
    if (!settings().shadow || cpu < 0 || cpu >= MAX_CPU)
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto existing = std::find_if(workers.begin(), workers.end(), [=](const auto &worker) {
        return worker.generation == generation && worker.slot == slot;
    });
    if (existing == workers.end())
        workers.push_back({generation, slot, cpu, slo});
    else
        *existing = {generation, slot, cpu, slo};
}

void scheduler_unregister_workers(uint64_t generation)
{
    if (!settings().shadow)
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    workers.erase(std::remove_if(workers.begin(), workers.end(), [generation](const auto &worker) {
        return worker.generation == generation;
    }), workers.end());
}

int start_scheduler(const CpuTopology &topology, unsigned int queues, int napi_fd, int tw_fd,
                    int tw_pending_fd, const bpf_cc_map_user *cc)
{
    const auto &config = settings();
    if (config.mode && strcmp(config.mode, "off") && !config.shadow && !config.admit_only) {
        fprintf(stderr, "ETRAN_SCHED_MODE must be off, shadow, or admit_only\n");
        return -EINVAL;
    }
    if (config.probe_enabled && !config.qid_enabled) {
        fprintf(stderr, "ETRAN_SOFTIRQ_PROBE=1 requires ETRAN_QID_STATS=1 in off mode\n");
        return -EINVAL;
    }
    if (!config.qid_enabled && !config.probe_enabled)
        return 0;
    if (!config.sample) {
        if (config.shadow || config.admit_only || config.probe_enabled) {
            fprintf(stderr, "ETRAN_QID_STATS_SAMPLE=0 is only for counters-only off-mode comparisons\n");
            return -EINVAL;
        }
        return 0;
    }
    Thresholds limits;
    if (!limits.load()) {
        fprintf(stderr, "Scheduler high thresholds must exceed low thresholds; softirq ratios must be in [0, 1]\n");
        return -EINVAL;
    }

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
    } else if (config.verbose) {
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
            sample_queues(queues, napi_fd, tw_fd, tw_pending_fd, possible,
                          probe ? probe->map_fd : -1, *cc, limits, topology.pool);
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
    if (config.verbose)
        fprintf(stderr, "sched_start mode=%s cpu=%d qid=%d softirq=%d\n",
                config.mode ? config.mode : "off", cpu, config.qid_enabled, config.probe_enabled);
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
