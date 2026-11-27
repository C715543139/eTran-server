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
#include <sys/mman.h>

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
    bool manual = mode && strcmp(mode, "manual_actuate") == 0;
    bool active = mode && strcmp(mode, "active") == 0;
    bool sample = !sample_flag || strcmp(sample_flag, "0") != 0;
    bool probe_enabled = probe && strcmp(probe, "1") == 0;
    bool qid_enabled = shadow || admit_only || manual || active || (qid && strcmp(qid, "1") == 0);
    bool verbose = shadow || (verbose_flag && strcmp(verbose_flag, "1") == 0);
};

struct ActiveLimits {
    double bps = 22e9, pps = 2e6;
    bool irq_to_app = false;

    bool load()
    {
        const char *names[] = {"ETRAN_SCHED_CAP_BPS", "ETRAN_SCHED_CAP_PPS"};
        double *values[] = {&bps, &pps};
        for (size_t i = 0; i < std::size(names); ++i) {
            if (const char *value = std::getenv(names[i])) {
                char *end = nullptr;
                errno = 0;
                *values[i] = strtod(value, &end);
                if (errno || end == value || *end || !std::isfinite(*values[i]) || *values[i] <= 0)
                    return false;
            }
        }
        if (const char *fold = std::getenv("ETRAN_FOLD_MODE")) {
            irq_to_app = strcmp(fold, "irq_to_app") == 0;
            if (!irq_to_app && strcmp(fold, "app_to_irq") != 0)
                return false;
        }
        return true;
    }
};

ActiveLimits active_limits;

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
    bool irq_handoff = false;
    uint64_t owner = 0;
    bool single_worker_queue = false;
    bool fixed_irq = false;
};

enum class Profile { Unknown, Idle, Light, Heavy };
std::array<Profile, MAX_NIC_QUEUES> last_profiles{};

struct IrqProgressSample {
    uint64_t packets = 0;
    Clock::time_point sampled_at{};
    Clock::time_point last_traffic{};
    bool no_backlog = false;
};

std::array<IrqProgressSample, MAX_NIC_QUEUES> last_irq_progress;
std::array<__u32, MAX_CPU> last_pending{};
bool pending_is_valid = false;

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
    uint64_t rx_packets = 0, direct_packets = 0;
    Clock::time_point since{};
    Clock::time_point last{};
    Clock::time_point last_log{};
    Profile profile = Profile::Unknown;
    unsigned int high = 0, low = 0, idle = 0;
    unsigned int large_packet_samples = 0;
    double bps = 0, pps = 0;
    bool sampled = false, paced = false, drops = false;
    bool wheel_followup = false;
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
    int pid;
    uint64_t start_time;
    bool fixed;
};
std::vector<WorkerSlot> workers;
std::mutex registration_mutex;
SchedulerPlacement active_placement;
std::optional<SchedulerAction> pending_action;
struct QueueRate {
    Clock::time_point time{};
    double irq_share = -1;
    bool self_folded_large = false;
    std::array<double, 10> bps_history{}, pps_history{};
    unsigned int next = 0;
};
std::array<QueueRate, MAX_NIC_QUEUES> last_rates;
std::thread sampler;
std::condition_variable sampler_wake;
std::mutex sampler_mutex;
bool stopping = false;
std::unique_ptr<SoftirqProbe> probe;

// The control plane publishes a fresh token before a reused CC slot can be sampled.
// Zero in the low word means unregistered. The high word is the slot generation.
std::array<std::atomic<uint64_t>, MAX_TCP_FLOWS> flow_slots{};
struct FlowRegistration {
    unsigned int index;
    uint64_t token;
};
std::vector<FlowRegistration> active_flows;
std::array<unsigned int, MAX_TCP_FLOWS> flow_positions{};
std::array<uint64_t, MAX_NIC_QUEUES> flow_revisions{};

struct FlowSample {
    uint64_t token = 0;
    uint32_t ack = 0;
    unsigned quiet = 0;
    unsigned epoch = 0;
};

struct QueueProgress {
    bool known = false, inflight = false, stalled = false;
};

void sample_progress(const bpf_cc_map_user &cc, std::vector<FlowSample> &flows,
                     const std::vector<FlowRegistration> &active, unsigned int epoch,
                     std::array<QueueProgress, MAX_NIC_QUEUES> &progress)
{
    for (const auto &[i, token] : active) {
        unsigned qid = static_cast<uint32_t>(token);
        if (!qid)
            continue;
        auto &queue = progress[qid - 1];
        if (!queue.known)
            continue;
        if (flow_slots[i].load(std::memory_order_acquire) != token) {
            queue.known = false;
            continue;
        }
        const auto &entry = cc.entry[i];
        const auto ack = __atomic_load_n(&entry.cnt_rx_ack_bytes, __ATOMIC_RELAXED);
        const bool inflight = __atomic_load_n(&entry.txp, __ATOMIC_RELAXED) != 0;
        if (flow_slots[i].load(std::memory_order_acquire) != token) {
            queue.known = false;
            continue;
        }
        auto &sample = flows[i];
        sample.quiet = sample.token == token && sample.epoch == epoch - 1 && inflight && sample.ack == ack
                       ? std::min(sample.quiet + 1, 2U) : 0;
        sample.epoch = epoch;
        sample.token = token;
        sample.ack = ack;
        queue.inflight |= inflight;
        queue.stalled |= sample.quiet >= 2;
    }
}

class NapiCounters {
    using Samples = std::array<bpf_qid_napi_stats, MAX_NIC_QUEUES>;
    const Samples *mapped = nullptr;
    Samples fallback{};
    int fd;

public:
    explicit NapiCounters(int map_fd) : fd(map_fd)
    {
        void *addr = mmap(nullptr, sizeof(Samples), PROT_READ, MAP_SHARED, fd, 0);
        if (addr != MAP_FAILED)
            mapped = static_cast<const Samples *>(addr);
        if (settings().verbose)
            fprintf(stderr, "sched_napi_read mode=%s\n", mapped ? "mmap" : "lookup");
    }

    ~NapiCounters()
    {
        if (mapped)
            munmap(const_cast<Samples *>(mapped), sizeof(Samples));
    }

    bool refresh()
    {
        const __u32 key = 0;
        return mapped || bpf_map_lookup_elem(fd, &key, fallback.data()) == 0;
    }

    bpf_qid_napi_stats read(unsigned int qid) const
    {
        if (!mapped)
            return fallback[qid];
        const auto &row = (*mapped)[qid];
        bpf_qid_napi_stats sample{};
        sample.rx_xsk = __atomic_load_n(&row.rx_xsk, __ATOMIC_RELAXED);
        sample.tx_direct = __atomic_load_n(&row.tx_direct, __ATOMIC_RELAXED);
        sample.tw_enq_attempts = __atomic_load_n(&row.tw_enq_attempts, __ATOMIC_RELAXED);
        return sample;
    }
};

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

bool light_queue(Profile profile)
{
    return profile == Profile::Light || profile == Profile::Idle;
}

bool can_merge_irq(unsigned int qid, int cpu, Clock::time_point now)
{
    const auto &source = registrations[qid];
    const auto &target = active_placement.cores[cpu];
    if (!source.pid || source.slo == LATENCY || source.irq_cpu == cpu || !target.irq || target.app ||
        target.latency || target.quarantined)
        return false;
    double bps = 0, pps = 0;
    int confirmed = 0;
    for (unsigned q = 0; q < registrations.size(); ++q) {
        const auto &slot = registrations[q];
        if (!slot.pid || (q != qid && slot.irq_cpu != cpu))
            continue;
        const auto &rate = last_rates[q];
        if (slot.slo == LATENCY || slot.irq_handoff || last_profiles[q] == Profile::Unknown ||
            now - rate.time > 2 * sample_period)
            return false;
        confirmed += q != qid;
        bps += *std::max_element(rate.bps_history.begin(), rate.bps_history.end());
        pps += *std::max_element(rate.pps_history.begin(), rate.pps_history.end());
    }
    const auto probe_room = [&] {
        if (!settings().probe_enabled)
            return true;
        auto other = std::find_if(registrations.begin(), registrations.end(), [cpu](const auto &q) {
            return q.pid && q.irq_cpu == cpu;
        });
        if (other == registrations.end())
            return false;
        const auto a = last_rates[qid].irq_share, b = last_rates[other - registrations.begin()].irq_share;
        return a >= 0 && b >= 0 && a + b <= .85;
    };
    return confirmed == target.irq && bps <= active_limits.bps && pps <= active_limits.pps && probe_room();
}

std::optional<SchedulerAction> select_active_action(const std::vector<int> &pool, Clock::time_point now)
{
    using Action = SchedulerAction;
    using Kind = Action::Kind;
    using Reason = Action::Reason;
    static std::array<uint64_t, MAX_NIC_QUEUES> degraded_owner{};
    static std::array<uint64_t, MAX_NIC_QUEUES> light_conflict{};
    static std::array<bool, MAX_CPU> worker_conflict{};
    static int last_spare = -1;
    static Clock::time_point spare_since{};

    const auto &cores = active_placement.cores;
    auto usable = [&](int cpu) { return cpu >= 0 && cpu < MAX_CPU && !cores[cpu].quarantined; };
    auto spare_it = std::find_if(pool.begin(), pool.end(), [&](int cpu) {
        return usable(cpu) && !cores[cpu].app && !cores[cpu].irq;
    });
    const int spare = spare_it == pool.end() ? -1 : *spare_it;
    if (spare != last_spare || active_placement.busy) {
        spare_since = now;
        last_spare = spare;
    }
    if (active_placement.busy) {
        light_conflict.fill(0);
        worker_conflict.fill(false);
        return {};
    }

    auto irq_move = [&](unsigned qid, int target, Reason reason) {
        const auto &q = registrations[qid];
        return Action{Kind::Irq, reason, q.owner, qid, q.pid, q.irq_cpu, target};
    };
    auto worker_move = [](const WorkerSlot &slot, int target, Reason reason) {
        return Action{Kind::Worker, reason, slot.generation, slot.slot, slot.pid, slot.cpu, target, slot.start_time};
    };
    auto movable_irq = [&](unsigned qid) {
        const auto &q = registrations[qid];
        return q.pid && !q.fixed_irq && !q.irq_handoff && usable(q.irq_cpu);
    };
    auto own_light_app = [&](unsigned int qid) {
        const auto &q = registrations[qid];
        return q.single_worker_queue && q.slo != THROUGHPUT && light_queue(last_profiles[qid]) &&
               usable(q.app_cpu) && q.app_cpu != q.irq_cpu && cores[q.app_cpu].app == 1 && !cores[q.app_cpu].irq;
    };
    bool redline = false;
    for (unsigned qid = 0; qid < registrations.size(); ++qid) {
        const auto &q = registrations[qid];
        if (!movable_irq(qid) || q.slo == LATENCY || last_profiles[qid] != Profile::Heavy)
            continue;
        for (unsigned j = 0; j < registrations.size(); ++j) {
            const auto &lat = registrations[j];
            if (!lat.pid || lat.slo != LATENCY || lat.irq_cpu != q.irq_cpu || !light_queue(last_profiles[j]))
                continue;
            spare_since = now;
            redline = true;
            if (spare >= 0)
                return irq_move(qid, spare, Reason::R4);
            if (movable_irq(j) && own_light_app(j))
                return irq_move(j, lat.app_cpu, Reason::R4);
        }
    }
    for (unsigned qid = 0; qid < registrations.size(); ++qid) {
        const auto &q = registrations[qid];
        const bool busy_queue = last_profiles[qid] == Profile::Heavy ||
                                (last_profiles[qid] != Profile::Unknown && last_rates[qid].self_folded_large);
        const bool heavy_shared = movable_irq(qid) && cores[q.irq_cpu].app && busy_queue;
        if (!heavy_shared) {
            degraded_owner[qid] = 0;
            continue;
        }
        spare_since = now;
        redline = true;
        if (spare >= 0)
            return irq_move(qid, spare, Reason::R1);
        if (q.slo == LATENCY)
            continue;
        for (int cpu : pool)
            if (can_merge_irq(qid, cpu, now))
                return irq_move(qid, cpu, Reason::Merge);
        if (degraded_owner[qid] != q.owner) {
            degraded_owner[qid] = q.owner;
            return Action{Kind::Degraded, Reason::Capacity, q.owner, qid, q.pid, q.irq_cpu, -1};
        }
    }
    for (unsigned qid = 0; qid < registrations.size(); ++qid) {
        const auto &q = registrations[qid];
        if (!movable_irq(qid) || !light_queue(last_profiles[qid]) ||
            !std::any_of(workers.begin(), workers.end(), [&](const auto &w) {
                return w.cpu == q.irq_cpu && w.slo == THROUGHPUT;
            })) {
            light_conflict[qid] = 0;
            continue;
        }
        redline = true;
        spare_since = now;
        if (light_conflict[qid] != q.owner) {
            light_conflict[qid] = q.owner;
            continue;
        }
        if (spare >= 0)
            return irq_move(qid, spare, Reason::R2);
        if (own_light_app(qid))
            return irq_move(qid, q.app_cpu, Reason::R2);
    }
    for (int cpu : pool) {
        if (cores[cpu].app <= 1) {
            worker_conflict[cpu] = false;
            continue;
        }
        redline = true;
        spare_since = now;
        if (!worker_conflict[cpu]) {
            worker_conflict[cpu] = true;
            continue;
        }
        if (spare < 0 || !usable(cpu))
            continue;
        auto select = [&](bool latency) {
            return std::find_if(workers.begin(), workers.end(), [&](const auto &w) {
                return w.cpu == cpu && !w.fixed && w.start_time && (!latency || w.slo == LATENCY);
            });
        };
        auto slot = select(true);
        if (slot == workers.end())
            slot = select(false);
        if (slot != workers.end())
            return worker_move(*slot, spare, Reason::R3);
    }
    if (redline || spare < 0 || now - spare_since < std::chrono::milliseconds(500))
        return {};
    auto folded = [&](unsigned qid) {
        const auto &q = registrations[qid];
        return q.single_worker_queue && q.irq_cpu == q.app_cpu && usable(q.irq_cpu) &&
               cores[q.irq_cpu].irq == 1 && cores[q.irq_cpu].app == 1;
    };
    for (unsigned qid = 0; qid < registrations.size(); ++qid)
        if (movable_irq(qid) && folded(qid) && (registrations[qid].slo == THROUGHPUT ||
                                               last_profiles[qid] == Profile::Heavy))
            return irq_move(qid, spare, Reason::Expand);
    for (unsigned qid = 0; qid < registrations.size(); ++qid) {
        const auto &q = registrations[qid];
        if (!q.pid || !folded(qid) || !light_queue(last_profiles[qid]))
            continue;
        auto worker = std::find_if(workers.begin(), workers.end(), [&](const auto &slot) {
            return slot.generation == q.owner && !slot.fixed && slot.start_time;
        });
        if (worker != workers.end())
            return worker_move(*worker, spare, Reason::Expand);
    }
    for (unsigned qid = 0; qid < registrations.size(); ++qid)
        if (movable_irq(qid) && cores[registrations[qid].irq_cpu].irq > 1 && last_profiles[qid] == Profile::Heavy)
            return irq_move(qid, spare, Reason::Expand);
    return {};
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
    NapiCounters napi(napi_fd);
    std::array<__u32, MAX_CPU> pending{};
    std::vector<FlowSample> flows(MAX_TCP_FLOWS);
    std::vector<bpf_qid_tw_stats> per_cpu(cpus);
    std::vector<SoftirqSample> softirq(softirq_fd >= 0 ? cpus : 0), last_probe = softirq;
    std::vector<CoreSample> cores(cpus);
    std::vector<uint64_t> counts(cpus), last_counts(cpus);
    auto now = Clock::now();
    bool probe_baseline = false, counts_baseline = false;
    std::vector<WorkerSlot> worker_snapshot;
    std::vector<FlowRegistration> flow_snapshot;
    unsigned int epoch = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(sampler_mutex);
            if (sampler_wake.wait_until(lock, now + sample_period, [] { return stopping; }))
                break;
        }
        const auto last_tick = now;
        now = Clock::now();
        const double tick_ns = std::chrono::duration<double, std::nano>(now - last_tick).count();
        ++epoch;
        const __u32 zero = 0;
        const bool have_probe = softirq_fd >= 0 && !bpf_map_lookup_elem(softirq_fd, &zero, softirq.data());
        const bool have_napi = napi.refresh();
        const bool have_pending = !bpf_map_lookup_elem(pending_fd, &zero, pending.data());
        const bool any_pending = have_pending && std::any_of(pending.begin(), pending.end(), [](auto count) {
            return count != 0;
        });
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
        std::array<uint64_t, MAX_NIC_QUEUES> flow_versions;
        {
            std::lock_guard<std::mutex> lock(registration_mutex);
            snapshot = registrations;
            flow_versions = flow_revisions;
            flow_snapshot = active_flows;
            if (settings().shadow)
                worker_snapshot = workers;
        }
        std::array<QueueProgress, MAX_NIC_QUEUES> progress{};
        bool need_counts = false;
        for (__u32 qid = 0; qid < queues; ++qid) {
            const auto &app = snapshot[qid];
            auto &state = queue_state[qid];
            state.sampled = false;
            if (!app.pid)
                continue;
            const auto current = have_napi ? napi.read(qid) : bpf_qid_napi_stats{};
            // A delayed sampler must not treat ambiguous packed deltas as measured rates.
            // A second is shorter than either field's wrap horizon on the supported NICs.
            const bool first = state.generation != app.generation || state.last == Clock::time_point{} ||
                               now - state.last > std::chrono::seconds(1);
            const bool enqueued = current.tw_enq_attempts != state.napi.tw_enq_attempts;
            const bool read_wheel = first || enqueued || state.paced || state.wheel_followup ||
                                    !have_pending || pending[app.irq_cpu] != 0;
            if (!have_napi || (read_wheel && bpf_map_lookup_elem(tw_fd, &qid, per_cpu.data()))) {
                state.profile = Profile::Unknown;
                state.last = {};
                continue;
            }
            const auto wheel = read_wheel ? sum_tw_stats(per_cpu) : state.wheel;
            if (first) {
                state = {};
                state.generation = app.generation;
                state.napi = current;
                state.wheel = wheel;
                state.wheel_followup = current.tw_enq_attempts != 0;
                state.last = state.since = state.last_log = now;
                continue;
            }

            const double hz = 1.0 / std::chrono::duration<double>(now - state.last).count();
            const auto rx = current.rx_xsk - state.napi.rx_xsk;
            const auto tx = current.tx_direct - state.napi.tx_direct;
            const auto bytes = (rx >> QID_COUNTER_PACKET_BITS) + (tx >> QID_COUNTER_PACKET_BITS) +
                               wheel.tw_sent_bytes - state.wheel.tw_sent_bytes;
            const auto rx_packets = rx & QID_COUNTER_PACKET_MASK;
            const auto direct_packets = tx & QID_COUNTER_PACKET_MASK;
            const auto packets = rx_packets + direct_packets + wheel.tw_sent_pkts - state.wheel.tw_sent_pkts;
            state.rx_packets += rx_packets;
            state.direct_packets += direct_packets;
            const bool high_progress = bytes * 8.0 * hz >= limits.b_high ||
                                       packets * hz >= limits.p_high;
            progress[qid].known = app.irq_handoff || !high_progress;
            state.bps = 0.6 * state.bps + 0.4 * (bytes * 8.0 * hz);
            state.pps = 0.6 * state.pps + 0.4 * (packets * hz);
            if (settings().active && app.slo == AUTO && app.single_worker_queue && app.app_cpu == app.irq_cpu &&
                state.bps >= limits.b_low && packets && bytes / packets >= 512)
                state.large_packet_samples = std::min(state.large_packet_samples + 1, 2U);
            else
                state.large_packet_samples = 0;
            state.paced = (current.tw_enq_attempts - state.napi.tw_enq_attempts) * hz > 1000 ||
                          (wheel.tw_sent_pkts - state.wheel.tw_sent_pkts) * hz > 1000;
            state.drops = wheel.tw_drop_pkts != state.wheel.tw_drop_pkts;
            state.wheel_followup = enqueued || state.drops || wheel.tw_sent_pkts != state.wheel.tw_sent_pkts ||
                                   (state.wheel_followup && (!have_pending || any_pending));
            need_counts |= app.irq_handoff || (state.bps < 1e6 && state.pps < 1000);
            update_wait(app, state, hz);
            state.last = now;
            state.napi = current;
            state.wheel = wheel;
            state.sampled = true;
            auto &core = cores[app.irq_cpu];
            ++core.queues;
            core.weights += load_weight(state);
        }

        sample_progress(cc, flows, flow_snapshot, epoch, progress);
        const bool have_counts = softirq_fd < 0 && need_counts && read_softirq_counts(counts);
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
                                 (idle && (!progress[qid].known || !have_pending || pending[app.irq_cpu] || noisy ||
                                           (!valid_probe && !valid_counts)));
            state.high = heavy ? std::min(state.high + 1, 2U) : 0;
            state.low = low && progress[qid].known && !blocked ? std::min(state.low + 1, 6U) : 0;
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
                        static_cast<unsigned long long>(state.rx_packets),
                        static_cast<unsigned long long>(state.direct_packets), state.napi.tw_enq_attempts,
                        state.wheel.tw_sent_pkts, state.wheel.tw_drop_pkts);
                state.last_log = now;
            }
        }
        if (settings().shadow)
            report_shadow(snapshot, queue_state, worker_snapshot, pool);
        {
            std::lock_guard<std::mutex> lock(registration_mutex);
            for (unsigned qid = 0; qid < queues; ++qid) {
                if (snapshot[qid].generation == registrations[qid].generation &&
                    snapshot[qid].pid && registrations[qid].pid) {
                    const auto &state = queue_state[qid];
                    last_profiles[qid] = state.sampled && !state.drops && !progress[qid].stalled &&
                                         flow_versions[qid] == flow_revisions[qid]
                                         ? state.profile : Profile::Unknown;
                    if (settings().active) {
                        const auto &cpu = cores[snapshot[qid].irq_cpu];
                        auto &rate = last_rates[qid];
                        if (rate.time == Clock::time_point{} || now - rate.time > 2 * sample_period)
                            rate = {};
                        rate.time = state.sampled ? now : Clock::time_point{};
                        rate.irq_share = cpu.valid_probe ? cpu.rx + cpu.tx : -1;
                        rate.self_folded_large = state.large_packet_samples >= 2;
                        // Keep a half second of demand evidence across short incumbent troughs.
                        rate.bps_history[rate.next] = state.bps;
                        rate.pps_history[rate.next] = state.pps;
                        rate.next = (rate.next + 1) % rate.bps_history.size();
                    }
                    if (settings().manual || settings().active) {
                        auto &irq = last_irq_progress[qid];
                        const auto packets = state.rx_packets + state.direct_packets +
                                             state.wheel.tw_sent_pkts;
                        irq.no_backlog = state.sampled && (cores[snapshot[qid].irq_cpu].valid_probe ||
                                         (have_counts && counts_baseline)) &&
                                         flow_versions[qid] == flow_revisions[qid] &&
                                         progress[qid].known && !state.drops && !state.noisy &&
                                         !progress[qid].inflight && !progress[qid].stalled;
                        if (packets != irq.packets || !irq.no_backlog || !irq.sampled_at.time_since_epoch().count())
                            irq.last_traffic = now;
                        irq.packets = packets;
                        irq.sampled_at = state.sampled ? now : Clock::time_point{};
                    }
                }
            }
            if (settings().manual || settings().active) {
                last_pending = pending;
                pending_is_valid = have_pending;
            }
            if (settings().active && !pending_action)
                pending_action = select_active_action(pool, now);
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

std::optional<SchedulerAction> scheduler_exchange_placement(const SchedulerPlacement &placement)
{
    std::lock_guard<std::mutex> lock(registration_mutex);
    active_placement = placement;
    std::optional<SchedulerAction> result;
    result.swap(pending_action);
    return result;
}

bool scheduler_can_merge_irq(unsigned int qid, int cpu)
{
    std::lock_guard<std::mutex> lock(registration_mutex);
    return can_merge_irq(qid, cpu, Clock::now());
}

bool scheduler_request_admission_fold()
{
    using Kind = SchedulerAction::Kind;
    using Reason = SchedulerAction::Reason;
    std::lock_guard<std::mutex> lock(registration_mutex);
    if (active_placement.busy || pending_action)
        return false;
    const auto now = Clock::now();
    for (unsigned qid = 0; qid < registrations.size(); ++qid) {
        const auto &q = registrations[qid];
        if (!q.pid || q.slo != LATENCY || !q.single_worker_queue || q.irq_handoff || q.app_cpu < 0 ||
            q.irq_cpu == q.app_cpu || !light_queue(last_profiles[qid]) || now - last_rates[qid].time > 2 * sample_period)
            continue;
        const auto &source = active_placement.cores[q.app_cpu];
        const auto &target = active_placement.cores[q.irq_cpu];
        if (source.app != 1 || source.irq || source.quarantined || target.app || target.irq != 1 || target.quarantined)
            continue;
        if (active_limits.irq_to_app) {
            if (q.fixed_irq)
                continue;
            pending_action = SchedulerAction{Kind::Irq, Reason::Fold, q.owner, qid, q.pid, q.irq_cpu, q.app_cpu};
        } else {
            auto w = std::find_if(workers.begin(), workers.end(), [&](const auto &slot) {
                return slot.generation == q.owner && !slot.fixed && slot.start_time;
            });
            if (w == workers.end())
                continue;
            pending_action = SchedulerAction{Kind::Worker, Reason::Fold, q.owner, w->slot, q.pid,
                                              q.app_cpu, q.irq_cpu, w->start_time};
        }
        return true;
    }
    return false;
}

SchedulerIrqProgress scheduler_irq_progress(unsigned int qid, int source, int target)
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(registration_mutex);
    const auto &sample = last_irq_progress[qid];
    const bool fresh = registrations[qid].pid && now - sample.sampled_at <= 2 * sample_period;
    const bool drained = fresh && sample.no_backlog && pending_is_valid &&
                         last_pending[source] == 0 && last_pending[target] == 0;
    return {fresh, drained && last_profiles[qid] == Profile::Idle,
            drained && now - sample.last_traffic >= std::chrono::milliseconds(100)};
}

void scheduler_track_irq_handoff(unsigned int qid, bool moving)
{
    std::lock_guard<std::mutex> lock(registration_mutex);
    registrations[qid].irq_handoff = moving;
}

void scheduler_register_queue(unsigned int qid, int pid, int slo, int irq_cpu, int app_cpu,
                              uint64_t owner, bool single_worker_queue, bool fixed_irq)
{
    if (!scheduler_collect_qid_stats())
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto &entry = registrations[qid];
    entry = {entry.generation + 1, pid, slo, irq_cpu, app_cpu};
    entry.owner = owner;
    entry.single_worker_queue = single_worker_queue;
    entry.fixed_irq = fixed_irq;
    last_profiles[qid] = Profile::Unknown;
    last_irq_progress[qid] = {};
    last_rates[qid] = {};
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
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto &slot = flow_slots[cc_idx];
    const uint64_t generation = (slot.load(std::memory_order_relaxed) >> 32) + 1;
    const auto token = (generation << 32) | (qid + 1);
    flow_positions[cc_idx] = active_flows.size();
    active_flows.push_back({cc_idx, token});
    ++flow_revisions[qid];
    slot.store(token, std::memory_order_release);
}

void scheduler_unregister_flow(unsigned int cc_idx)
{
    if (!scheduler_collect_qid_stats())
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    const auto token = flow_slots[cc_idx].fetch_and(0xffffffff00000000ULL, std::memory_order_release);
    const auto qid = static_cast<uint32_t>(token);
    if (!qid)
        return;
    const auto position = flow_positions[cc_idx];
    active_flows[position] = active_flows.back();
    flow_positions[active_flows[position].index] = position;
    active_flows.pop_back();
    ++flow_revisions[qid - 1];
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

void scheduler_update_app_cpu(unsigned int qid, int pid, int cpu)
{
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto &entry = registrations[qid];
    if (entry.pid == pid)
        entry.app_cpu = cpu;
}

void scheduler_update_irq_cpu(unsigned int qid, int pid, int cpu)
{
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto &entry = registrations[qid];
    if (entry.pid == pid)
        entry.irq_cpu = cpu;
}

void scheduler_register_worker_slot(uint64_t generation, unsigned int slot, int cpu, int slo,
                                    int pid, uint64_t start_time, bool fixed)
{
    if (!(settings().shadow || settings().active) || cpu < 0 || cpu >= MAX_CPU)
        return;
    std::lock_guard<std::mutex> lock(registration_mutex);
    auto existing = std::find_if(workers.begin(), workers.end(), [=](const auto &worker) {
        return worker.generation == generation && worker.slot == slot;
    });
    if (existing == workers.end())
        workers.push_back({generation, slot, cpu, slo, pid, start_time, fixed});
    else
        *existing = {generation, slot, cpu, slo, pid, start_time, fixed};
}

void scheduler_unregister_workers(uint64_t generation)
{
    if (!(settings().shadow || settings().active))
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
    if (config.mode && strcmp(config.mode, "off") && !config.shadow && !config.admit_only && !config.manual && !config.active) {
        fprintf(stderr, "ETRAN_SCHED_MODE must be off, shadow, admit_only, manual_actuate, or active\n");
        return -EINVAL;
    }
    if (config.probe_enabled && !config.qid_enabled) {
        fprintf(stderr, "ETRAN_SOFTIRQ_PROBE=1 requires ETRAN_QID_STATS=1 in off mode\n");
        return -EINVAL;
    }
    if (!config.qid_enabled && !config.probe_enabled)
        return 0;
    if (!config.sample) {
        if (config.shadow || config.admit_only || config.manual || config.active || config.probe_enabled) {
            fprintf(stderr, "ETRAN_QID_STATS_SAMPLE=0 is only for counters-only off-mode comparisons\n");
            return -EINVAL;
        }
        return 0;
    }
    Thresholds limits;
    if (config.active && !active_limits.load()) {
        fprintf(stderr, "Invalid active scheduler cap or fold mode\n");
        return -EINVAL;
    }
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
