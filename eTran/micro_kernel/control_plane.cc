#include <assert.h>
#include <errno.h>
#include <ifaddrs.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <mutex>
#include <optional>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <climits>
#include <deque>
#include <thread>
#include <utility>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <linux/if_link.h>

#include <base/ipc.h>
#include <shm/shm_wrapper.h>
#include <intf/intf.h>
#include <intf/intf_ebpf.h>

#include <runtime/app_if.h>
#include <runtime/defs.h>
#include <runtime/ebpf_if.h>
#include <runtime/tcp.h>
#include <runtime/homa.h>
#include <utils/utils.h>
#include <list>
#include <linux/fs.h>
#include <fstream>
#include <sstream>

#include "ebpf.h"
#include "trans_ebpf.h"
#include "nic.h"
#include "scheduler.h"
#include "irq_migration_io.h"

#define TICK_US 1000
#include <regex>      // std::regex、std::smatch、std::regex_search
#include <fstream>    // std::ifstream

/* Unidx domain socket for application enrollment */
int uds_sockfd;
/* epoll fd for application enrollment */
static int epfd;
/* list of active applications */
static struct apps_info active_apps;
/* epoll fd for receving packets from control path XSKs */
static int xsk_epfd;
/* track key/value in BPF_MAP_TYPE_XSKMAP */
static std::map<int, int> tcp_monitor_xsk_map;
static std::map<int, int> homa_monitor_xsk_map;
/* available ports in [PORT_MIN, PORT_MAX] */
static std::list<uint16_t> available_ports;
/* track status of port */
static std::unordered_map<uint16_t, bool> port_is_available;
static pthread_t micro_kernel_thread;
static std::atomic<bool> control_stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);

static std::unordered_map<uint16_t, app_ctx *> sport_to_actx,
                                               dport_to_actx;

static std::vector<app_ctx *> polled_actx;
#ifdef ALLOCATION
static int latencyLoadThreshold=60;
static int throughputLoadThreshold=80;
static int nxtThroughputIdx=0, nxtLatencyIdx=0;
std::set<int, std::greater<int>>  availableCores;
std::vector<int> latUsingCores;
std::vector<int> throughputUsingCores;
int cpuUseRef[MAX_CPU]={0}, cpuLatUseRef[MAX_CPU]={0};//一个cpu被lat使用了之后就不会再分配给throughput使用
static int cpuIrqRefs[MAX_CPU];
static int cpuThroughputAppRefs[MAX_CPU];
static bool scheduler_admission = false;
static bool scheduler_degrade = false;
static bool scheduler_manual = false;
static unsigned int quarantine_refs[MAX_CPU];
static std::array<std::atomic<uint64_t>, MAX_NIC_QUEUES> irq_owner_tokens{};
enum class CpuRole { Irq, App };
static int cpuUsage[MAX_CPU]={0};//CPU利用率，80%在这里表示为80
static std::array<std::atomic<int>, MAX_CPU> cpu_usage_sample{};
//默认前一半的CPU用于throughput，之后2个给latency，之后全部放入available中

struct cpuOverheadInfo {
    long long user;//normal processes executing in user mode
    long long nice;//niced processes executing in user mode 
    long long system;//processes executing in kernel mode
    long long idle;//twiddling thumbs
    long long iowait;//waiting for I/O to complete
    long long irq;//servicing interrupts
    long long softirq;//servicing softirqs
};
#endif

// micro_kernel.cc
extern class eTranNIC *etran_nic;

// ebpf.cc
extern class eTranEntrance *etran_entrance;
extern class eTranTCP *etran_tcp;
extern class eTranHoma *etran_homa;

// tcp.cc
extern uint64_t next_tcp_cc_to_tsc;

/* manage Unix domain socket */
static int init_uds_ctx(void);
void destroy_uds_ctx(int sockfd);

/* notify application to exit */
static void kill_app(int pid);

/* add/delete control path XSK fds to BPF_MAP_TYPE_XSKMAP 
 * note: each transport protocol has its own BPF_MAP_TYPE_XSKMAP
 */
static int register_xsk_map(int xsk_fd, int xsk_map_key, int proto);
static void unregister_xsk_map(int xsk_fd, int xsk_map_key, int proto);

/* For each packet redirected to control path XSK:
 * step1. use queue index to get the key in BPF_MAP_TYPE_XSKMAP
 * step2. use the key to get the XSK fd
*/
static int register_slow_path_map(unsigned int qid, int xsk_map_key, int proto);
static void unregister_slow_path_map(unsigned int qid, int proto);

/* manage RSS context for this application */
static int create_rss_context(struct app_ctx *actx, const std::vector<unsigned int> &qids);
static void destroy_rss_context(struct app_ctx *actx);

/* manage application resources */
static int alloc_app_resources(struct register_request &req, int fd, int requested_app_cpu);
static void free_app_resources(struct app_ctx *actx, bool return_queues = true);

/* called when microkernel exits */
static void destroy_all_apps(void);
/* called when application exits */
static void destroy_app(int fd);

/* find application context with fd */
static struct app_ctx *find_actx_with_fd(int fd);

/* accept and create context for new application */
static int accept_app(void);
/* process requests from application */

/* poll events from Unix domain socket */
static int poll_uds(int timeout_ms);
/* poll events from lrpc channel */
static void poll_lrpc(void);
/* poll events from network */
static int poll_network(int timeout_ms);

struct IrqWork {
    uint64_t token;
    unsigned int qid;
    int target;
    int expected_source;
    bool write;
    bool read_counters;
};

struct IrqResult {
    IrqWork operation;
    IrqHandoffSample sample;
    bool attempted = false;
};

struct RssJob {
    uint64_t generation;
    int remove_context;
    std::vector<std::pair<unsigned int, int>> placement;
    std::optional<IrqWork> irq = std::nullopt;
};

struct RssResult {
    uint64_t generation;
    int remove_context;
    int context;
    int error;
    std::optional<IrqResult> irq = std::nullopt;
};

static std::mutex rss_mutex;
static std::condition_variable rss_notify;
static std::deque<RssJob> rss_jobs;
static std::vector<RssResult> rss_results;
static std::thread rss_thread;
static bool rss_stopping;

static void submit_rss_job(RssJob job)
{
    {
        std::lock_guard<std::mutex> lock(rss_mutex);
        rss_jobs.push_back(std::move(job));
    }
    rss_notify.notify_one();
}

static std::vector<RssResult> take_rss_results()
{
    std::vector<RssResult> ready;
    std::lock_guard<std::mutex> lock(rss_mutex);
    ready.swap(rss_results);
    return ready;
}

static void stop_rss_worker()
{
    {
        std::lock_guard<std::mutex> lock(rss_mutex);
        rss_stopping = true;
    }
    rss_notify.notify_one();
    if (rss_thread.joinable())
        rss_thread.join();
}

/* dump ebpf maps */
static void dump_xsk_map(int proto);
static void dump_homa_port_map(void);
static void dump_homa_rpc_map(void);
static void dump_tcp_conn_map(void);

static void kill_app(int pid)
{
    kill(pid, SIGUSR1);
}

static int init_uds_ctx(void)
{
    int sockfd;
    struct sockaddr_un addr;
    mode_t old_mask;

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket failed\n");
        goto err;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MICRO_KERNEL_SOCK_PATH.c_str(), sizeof(addr.sun_path) - 1);

    old_mask = umask(0);
    unlink(MICRO_KERNEL_SOCK_PATH.c_str());
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) < 0)
    {
        perror("bind failed\n");
        goto err_close_fd;
    }
    umask(old_mask);

    if (listen(sockfd, MAX_SUPPORT_APP) < 0)
    {
        perror("listen failed\n");
        goto err_close_fd;
    }

    return sockfd;

err_close_fd:
    close(sockfd);
err:
    return -errno;
}

void destroy_uds_ctx(int sockfd)
{
    unlink(MICRO_KERNEL_SOCK_PATH.c_str());
    close(sockfd);
}

/* try to allocate port for application */
int alloc_port(uint16_t port)
{
    if (port < PORT_MIN || port > PORT_MAX)
        return -EINVAL;

    if (!port_is_available[port])
        return -EINVAL;

    available_ports.remove(port);

    port_is_available[port] = false;

    return 0;
}

/* try to allocate random port for application */
int alloc_port(void)
{
    uint16_t port;

    if (available_ports.empty())
        return -1;

    port = available_ports.front();
    available_ports.pop_front();
    assert(port_is_available[port]);
    port_is_available[port] = false;

    return port;
}

int free_port(uint16_t port)
{
    if (port < PORT_MIN || port > PORT_MAX)
        return -EINVAL;

    if (port_is_available[port])
        return -EINVAL;

    port_is_available[port] = true;
    available_ports.push_back(port);

    return 0;
}

int record_port(struct app_ctx *actx, uint16_t local_port, uint16_t remote_port)
{
    actx->ports.insert(local_port);

    if (actx->proto != IPPROTO_TCP)
        return 0;

    std::string cmd;
    std::string res;

    /* only support TCP */
    if (!remote_port && dport_to_actx.find(local_port) == dport_to_actx.end()){
        cmd = "ethtool -U " + etran_nic->_if_name + " flow-type tcp4 dst-port " + std::to_string(local_port) + " context " + std::to_string(actx->rss_ctx_id);
        dport_to_actx[local_port] = actx;
    }
    else if (sport_to_actx.find(remote_port) == sport_to_actx.end()){
        cmd = "ethtool -U " + etran_nic->_if_name + " flow-type tcp4 src-port " + std::to_string(remote_port) + " context " + std::to_string(actx->rss_ctx_id);
        sport_to_actx[remote_port] = actx;
    }
    if (cmd.empty())
        return 0;
    exec_cmd(cmd, res);
    if (res.empty())
    {
        fprintf(stderr, "Failed to configure NIC flow director(%s)\n", cmd.c_str());
        return -1;
    }

    size_t pos = res.find("Added rule with ID ");
    if (pos != std::string::npos)
    {
        actx->flow_director_rules.push_back({std::stoi(res.substr(pos + 19)), remote_port ? remote_port : local_port, remote_port ? true : false});
    }
    else
    {
        fprintf(stderr, "The required pattern was not found.\n");
        return -1;
    }

    std::cout << cmd << std::endl;

    return 0;
}

int unrecord_port(struct app_ctx *actx, uint16_t port)
{
    actx->ports.erase(port);
    if (actx->proto != IPPROTO_TCP)
        return 0;
    for (auto it = actx->flow_director_rules.begin(); it != actx->flow_director_rules.end(); it++)
    {
        std::string cmd;
        std::string res;
        cmd = "ethtool -U " + etran_nic->_if_name + " delete " + std::to_string(it->id);
        if (it->source)
            sport_to_actx.erase(it->port);
        else
            dport_to_actx.erase(it->port);
        std::cout << cmd << std::endl;
        exec_cmd(cmd);
    }

    actx->flow_director_rules.clear();

    return 0;
}

static bool delete_tcp_rss_context(int id)
{
    std::string output;
    if (!exec_cmd("ethtool -X " + etran_nic->_if_name + " delete context " + std::to_string(id), output, true))
        return false;
    printf("Destroy RSS context success: %d\n", id);
    return true;
}

static void destroy_rss_context(struct app_ctx *actx)
{
    if (actx->rss_ctx_id && actx->proto == IPPROTO_TCP && delete_tcp_rss_context(actx->rss_ctx_id))
        actx->rss_ctx_id = 0;
}

#ifdef ALLOCATION
// 读取 /proc/stat 的每核（cpu0..）计数，跳过汇总行 "cpu"
static bool read_cpu_times(std::map<int, cpuOverheadInfo> &out) {
    std::ifstream fin("/proc/stat");
    if (!fin) return false;

    out.clear();
    std::string line, label;
    while (std::getline(fin, line)) {
        if (line.rfind("cpu", 0) != 0) break;  // 非 cpu 开头的行，结束
        std::istringstream iss(line);
        iss >> label;
        if (label == "cpu") continue;          // 跳过汇总

        const char *id = label.c_str() + 3;
        char *end;
        long cpu = strtol(id, &end, 10);
        if (end == id || *end || cpu < 0 || cpu >= MAX_CPU)
            continue;

        cpuOverheadInfo t;
        if (iss >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq)
            out.emplace(cpu, t);
    }
    return !out.empty();
}

static inline unsigned long long sum_all(const cpuOverheadInfo &x) {
    return (unsigned long long)x.user + x.nice + x.system + x.idle +
           x.iowait + x.irq + x.softirq;
}

static inline unsigned long long sum_idle(const cpuOverheadInfo &x) {
    return (unsigned long long)x.idle + x.iowait; // 计算 idleAll
}

static void sample_legacy_cpu_load(std::map<int, cpuOverheadInfo> &previous) {
    std::map<int, cpuOverheadInfo> current;
    if (!read_cpu_times(current))
        return;

    for (const auto &[cpu, before] : previous) {
        auto it = current.find(cpu);
        if (it == current.end())
            continue;
        const auto &after = it->second;
        unsigned long long ta = sum_all(before);
        unsigned long long tb = sum_all(after);
        unsigned long long ia = sum_idle(before);
        unsigned long long ib = sum_idle(after);

        unsigned long long dt = (tb > ta) ? (tb - ta) : 0ULL;
        unsigned long long di = (ib > ia) ? (ib - ia) : 0ULL;

        const int usage = dt > di ? ((dt - di) * 100 + dt / 2) / dt : 0;
        cpu_usage_sample[cpu].store(usage, std::memory_order_relaxed);
    }
    previous.swap(current);
}

int get_least_loaded_cpu(const std::set<int> *candidate, int *cpuId, int *cpuOverhead) {
    int minLoad = 100;
    for (int cpu : *candidate) {
        if (cpuUsage[cpu] < minLoad) {
            minLoad = cpuUsage[cpu];
            *cpuId = cpu;
            *cpuOverhead = minLoad;
        }
    }
    return (minLoad < 100) ? 0 : -1;
}

static bool valid_pool_cpu(int cpu)
{
    const auto &pool = etran_nic->_cpu_topology.pool;
    return std::binary_search(pool.begin(), pool.end(), cpu);
}

static int reserve_cpu(int type, CpuRole role, int cpu)
{
    if (!valid_pool_cpu(cpu))
        return -EINVAL;
    if (quarantine_refs[cpu])
        return -EBUSY;
    availableCores.erase(cpu);
    if (type == LATENCY) {
        throughputUsingCores.erase(std::remove(throughputUsingCores.begin(), throughputUsingCores.end(), cpu), throughputUsingCores.end());
        if (std::find(latUsingCores.begin(), latUsingCores.end(), cpu) == latUsingCores.end())
            latUsingCores.push_back(cpu);
        cpuLatUseRef[cpu]++;
    } else if (cpuLatUseRef[cpu] == 0 && std::find(throughputUsingCores.begin(), throughputUsingCores.end(), cpu) == throughputUsingCores.end()) {
        throughputUsingCores.push_back(cpu);
    }
    cpuUseRef[cpu]++;
    if (role == CpuRole::Irq)
        ++cpuIrqRefs[cpu];
    else if (type != LATENCY)
        ++cpuThroughputAppRefs[cpu];
    return cpu;
}

static void release_cpu(const app_ctx &app, CpuRole role, int cpu)
{
    if (role == CpuRole::Irq) {
        assert(cpuIrqRefs[cpu] > 0);
        --cpuIrqRefs[cpu];
    } else if (app.tcp_type != LATENCY) {
        assert(cpuThroughputAppRefs[cpu] > 0);
        --cpuThroughputAppRefs[cpu];
    }
    assert(cpuUseRef[cpu] > 0);
    --cpuUseRef[cpu];
    if (app.tcp_type == LATENCY) {
        assert(cpuLatUseRef[cpu] > 0);
        --cpuLatUseRef[cpu];
    }
    if (cpuLatUseRef[cpu] == 0) {
        latUsingCores.erase(std::remove(latUsingCores.begin(), latUsingCores.end(), cpu), latUsingCores.end());
        if (cpuUseRef[cpu] == 0) {
            throughputUsingCores.erase(std::remove(throughputUsingCores.begin(), throughputUsingCores.end(), cpu), throughputUsingCores.end());
            availableCores.insert(cpu);
        } else if (std::find(throughputUsingCores.begin(), throughputUsingCores.end(), cpu) == throughputUsingCores.end()) {
            throughputUsingCores.push_back(cpu);
        }
    }
    printf("CPU release pid=%d cpu=%d refs=%d latency_refs=%d available=%d\n",
           app.pid, cpu, cpuUseRef[cpu], cpuLatUseRef[cpu], availableCores.count(cpu) != 0);
}

static void release_app_cpus(struct app_ctx *actx)
{
    // usedCores contains the IRQ reservations first, including on partial failure.
    unsigned int slot = 0;
    for (int cpu : actx->usedCores)
        release_cpu(*actx, slot++ < actx->nr_nic_queues ? CpuRole::Irq : CpuRole::App, cpu);
    actx->usedCores.clear();
}

static int select_cpu(int type, CpuRole role, int requested_app_cpu)
{
    // A throughput poller can become busy immediately after registration.
    // Account for its reserved role even when the load sample still shows idle.
    auto compatible = [&](int cpu) {
        if (quarantine_refs[cpu])
            return false;
        if (role == CpuRole::Irq)
            return cpuThroughputAppRefs[cpu] == 0 &&
                   (type == LATENCY || cpu != requested_app_cpu);
        if (type != LATENCY)
            return cpuIrqRefs[cpu] == 0;
        return cpuThroughputAppRefs[cpu] == 0;
    };

    bool latency = type == LATENCY;
    int threshold = latency ? latencyLoadThreshold : throughputLoadThreshold;
    auto pick_used = [&](const std::vector<int> &pool, int &next, bool allow_overload = false) {
        for (size_t i = 0; i < pool.size(); ++i) {
            size_t index = (next + i) % pool.size();
            int cpu = pool[index];
            if (compatible(cpu) && (allow_overload || cpuUsage[cpu] < threshold)) {
                next = (index + 1) % pool.size();
                return cpu;
            }
        }
        return -1;
    };

    int cpu = latency ? pick_used(latUsingCores, nxtLatencyIdx) : -1;
    if (cpu < 0)
        cpu = pick_used(throughputUsingCores, nxtThroughputIdx);
    if (cpu >= 0)
        return cpu;

    for (int free_cpu : availableCores) {
        if (compatible(free_cpu))
            return free_cpu;
    }

    // Preserve the existing overcommit policy when the eligible pool is full.
    auto &pool = latency ? latUsingCores : throughputUsingCores;
    int &next = latency ? nxtLatencyIdx : nxtThroughputIdx;
    cpu = pick_used(pool, next, true);
    if (cpu >= 0)
        return cpu;
    if (!availableCores.empty())
        return *availableCores.begin();
    if (pool.empty())
        return -ENOSPC;
    cpu = pool[next % pool.size()];
    next = (next + 1) % pool.size();
    return cpu;
}

static int allocate_cpu(const app_ctx *actx, CpuRole role, int requested_cpu)
{
    int cpu = requested_cpu;
    if (cpu < 0)
        cpu = select_cpu(actx->tcp_type, role, actx->requested_app_cpu);
    return cpu < 0 ? cpu : reserve_cpu(actx->tcp_type, role, cpu);
}

static int place_new_tcp_app(app_ctx &app, int fixed_irq)
{
    if ((fixed_irq >= 0 && quarantine_refs[fixed_irq]) ||
        (app.requested_app_cpu >= 0 && quarantine_refs[app.requested_app_cpu]))
        return -EBUSY;
    auto free = availableCores;
    std::vector<int> irq_cpus, app_cpus;
    bool degraded = false;
    auto take = [&free](int fixed, bool irq) {
        if (fixed >= 0) {
            free.erase(fixed);
            return fixed;
        }
        if (free.empty())
            return -1;
        const int cpu = irq ? *free.begin() : *free.rbegin();
        free.erase(cpu);
        return cpu;
    };

    // Exclude an explicit IRQ target before choosing an unpinned APP slot.
    if (fixed_irq >= 0)
        free.erase(fixed_irq);
    for (unsigned i = 0; i < app.nr_app_threads; ++i) {
        const int cpu = take(app.requested_app_cpu, false);
        if (cpu < 0)
            return -ENOSPC;
        app_cpus.push_back(cpu);
    }
    for (unsigned i = 0; i < app.nr_nic_queues; ++i) {
        int cpu = take(fixed_irq, true);
        if (cpu < 0 && app.nr_nic_queues == 1 && app.nr_app_threads == 1) {
            const int app_cpu = app_cpus.front();
            if ((app.tcp_type == LATENCY || app.tcp_type == AUTO) && !cpuUseRef[app_cpu])
                cpu = app_cpu;
            if (app.tcp_type == THROUGHPUT) {
                // The LAT-first three-core transient is limited to an unstacked IRQ-only LAT core.
                for (const auto *other : active_apps.apps) {
                    if (!other->resources_ready || other->proto != IPPROTO_TCP || other->use_shared_queues ||
                        other->tcp_type != LATENCY || other->nr_nic_queues != 1 || other->nr_app_threads != 1)
                        continue;
                    const int candidate = other->usedCores.front();
                    if (candidate == app_cpu || quarantine_refs[candidate] ||
                        cpuUseRef[candidate] != 1 || cpuIrqRefs[candidate] != 1)
                        continue;
                    if (other->ports.empty() || scheduler_queue_is_light(other->nic_qid[0])) {
                        cpu = candidate;
                        break;
                    }
                }
                if (cpu < 0 && scheduler_degrade && !cpuUseRef[app_cpu]) {
                    cpu = app_cpu;
                    degraded = true;
                }
            }
        }
        if (cpu < 0)
            return -ENOSPC;
        irq_cpus.push_back(cpu);
    }
    for (int cpu : irq_cpus)
        app.usedCores.push_back(reserve_cpu(app.tcp_type, CpuRole::Irq, cpu));
    for (int cpu : app_cpus)
        app.usedCores.push_back(reserve_cpu(app.tcp_type, CpuRole::App, cpu));
    if (degraded)
        fprintf(stderr, "sched_admission app_gen=%llu state=DEGRADED reason=tp_self_fold\n",
                static_cast<unsigned long long>(app.generation));
    return 0;
}

static int bind_rss_core(struct app_ctx *actx, const std::vector<unsigned int> &qids, bool deferred) {
    int irq_cpu = -1;
    const char *fixed_cpu = std::getenv(actx->tcp_type == LATENCY ? "ETRAN_LATENCY_CPU" : "ETRAN_THROUGHPUT_CPU");
    if (fixed_cpu) {
        char *end = nullptr;
        errno = 0;
        long cpu = strtol(fixed_cpu, &end, 10);
        if (errno || end == fixed_cpu || *end || cpu < 0 || cpu >= MAX_CPU || !valid_pool_cpu(cpu))
            return -EINVAL;
        irq_cpu = cpu;
    }
    if (scheduler_admission && deferred) {
        if (int rc = place_new_tcp_app(*actx, irq_cpu))
            return rc;
        auto cpu = actx->usedCores.begin();
        for (auto qid : qids) {
            printf("CPU allocation pid=%d role=IRQ queue=%u cpu=%d refs=%d latency_refs=%d\n",
                   actx->pid, qid, *cpu, cpuUseRef[*cpu], cpuLatUseRef[*cpu]);
            ++cpu;
        }
        for (unsigned i = 0; cpu != actx->usedCores.end(); ++i, ++cpu)
            printf("CPU allocation pid=%d role=APP thread=%u cpu=%d refs=%d latency_refs=%d\n",
                   actx->pid, i, *cpu, cpuUseRef[*cpu], cpuLatUseRef[*cpu]);
        return 0;
    }
    if (irq_cpu < 0 || actx->requested_app_cpu < 0)
        for (int cpu : etran_nic->_cpu_topology.pool)
            cpuUsage[cpu] = cpu_usage_sample[cpu].load(std::memory_order_relaxed);
    for (unsigned int qid : qids) {
        int cpu = allocate_cpu(actx, CpuRole::Irq, irq_cpu);
        if (cpu < 0)
            return cpu;
        actx->usedCores.push_back(cpu);
        if (!deferred)
            if (int err = etran_nic->set_queue_cpu(qid, cpu))
                return err;
        printf("CPU allocation pid=%d role=IRQ queue=%u cpu=%d refs=%d latency_refs=%d\n",
               actx->pid, qid, cpu, cpuUseRef[cpu], cpuLatUseRef[cpu]);
    }
    for (unsigned int i = 0; i < actx->nr_app_threads; i++) {
        int cpu = allocate_cpu(actx, CpuRole::App, actx->requested_app_cpu);
        if (cpu < 0)
            return cpu;
        actx->usedCores.push_back(cpu);
        printf("CPU allocation pid=%d role=APP thread=%u cpu=%d refs=%d latency_refs=%d\n",
               actx->pid, i, cpu, cpuUseRef[cpu], cpuLatUseRef[cpu]);
    }
    return 0;
}

struct WorkerMigrationRequest {
    int pid;
    unsigned int slot;
    int cpu;
    uint64_t start_time;
};

struct IrqMigrationRequest {
    unsigned int qid;
    int cpu;
    uint64_t generation;
};

static std::mutex migration_request_mutex;
static std::optional<WorkerMigrationRequest> migration_request;
static std::optional<IrqMigrationRequest> irq_migration_request;

struct WorkerTaskState {
    uint64_t start_time = 0;
    int processor = -1;
    char state = 0;
};

static std::string worker_task_path(const app_ctx &app, int tid)
{
    return "/proc/" + std::to_string(app.pid) + "/task/" + std::to_string(tid) + '/';
}

static WorkerTaskState read_worker_state(const std::string &path)
{
    std::ifstream stat(path + "stat");
    std::string line, field;
    WorkerTaskState result{};
    if (!std::getline(stat, line))
        return result;
    const auto comm_end = line.rfind(')');
    if (comm_end == std::string::npos)
        return result;
    std::istringstream fields(line.substr(comm_end + 1));
    for (unsigned i = 3; i < 39; ++i) {
        if (!(fields >> field))
            return {};
        if (i == 3)
            result.state = field.front();
        if (i == 22)
            result.start_time = strtoull(field.c_str(), nullptr, 10);
    }
    if (!(fields >> result.processor) || result.state == 'Z' || result.state == 'X')
        return {};
    return result;
}

static bool worker_is_on(const app_ctx &app, const app_ctx_per_thread &worker, int cpu)
{
    const auto path = worker_task_path(app, worker.os_tid);
    std::ifstream status(path + "status");
    std::string line;
    const std::string expected = std::to_string(cpu);
    bool mask_matches = false;
    while (std::getline(status, line)) {
        if (line.compare(0, 18, "Cpus_allowed_list:") != 0)
            continue;
        const auto first = line.find_first_not_of(" \t", 18);
        mask_matches = first != std::string::npos && line.substr(first) == expected;
        break;
    }
    if (!mask_matches)
        return false;
    const auto task = read_worker_state(path);
    return task.start_time == worker.os_start_time && (task.state != 'R' || task.processor == cpu);
}

class MigrationExecutor {
    using Clock = std::chrono::steady_clock;
    enum class State { Migrating, RollingBack };
    struct Move {
        app_ctx *app;
        unsigned int slot;
        int src, dst;
        State state;
        uint32_t seq;
        Clock::time_point published;
        bool wake_queued;
    };
    struct IrqMove {
        app_ctx *app;
        unsigned int slot, qid;
        int src, dst;
        uint64_t token = 0;
        State state = State::Migrating;
        bool pending_io = false;
        bool cancelled = false;
        bool initially_idle = false;
        IrqHandoffSample base{};
        Clock::time_point began{}, next_sample{};
    };
    std::optional<Move> action;
    std::optional<IrqMove> irq_action;
    std::vector<Move> quarantined;
    std::vector<IrqMove> irq_holds;
    Clock::time_point backoff_until{};
    uint32_t next_seq = 0;
    uint64_t next_irq_token = 0;

    static int &worker_cpu(const Move &move)
    {
        auto it = move.app->usedCores.begin();
        std::advance(it, move.app->nr_nic_queues + move.slot);
        return *it;
    }

    void publish(Move &move, State state, int cpu)
    {
        auto *ctrl = move.app->tctx[move.slot].sched_ctrl;
        if (++next_seq == 0)
            ++next_seq;
        move.seq = next_seq;
        move.state = state;
        move.published = Clock::now();
        move.wake_queued = false;
        const uint64_t command = (static_cast<uint64_t>(move.seq) << 32) | static_cast<uint32_t>(cpu);
        __atomic_store_n(&ctrl->cmd_word, command, __ATOMIC_SEQ_CST);
        fprintf(stderr, "sched_migration state=%s pid=%d slot=%u seq=%u from=%d to=%d\n",
                state == State::Migrating ? "MIGRATING" : "ROLLING_BACK",
                move.app->pid, move.slot, move.seq, state == State::Migrating ? move.src : move.dst, cpu);
    }

    static int &irq_cpu(const IrqMove &move)
    {
        auto it = move.app->usedCores.begin();
        std::advance(it, move.slot);
        return *it;
    }

    static void send_irq_io(IrqMove &move, bool write)
    {
        const bool rollback = move.state == State::RollingBack;
        const int target = rollback ? move.src : move.dst;
        const int expected = write && !rollback ? move.src : -1;
        move.pending_io = true;
        submit_rss_job({move.app->generation, 0, {},
                        IrqWork{move.token, move.qid, target, expected, write, !move.initially_idle}});
    }

    void begin_irq(IrqMove &move, State state)
    {
        move.token = ++next_irq_token;
        move.state = state;
        move.began = Clock::now();
        move.initially_idle = scheduler_irq_progress(move.qid, move.src, move.dst).idle;
        fprintf(stderr, "sched_migration kind=IRQ state=%s app_gen=%llu qid=%u token=%llu src=%d dst=%d\n",
                state == State::Migrating ? "MIGRATING" : "ROLLING_BACK",
                static_cast<unsigned long long>(move.app->generation), move.qid,
                static_cast<unsigned long long>(move.token), move.src, move.dst);
        send_irq_io(move, true);
    }

    void hold_irq(IrqMove move, const char *state)
    {
        ++quarantine_refs[move.src];
        ++quarantine_refs[move.dst];
        irq_holds.push_back(move);
        fprintf(stderr, "sched_migration kind=IRQ state=%s app_gen=%llu qid=%u token=%llu src=%d dst=%d\n",
                state, static_cast<unsigned long long>(move.app->generation), move.qid,
                static_cast<unsigned long long>(move.token), move.src, move.dst);
        irq_action.reset();
    }

    void poll_irq()
    {
        if (!irq_action || irq_action->pending_io)
            return;
        auto &move = *irq_action;
        const auto now = Clock::now();
        if (now - move.began >= std::chrono::milliseconds(150)) {
            if (move.state == State::Migrating)
                begin_irq(move, State::RollingBack);
            else
                hold_irq(move, "QUARANTINED");
        } else if (now >= move.next_sample) {
            send_irq_io(move, false);
        }
    }

public:
    int start(WorkerMigrationRequest request)
    {
        if (action || irq_action || Clock::now() < backoff_until)
            return -EBUSY;
        const auto it = std::find_if(active_apps.apps.begin(), active_apps.apps.end(), [&](const auto *app) {
            return app->done && app->fd >= 0 && app->pid == request.pid &&
                   app->proto == IPPROTO_TCP && !app->use_shared_queues;
        });
        if (it == active_apps.apps.end())
            return -ENOENT;
        auto *app = *it;
        if (read_worker_state(worker_task_path(*app, request.pid)).start_time != request.start_time)
            return -ESTALE;
        if (request.slot >= app->nr_app_threads || !valid_pool_cpu(request.cpu))
            return -EINVAL;
        auto &worker = app->tctx[request.slot];
        if (!worker.os_tid || !worker.os_start_time)
            return -EAGAIN;
        Move move{app, request.slot, -1, request.cpu, State::Migrating, 0, {}, false};
        move.src = worker_cpu(move);
        if (move.src == move.dst)
            return -EALREADY;
        if (quarantine_refs[move.src] || quarantine_refs[move.dst])
            return -EBUSY;
        const bool own_light_irq = app->nr_nic_queues == 1 && app->nr_app_threads == 1 &&
                                   app->tcp_type != THROUGHPUT && app->usedCores.front() == move.dst &&
                                   cpuUseRef[move.dst] == 1 && scheduler_queue_is_light(app->nic_qid[0]);
        if (cpuUseRef[move.dst] && !own_light_irq)
            return -EBUSY;
        if (!worker_is_on(*app, worker, move.src))
            return -ESTALE;
        if (int rc = reserve_cpu(app->tcp_type, CpuRole::App, move.dst); rc < 0)
            return rc;
        action = move;
        publish(*action, State::Migrating, move.dst);
        return 0;
    }

    int start(IrqMigrationRequest request)
    {
        if (action || irq_action || Clock::now() < backoff_until)
            return -EBUSY;
        if (!valid_pool_cpu(request.cpu) || request.qid >= etran_nic->_num_queues)
            return -EINVAL;
        for (auto *app : active_apps.apps) {
            if (app->generation != request.generation || !app->done || app->fd < 0 ||
                app->proto != IPPROTO_TCP || app->use_shared_queues)
                continue;
            for (unsigned int slot = 0; slot < app->nr_nic_queues; ++slot) {
                if (app->nic_qid[slot] != request.qid)
                    continue;
                IrqMove move{app, slot, request.qid, -1, request.cpu};
                move.src = irq_cpu(move);
                if (move.src == move.dst)
                    return -EALREADY;
                if (quarantine_refs[move.src] || quarantine_refs[move.dst])
                    return -EBUSY;
                const bool own_app = app->tcp_type != THROUGHPUT && app->nr_nic_queues == 1 &&
                                     app->nr_app_threads == 1 && app->usedCores.back() == move.dst &&
                                     cpuUseRef[move.dst] == 1 && scheduler_queue_is_light(move.qid);
                if (cpuUseRef[move.dst] && !own_app)
                    return -EBUSY;
                if (int rc = reserve_cpu(app->tcp_type, CpuRole::Irq, move.dst); rc < 0)
                    return rc;
                irq_action = move;
                begin_irq(*irq_action, State::Migrating);
                return 0;
            }
        }
        return -ENOENT;
    }

    void on_irq_result(const RssResult &result)
    {
        assert(result.irq);
        const auto &io = *result.irq;
        assert(irq_action && irq_action->token == io.operation.token &&
               irq_action->app->generation == result.generation);
        auto &move = *irq_action;
        move.pending_io = false;
        if (move.cancelled) {
            hold_irq(move, "CANCELLED_QUIESCENT");
            return;
        }
        const bool rollback = move.state == State::RollingBack;
        move.next_sample = Clock::now() + std::chrono::milliseconds(5);
        if (io.operation.write) {
            move.base = io.sample;
            if (!result.error)
                return;
            if (rollback) {
                hold_irq(move, "QUARANTINED");
            } else if (io.attempted) {
                begin_irq(move, State::RollingBack);
            } else if (io.sample.configured == move.src && io.sample.effective == move.src) {
                release_cpu(*move.app, CpuRole::Irq, move.dst);
                fprintf(stderr, "sched_migration kind=IRQ state=REJECTED qid=%u error=%d\n", move.qid, result.error);
                irq_action.reset();
            } else
                hold_irq(move, "QUARANTINED");
            return;
        }
        if (result.error)
            return;
        const auto &current = io.sample;
        const int target = rollback ? move.src : move.dst;
        if (current.timeouts != move.base.timeouts || current.done < move.base.done) {
            if (rollback)
                hold_irq(move, "QUARANTINED");
            else
                begin_irq(move, State::RollingBack);
            return;
        }
        if (current.configured != target || current.effective != target)
            return;
        const auto elapsed = Clock::now() - move.began;
        const auto progress = scheduler_irq_progress(move.qid, move.src, move.dst);
        if (!(current.done > move.base.done || (move.initially_idle && progress.idle) ||
              (elapsed >= std::chrono::milliseconds(100) && progress.quiet)))
            return;

        if (rollback) {
            release_cpu(*move.app, CpuRole::Irq, move.dst);
            backoff_until = Clock::now() + std::chrono::seconds(1);
        } else {
            irq_cpu(move) = move.dst;
            scheduler_update_irq_cpu(move.qid, move.app->pid, move.dst);
            release_cpu(*move.app, CpuRole::Irq, move.src);
        }
        fprintf(stderr, "sched_migration kind=IRQ state=CONFIRM qid=%u cpu=%d rollback=%d done_delta=%llu elapsed_us=%lld\n",
                move.qid, target, rollback, static_cast<unsigned long long>(current.done - move.base.done),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
        irq_action.reset();
    }

    void poll()
    {
        poll_irq();
        if (!action)
            return;
        auto &move = *action;
        auto &worker = move.app->tctx[move.slot];
        const auto elapsed = Clock::now() - move.published;
        const bool rolling_back = move.state == State::RollingBack;
        const int target = rolling_back ? move.src : move.dst;
        const uint64_t reply = __atomic_load_n(&worker.sched_ctrl->ack_word, __ATOMIC_ACQUIRE);
        const bool replied = (reply >> 32) == move.seq;
        const uint32_t error = static_cast<uint32_t>(reply);

        if (replied && !error && worker_is_on(*move.app, worker, target)) {
            if (rolling_back) {
                release_cpu(*move.app, CpuRole::App, move.dst);
                backoff_until = Clock::now() + std::chrono::seconds(1);
            } else {
                worker_cpu(move) = move.dst;
                if (move.app->nr_app_threads == 1)
                    for (unsigned int i = 0; i < move.app->nr_nic_queues; ++i)
                        scheduler_update_app_cpu(move.app->nic_qid[i], move.app->pid, move.dst);
                release_cpu(*move.app, CpuRole::App, move.src);
            }
            fprintf(stderr, "sched_migration state=CONFIRM pid=%d slot=%u seq=%u cpu=%d rollback=%d elapsed_us=%lld\n",
                    move.app->pid, move.slot, move.seq, target, rolling_back,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
            action.reset();
            return;
        }
        if (!rolling_back && ((replied && error) || elapsed >= std::chrono::milliseconds(50))) {
            publish(move, State::RollingBack, move.src);
            return;
        }
        if (rolling_back && elapsed >= std::chrono::milliseconds(150)) {
            ++quarantine_refs[move.src];
            ++quarantine_refs[move.dst];
            quarantined.push_back(move);
            fprintf(stderr, "sched_migration state=QUARANTINED pid=%d slot=%u seq=%u src=%d dst=%d\n",
                    move.app->pid, move.slot, move.seq, move.src, move.dst);
            action.reset();
            return;
        }
        if (!move.wake_queued && !replied &&
            ((__atomic_load_n(&worker.sched_ctrl->state_flags, __ATOMIC_SEQ_CST) & WORKER_FLAG_SLEEPING) ||
             elapsed >= std::chrono::milliseconds(5))) {
            lrpc_msg message{};
            message.cmd = APPIN_TCP_SCHED_REBIND;
            if (lrpc_send(&worker.kernel_out, &message) == 0) {
                kick_evfd(worker.evfd);
                move.wake_queued = true;
            }
        }
    }

    void cancel(const app_ctx &app)
    {
        if (irq_action && irq_action->app == &app) {
            irq_action->cancelled = true;
            if (!irq_action->pending_io)
                hold_irq(*irq_action, "CANCELLED_QUIESCENT");
        }
        if (!action || action->app != &app)
            return;
        fprintf(stderr, "sched_migration state=CANCELLED pid=%d slot=%u seq=%u\n",
                app.pid, action->slot, action->seq);
        // Keep the extra reservation until the resource owner finishes teardown.
        ++quarantine_refs[action->src];
        ++quarantine_refs[action->dst];
        quarantined.push_back(*action);
        action.reset();
    }

    void release(const app_ctx &app)
    {
        assert(!irq_action || irq_action->app != &app);
        for (const auto &move : irq_holds) {
            if (move.app != &app)
                continue;
            --quarantine_refs[move.src];
            --quarantine_refs[move.dst];
            release_cpu(app, CpuRole::Irq, move.dst);
        }
        irq_holds.erase(std::remove_if(irq_holds.begin(), irq_holds.end(), [&](const auto &move) {
            return move.app == &app;
        }), irq_holds.end());
        for (const auto &move : quarantined) {
            if (move.app != &app)
                continue;
            --quarantine_refs[move.src];
            --quarantine_refs[move.dst];
            release_cpu(app, CpuRole::App, move.dst);
        }
        quarantined.erase(std::remove_if(quarantined.begin(), quarantined.end(), [&](const auto &move) {
            return move.app == &app;
        }), quarantined.end());
    }
};

static MigrationExecutor migration_executor;

static void poll_worker_migration()
{
    std::optional<WorkerMigrationRequest> request;
    std::optional<IrqMigrationRequest> irq;
    {
        std::lock_guard<std::mutex> lock(migration_request_mutex);
        request.swap(migration_request);
        irq.swap(irq_migration_request);
    }
    if (irq)
        if (const int rc = migration_executor.start(*irq))
            fprintf(stderr, "sched_migration kind=IRQ state=REJECTED qid=%u to=%d error=%d\n",
                    irq->qid, irq->cpu, rc);
    if (request) {
        if (const int rc = migration_executor.start(*request))
            fprintf(stderr, "sched_migration state=REJECTED pid=%d slot=%u to=%d error=%d\n",
                    request->pid, request->slot, request->cpu, rc);
    }
    migration_executor.poll();
}
#endif

int request_worker_migration(int pid, unsigned int slot, int cpu)
{
#ifdef ALLOCATION
    if (!scheduler_manual)
        return -EOPNOTSUPP;
    if (pid <= 0 || cpu < 0)
        return -EINVAL;
    const auto start_time = read_worker_state("/proc/" + std::to_string(pid) + '/').start_time;
    if (!start_time)
        return -ENOENT;
    std::lock_guard<std::mutex> lock(migration_request_mutex);
    if (migration_request || irq_migration_request)
        return -EBUSY;
    migration_request = WorkerMigrationRequest{pid, slot, cpu, start_time};
    return 0;
#else
    return -EOPNOTSUPP;
#endif
}

int request_irq_migration(unsigned int qid, int cpu)
{
#ifdef ALLOCATION
    if (!scheduler_manual)
        return -EOPNOTSUPP;
    if (cpu < 0 || qid >= etran_nic->_num_queues)
        return -EINVAL;
    std::lock_guard<std::mutex> lock(migration_request_mutex);
    if (migration_request || irq_migration_request)
        return -EBUSY;
    const auto generation = irq_owner_tokens[qid].load(std::memory_order_acquire);
    if (!generation)
        return -ENOENT;
    irq_migration_request = IrqMigrationRequest{qid, cpu, generation};
    return 0;
#else
    return -EOPNOTSUPP;
#endif
}

static int create_tcp_rss_context(const std::vector<unsigned int> &qids)
{
    std::vector<int> weight(etran_nic->_num_queues);
    for (unsigned int qid : qids) {
        if (qid >= weight.size())
            return -EINVAL;
        weight[qid] = 1;
    }
    // convert weight to string like '1 1 1 0 1 0 0'
    std::string weight_str;
    for (size_t i = 0; i < weight.size(); i++)
    {
        weight_str += std::to_string(weight[i]);
        if (i + 1 != weight.size())
            weight_str += " ";
    }
    std::string res;
    if (!exec_cmd("ethtool -X " + etran_nic->_if_name + " context new weight " + weight_str, res, true)) {
        fprintf(stderr, "Failed to configure NIC flow director\n");
        return -EIO;
    }
    constexpr char prefix[] = "New RSS context is ";
    const size_t pos = res.find(prefix);
    if (pos == std::string::npos)
        return -EIO;
    const char *start = res.c_str() + pos + sizeof(prefix) - 1;
    char *end = nullptr;
    const long id = strtol(start, &end, 10);
    if (end == start || id <= 0 || id > INT_MAX)
        return -EIO;
    printf("Created RSS context: %ld\n", id);
    return static_cast<int>(id);
}

static int create_rss_context(struct app_ctx *actx, const std::vector<unsigned int> &qids)
{
    if (actx->proto != IPPROTO_TCP) {
        // Homa ingress requires the hardware queue and the CPU to match.
        for (unsigned int qid : qids)
            if (int err = etran_nic->set_queue_cpu(qid, qid))
                return err;
        return exec_cmd("ethtool -X " + etran_nic->_if_name + " equal " + std::to_string(qids.size())) ? 0 : -EIO;
    }
    const int id = create_tcp_rss_context(qids);
    if (id < 0)
        return id;
    actx->rss_ctx_id = id;
#if defined(ALLOCATION)
    return bind_rss_core(actx, qids, false);
#else
    return 0;
#endif
}

static RssResult execute_rss_job(const RssJob &job)
{
    RssResult result{job.generation, job.remove_context, 0, 0};
    if (job.irq) {
        const auto &op = *job.irq;
        result.irq = IrqResult{op, {}};
        auto &io = *result.irq;
        result.error = sample_irq_handoff(*etran_nic, op.qid, op.read_counters, io.sample);
        if (result.error || !op.write)
            return result;
        if (op.expected_source >= 0 &&
            (io.sample.configured != op.expected_source || io.sample.effective != op.expected_source)) {
            result.error = -ESTALE;
            return result;
        }
        io.attempted = true;
        result.error = etran_nic->set_queue_cpu(op.qid, op.target);
        return result;
    }
    if (job.remove_context) {
        result.error = delete_tcp_rss_context(job.remove_context) ? 0 : -EIO;
        return result;
    }

    std::vector<unsigned int> qids;
    for (const auto &[qid, cpu] : job.placement) {
        if (cpu >= 0 && (result.error = etran_nic->set_queue_cpu(qid, cpu)))
            return result;
        qids.push_back(qid);
    }
    const int id = create_tcp_rss_context(qids);
    if (id < 0)
        result.error = id;
    else
        result.context = id;
    return result;
}

static void run_rss_worker()
{
#ifdef ALLOCATION
    std::map<int, cpuOverheadInfo> previous;
    using Clock = std::chrono::steady_clock;
    auto sample_at = Clock::now();
#endif
    for (;;) {
        std::unique_lock<std::mutex> lock(rss_mutex);
#ifdef ALLOCATION
        if (!rss_notify.wait_until(lock, sample_at, [] { return rss_stopping || !rss_jobs.empty(); })) {
            lock.unlock();
            sample_legacy_cpu_load(previous);
            sample_at = Clock::now() + std::chrono::milliseconds(100);
            continue;
        }
#else
        rss_notify.wait(lock, [] { return rss_stopping || !rss_jobs.empty(); });
#endif
        if (rss_jobs.empty())
            return;
        auto job = std::move(rss_jobs.front());
        rss_jobs.pop_front();
        lock.unlock();
        const auto result = execute_rss_job(job);
        lock.lock();
        rss_results.push_back(result);
    }
}

static void unregister_xsk_map(int xsk_fd, int xsk_map_key, int proto)
{
    int xsk_map_fd;

    struct xdp_program *xdp_prog;

    if (proto == IPPROTO_HOMA)
        xdp_prog = etran_homa->_ebpf.xdp_prog;
    else
        xdp_prog = etran_tcp->_ebpf.xdp_prog;

    xsk_map_fd = bpf_map__fd(bpf_object__find_map_by_name(xdp_program__bpf_obj(xdp_prog), "xsks_map"));
    if (xsk_map_fd < 0)
    {
        fprintf(stderr, "ERROR: no xsk map found: %s\n", strerror(xsk_map_fd));
        return;
    }

    if (bpf_map_delete_elem(xsk_map_fd, &xsk_map_key))
    {
        fprintf(stderr, "ERROR: delete xsk map key %d failed\n", xsk_map_key);
        return;
    }
    if (proto == IPPROTO_HOMA)
        homa_monitor_xsk_map.erase(xsk_map_key);
    else
        tcp_monitor_xsk_map.erase(xsk_map_key);
    etran_nic->_available_xsk_keys.push_back(xsk_map_key);
}

static int register_xsk_map(int xsk_fd, int xsk_map_key, int proto)
{
    int xsk_map_fd;

    struct xdp_program *xdp_prog;

    if (proto == IPPROTO_HOMA)
        xdp_prog = etran_homa->_ebpf.xdp_prog;
    else
        xdp_prog = etran_tcp->_ebpf.xdp_prog;

    xsk_map_fd = bpf_map__fd(bpf_object__find_map_by_name(xdp_program__bpf_obj(xdp_prog), "xsks_map"));
    if (xsk_map_fd < 0)
    {
        fprintf(stderr, "ERROR: no xsk map found: %s\n", strerror(xsk_map_fd));
        return -EINVAL;
    }

    if (bpf_map_update_elem(xsk_map_fd, &xsk_map_key, &xsk_fd, BPF_NOEXIST)) {
        const int error = errno;
        fprintf(stderr, "ERROR: cannot register XSK map key %d: %s\n", xsk_map_key, strerror(error));
        return error ? -error : -EIO;
    }

    if (proto == IPPROTO_HOMA)
        homa_monitor_xsk_map[xsk_map_key] = xsk_fd;
    else
        tcp_monitor_xsk_map[xsk_map_key] = xsk_fd;

    return 0;
}

static void return_resources(app_ctx &app)
{
#ifdef ALLOCATION
    if (app.proto == IPPROTO_TCP) {
        migration_executor.release(app);
        release_app_cpus(&app);
    }
#endif
    if (!app.use_shared_queues)
        for (unsigned int i = 0; i < app.nr_nic_queues; ++i)
            etran_nic->_free_exclusive_qids.push_back(app.nic_qid[i]);
}

static bool collect_cc_jitter()
{
    static const bool enabled = [] {
        const char *flag = std::getenv("ETRAN_CC_JITTER");
        return flag && strcmp(flag, "1") == 0;
    }();
    return enabled;
}

static uint64_t report_slow_control_step(const char *operation, const app_ctx &app, uint64_t start)
{
    if (!start)
        return 0;
    const auto end = get_cycles();
    const auto us = static_cast<unsigned long long>(cycles_to_us(end - start));
    if (us > 500)
        fprintf(stderr, "cc_jitter_source operation=%s pid=%d app_gen=%llu elapsed_us=%llu\n",
                operation, app.pid, static_cast<unsigned long long>(app.generation), us);
    return end;
}

static void free_app_resources(struct app_ctx *actx, bool return_queues)
{
    auto stage = collect_cc_jitter() ? get_cycles() : 0;
    if (actx->proto == IPPROTO_TCP)
    {
#ifdef ALLOCATION
        migration_executor.cancel(*actx);
#endif
        scheduler_unregister_workers(actx->generation);
        if (actx->done && !actx->use_shared_queues) {
            for (unsigned int i = 0; i < actx->nr_nic_queues; ++i) {
#ifdef ALLOCATION
                irq_owner_tokens[actx->nic_qid[i]].store(0, std::memory_order_release);
#endif
                scheduler_unregister_queue(actx->nic_qid[i]);
            }
        }
        free_tcp_resources(actx);
    }

    if (actx->done && actx->proto == IPPROTO_HOMA)
    {
        free_homa_resources(actx);
    }

    if (return_queues)
        destroy_rss_context(actx);
    stage = report_slow_control_step("flow_cleanup", *actx, stage);

    for (unsigned int i = 0; i < actx->nr_nic_queues; i++)
    {
        unsigned int qid = actx->nic_qid[i];

        if (!etran_nic->_nic_queues[qid].xsk_info)
            return;

        if (etran_nic->_nic_queues[qid].is_shared) {
            fprintf(stderr, "TODO: How to free a shared queue?\n");
            return;
        }

        epoll_ctl(xsk_epfd, EPOLL_CTL_DEL, xsk_socket__fd(etran_nic->_nic_queues[qid].xsk_info->xsk), nullptr);

        unregister_xsk_map(xsk_socket__fd(etran_nic->_nic_queues[qid].xsk_info->xsk), etran_nic->_nic_queues[qid].xsk_map_key, actx->proto);

        xsk_delete_socket(etran_nic->_nic_queues[qid].xsk_info);

        etran_nic->_nic_queues[qid].xsk_info = nullptr;
        etran_nic->_nic_queues[qid].bpw = nullptr;
        etran_nic->_nic_queues[qid].xsk_map_key = 0;

        unregister_slow_path_map(qid, actx->proto);

        auto &owners = etran_nic->_nic_queues[qid].actxs;
        owners.erase(std::remove(owners.begin(), owners.end(), actx), owners.end());
    }

    for (unsigned int i = 0; i < actx->nr_app_threads; i++)
        for (unsigned int j = 0; j < actx->nr_nic_queues; j++)
        {
            unregister_xsk_map(actx->tctx[i].txrx_xsk_fd[j], actx->tctx[i].txrx_xsk_map_key[j], actx->proto);
            close(actx->tctx[i].txrx_xsk_fd[j]);
        }

    for (auto it = actx->ports.begin(); it != actx->ports.end(); it++)
        free_port(*it);

    actx->ports.clear();

    bpf_map_delete_elem(etran_entrance->_umem_id_tran_map_fd, &actx->umem_id);
    bp_free(&actx->bpw);

    for (unsigned int i = 0; i < actx->nr_app_threads; i++) {
        shm_wrapper_destroy(actx->tctx[i].lrpc);
        close(actx->tctx[i].evfd);
    }
    if (return_queues)
        return_resources(*actx);
    actx->done = false;
    actx->resources_ready = false;
    report_slow_control_step("socket_cleanup", *actx, stage);
}

static void destroy_all_apps(void)
{
#ifdef ALLOCATION
    for (auto *app : active_apps.apps)
        migration_executor.cancel(*app);
#endif
    stop_rss_worker();
    for (const auto &result : take_rss_results()) {
#ifdef ALLOCATION
        if (result.irq) {
            migration_executor.on_irq_result(result);
            continue;
        }
#endif
        for (auto *app : active_apps.apps) {
            if (app->generation != result.generation)
                continue;
            if (!result.remove_context)
                app->rss_ctx_id = result.context;
            else if (!result.error)
                app->rss_ctx_id = 0;
            break;
        }
    }
    if (active_apps.apps.empty()) {
        return;
    }
    // Notify all apps to exit
    bool had_live_apps = false;
    for (auto *app : active_apps.apps) {
        if (app->fd >= 0 && app->done) {
            kill_app(app->pid);
            had_live_apps = true;
        }
    }
    if (had_live_apps)
        sleep(1);

    // Free all resources
    for (auto *app : active_apps.apps) {
        if (app->resources_ready || app->done)
            free_app_resources(app);
        else
            destroy_rss_context(app);
        if (app->fd >= 0)
            close(app->fd);
        delete app;
    }
    active_apps.apps.clear();
    active_apps.nr_app = 0;
}

static void erase_app(app_ctx *app)
{
    auto &apps = active_apps.apps;
    apps.erase(std::find(apps.begin(), apps.end(), app));
    --active_apps.nr_app;
    delete app;
}

static void retire_app(app_ctx *app)
{
    if (app->rss_ctx_id && !app->use_shared_queues) {
        free_app_resources(app, false);
        submit_rss_job({app->generation, app->rss_ctx_id, {}});
        return;
    }
    if (app->resources_ready || app->done)
        free_app_resources(app);
    erase_app(app);
}

static void destroy_app(int fd)
{
    if (auto *app = find_actx_with_fd(fd)) {
        close(fd);
        app->fd = -1;
        if (app->done)
            kill_app(app->pid);
        if (app->uds_state != UdsState::WaitMgmt)
            retire_app(app);
    }
}

static struct app_ctx *find_actx_with_fd(int fd)
{
    for (auto it = active_apps.apps.begin(); it != active_apps.apps.end(); it++)
    {
        if ((*it)->fd == fd)
            return *it;
    }
    return nullptr;
}

static void prepare_uds_response(app_ctx &app, UdsState state, resp_type type, int error = 0)
{
    app.uds_state = state;
    app.uds_offset = 0;
    app.uds_fd_batch = 0;
    auto &resp = app.uds_resp;
    resp = {};
    resp.type = type;
    resp.err_code = error;
    if (type == REG_DONE) {
        const auto *actx = &app;
        resp.shm_bp_size = actx->bpw.shm_bp->size;
        resp.shm_umem_size = actx->bpw.shm_umem->size;
        resp.shm_lrpc_size = actx->tctx[0].lrpc->size;
        snprintf(resp.shm_bp_name, SHM_NAME_MAX, "%s", actx->bpw.shm_bp->name.c_str());
        snprintf(resp.shm_umem_name, SHM_NAME_MAX, "%s", actx->bpw.shm_umem->name.c_str());
        resp.bp_params = actx->bpw.bp_params;
        resp.ifindex = if_nametoindex(etran_nic->_if_name.c_str());
        for (unsigned int i = 0; i < actx->nr_nic_queues; i++)
            resp.nic_qid[i] = actx->nic_qid[i];
        #ifdef ALLOCATION
        resp.sched_cooperative = scheduler_manual && actx->proto == IPPROTO_TCP && !actx->use_shared_queues;
        resp.cpuLen=actx->usedCores.size();
        int idx=0;
        for (auto core : actx->usedCores) {
            resp.cpus[idx] = core;
            idx++;
        };
        #endif
    }
}

static void finish_registration(app_ctx &app)
{
#ifdef ALLOCATION
    if (scheduler_collect_qid_stats() && app.proto == IPPROTO_TCP && !app.use_shared_queues) {
        auto irq_cpu = app.usedCores.begin();
        const int app_cpu = app.nr_app_threads == 1 ? app.usedCores.back() : -1;
        for (unsigned int i = 0; i < app.nr_nic_queues; ++i, ++irq_cpu)
            scheduler_register_queue(app.nic_qid[i], app.pid, app.tcp_type, *irq_cpu, app_cpu);
    }
    if (scheduler_manual && app.proto == IPPROTO_TCP && !app.use_shared_queues)
        for (unsigned int i = 0; i < app.nr_nic_queues; ++i)
            irq_owner_tokens[app.nic_qid[i]].store(app.generation, std::memory_order_release);
#endif
    app.done = true;
}

static void unregister_slow_path_map(unsigned int qid, int proto)
{
    int slow_path_map_fd;
    struct xdp_program *xdp_prog;

    if (proto == IPPROTO_HOMA)
        xdp_prog = etran_homa->_ebpf.xdp_prog;
    else
        xdp_prog = etran_tcp->_ebpf.xdp_prog;

    slow_path_map_fd = bpf_map__fd(bpf_object__find_map_by_name(xdp_program__bpf_obj(xdp_prog), "slow_path_map"));

    if (slow_path_map_fd < 0)
    {
        fprintf(stderr, "ERROR: no slow path map found: %s\n", strerror(slow_path_map_fd));
        return;
    }

    struct slow_path_info spi = {0};

    spi.active = false;
    spi.sp_xsk_map_key = 0;

    if (bpf_map_update_elem(slow_path_map_fd, &qid, &spi, BPF_ANY))
    {
        fprintf(stderr, "ERROR: slow path map key %d error\n", qid);
        return;
    }
}

static int register_slow_path_map(unsigned int qid, int xsk_map_key, int proto)
{
    int slow_path_map_fd;
    struct xdp_program *xdp_prog;

    if (proto == IPPROTO_HOMA)
        xdp_prog = etran_homa->_ebpf.xdp_prog;
    else
        xdp_prog = etran_tcp->_ebpf.xdp_prog;

    slow_path_map_fd = bpf_map__fd(bpf_object__find_map_by_name(xdp_program__bpf_obj(xdp_prog), "slow_path_map"));

    if (slow_path_map_fd < 0)
    {
        fprintf(stderr, "ERROR: no slow path map found: %s\n", strerror(slow_path_map_fd));
        return -EINVAL;
    }

    struct slow_path_info spi = {0};

    spi.active = true;
    spi.sp_xsk_map_key = xsk_map_key;

    if (bpf_map_update_elem(slow_path_map_fd, &qid, &spi, BPF_ANY))
    {
        fprintf(stderr, "ERROR: slow path map key %d error\n", qid);
        return -EEXIST;
    }

    return 0;
}

static inline int init_lrpc_channels(struct app_ctx *actx, const char *lrpc_prefix)
{
    std::string shm_lrpc_name;
    uint64_t lrpc_size;
    char *k_recv_head_wb, *app_recv_head_wb;
    char *channel_1, *channel_2;

    for (unsigned int i = 0; i < actx->nr_app_threads; i++)
    {
        actx->tctx[i].evfd = eventfd(0, EFD_NONBLOCK);
        if (actx->tctx[i].evfd < 0)
        {
            fprintf(stderr, "eventfd failed\n");
            return -EIO;
        }
    }

    for (unsigned int i = 0; i < actx->nr_app_threads; i++)
    {
        // shm_lrpc_name = "LRPC_" + std::to_string(actx->pid) + "_" + std::to_string(i);
        shm_lrpc_name = lrpc_prefix + std::to_string(actx->pid) + "_" + std::to_string(i);
        lrpc_size = (sizeof(struct head_wb) + sizeof(lrpc_msg) * LRPC_CHANNEL_SIZE) * 2;
        static_assert(sizeof(head_wb) == 64 && sizeof(lrpc_msg) == 64, "scheduler control alignment");
        actx->tctx[i].lrpc = shm_wrapper_create(shm_lrpc_name, lrpc_size + sizeof(worker_sched_ctrl));
        if (!actx->tctx[i].lrpc)
        {
            fprintf(stderr, "shm_wrapper_create failed\n");
            return -ENOMEM;
        }

        k_recv_head_wb = (char *)actx->tctx[i].lrpc->addr;
        app_recv_head_wb = k_recv_head_wb + sizeof(struct head_wb);

        channel_1 = app_recv_head_wb + sizeof(struct head_wb);
        channel_2 = channel_1 + sizeof(lrpc_msg) * LRPC_CHANNEL_SIZE;
        actx->tctx[i].sched_ctrl = new (k_recv_head_wb + lrpc_size) worker_sched_ctrl{};

        if (lrpc_init_out(&actx->tctx[i].kernel_out, (lrpc_msg *)channel_1, LRPC_CHANNEL_SIZE,
                          (uint32_t *)app_recv_head_wb))
        {
            goto err;
        }

        if (lrpc_init_in(&actx->tctx[i].kernel_in, (lrpc_msg *)channel_2, LRPC_CHANNEL_SIZE,
                         (uint32_t *)k_recv_head_wb))
        {
            goto err;
        }

        actx->tctx[i].actx = actx;
    }

    return 0;

err:
    // The registration owner closes even partially initialized channels.
    return -EIO;
}

static int try_allocate_queues(unsigned int nr_queues, queue_usage_hint hint, std::vector<unsigned int> &qids) {
    auto &free = etran_nic->_free_exclusive_qids;
    const auto &shared = etran_nic->_shared_qids;
    switch (hint) {
    case Q_EXCLUSIVE:
        if (free.size() < nr_queues)
            return -ENOSPC;
        qids.assign(free.rbegin(), free.rbegin() + nr_queues);
        break;
    case Q_SHARED:
        if (shared.size() < nr_queues)
            return -E2BIG;
        qids.assign(shared.begin(), shared.begin() + nr_queues);
        break;
    default:
        return -EINVAL;
    }
    for (unsigned int qid : qids) {
        if (qid >= etran_nic->_num_queues)
            return -EINVAL;
    }
    if (hint == Q_EXCLUSIVE)
        free.resize(free.size() - nr_queues);
    return 0;
}

// step1: create a buffer pool and UMEM
// step2: create slow-path XSKs and rings(rx,tx,fill,comp), bind them to the UMEM 
// step3: create TX/RX XSKs (nr_nic_queues * nr_app_threads) step4:
// register XSK fd to the XSK map
static int alloc_app_resources(struct register_request &req, int fd, int requested_app_cpu)
{
    int xsk_fd;
    struct app_ctx *actx;
    std::vector<unsigned int> qids;
    struct xdp_options xdp_opts = {0};
    socklen_t optlen = sizeof(xdp_opts);
    int tran_idx;
    bool umem_registered = false;
    std::array<bool, MAX_NIC_QUEUES> ctrl_registered{};
    std::array<unsigned int, MAX_APP_THREADS> registered_data{};
    int error = -ENOMEM;
    auto stage = collect_cc_jitter() ? get_cycles() : 0;

    if (req.nr_app_threads == 0 || req.nr_app_threads > MAX_APP_THREADS ||
        req.nr_nic_queues == 0 || req.nr_nic_queues > etran_nic->_num_queues)
    {
        fprintf(stderr, "alloc_app_resources: too many threads");
        return -E2BIG;
    }

#ifdef ALLOCATION
    if (req.proto == IPPROTO_TCP &&
        ((req.tcp_type != LATENCY && req.tcp_type != THROUGHPUT && req.tcp_type != AUTO) ||
         requested_app_cpu < -1 ||
         (requested_app_cpu >= 0 && !valid_pool_cpu(requested_app_cpu)))) {
        return -EINVAL;
    }
#endif

    if (req.proto != IPPROTO_TCP) {
        if (req.queue_usage != Q_EXCLUSIVE) {
            fprintf(stderr, "Currently only TCP supports shared queues");
        }
    }

    auto &xsk_keys = etran_nic->_available_xsk_keys;
    size_t data_keys = req.nr_nic_queues * req.nr_app_threads;
    size_t total_keys = data_keys + req.nr_nic_queues;
    if (total_keys > xsk_keys.size())
    {
        fprintf(stderr,
                "alloc_app_resources: too many XSK keys, "
                "etran_nic->_available_xsk_keys.size()=%zu\n",
                etran_nic->_available_xsk_keys.size());
        return -E2BIG;
    }

    if (!SUPPORT_PROTO(req.proto))
    {
        fprintf(stderr, "alloc_app_resources: unsupported proto %d\n", req.proto);
        return -EINVAL;
    }

    actx = find_actx_with_fd(fd);

    if (!actx)
        return -ENOENT;

    if (req.proto == IPPROTO_TCP) {
        // TCP's per-connection map encodes the data XSK keys as u8.
        auto wide = std::stable_partition(xsk_keys.begin(), xsk_keys.end(),
                                          [](int key) { return key <= UINT8_MAX; });
        if (wide < xsk_keys.begin() + data_keys) {
            fprintf(stderr, "Not enough TCP data XSK keys in the 8-bit range\n");
            return -ENOSPC;
        }
        // Slow-path keys can use the remainder of the XSK map.
        if (wide < xsk_keys.begin() + total_keys)
            std::rotate(xsk_keys.begin(), wide, xsk_keys.begin() + total_keys);
    }

    if (int rc = try_allocate_queues(req.nr_nic_queues, req.queue_usage, qids)) {
        if (rc == -ENOSPC && req.proto == IPPROTO_TCP && req.queue_usage == Q_EXCLUSIVE) {
            size_t reclaiming = etran_nic->_free_exclusive_qids.size();
            for (const auto *other : active_apps.apps) {
                if (other->proto != IPPROTO_TCP || other->use_shared_queues ||
                    (!other->resources_ready && !other->rss_ctx_id))
                    continue;
                pollfd socket{other->fd, POLLRDHUP, 0};
                if (socket.fd < 0 || (poll(&socket, 1, 0) > 0 &&
                                      (socket.revents & (POLLRDHUP | POLLHUP | POLLERR))))
                    reclaiming += other->nr_nic_queues;
            }
            if (reclaiming >= req.nr_nic_queues)
                return -EAGAIN;
        }
        fprintf(stderr, "Cannot allocate NIC queues: %s\n", strerror(-rc));
        return rc;
    }

    actx->proto = req.proto;
    actx->nr_nic_queues = req.nr_nic_queues;
    actx->nr_app_threads = req.nr_app_threads;
    actx->use_shared_queues = req.queue_usage == Q_SHARED;
    std::copy(qids.begin(), qids.end(), actx->nic_qid);
    for (unsigned i = 0; i < req.nr_app_threads; ++i) {
        actx->tctx[i].evfd = -1;
        std::fill_n(actx->tctx[i].txrx_xsk_fd, req.nr_nic_queues, -1);
    }
#if defined(ALLOCATION)
    actx->tcp_type = req.tcp_type;
    actx->usedCores = std::list<int>();
    actx->requested_app_cpu = requested_app_cpu;
    if (actx->proto == IPPROTO_TCP && !actx->use_shared_queues) {
        if (int rc = bind_rss_core(actx, qids, true)) {
            return_resources(*actx);
            return rc;
        }
    }
#endif

    if (int rc = init_lrpc_channels(actx, SHM_LRPC_PREFIX))
    {
        fprintf(stderr, "init_lrpc_channels failed\n");
        error = rc;
        goto err;
    }
    printf("init_lrpc_channels success\n");
    stage = report_slow_control_step("reserve_and_lrpc", *actx, stage);

    if (actx->use_shared_queues && etran_nic->_shared_queues_ready) {
        // Use shared BP
        actx->bpw = *etran_nic->_shared_bpw;
        printf("Reusing shared BP.\n");
    } else {
        // Create new BP
        if (bp_init(&actx->bpw,
                    actx->use_shared_queues ? getpid() : actx->pid,
                    actx->use_shared_queues ? SHM_BP_SR_PREFIX : SHM_BP_PREFIX,
                    actx->use_shared_queues ? SHM_UMEM_SR_PREFIX : SHM_UMEM_PREFIX))
        {
            fprintf(stderr, "bp_init failed\n");
            goto err;
        }
        if (actx->use_shared_queues) {
            etran_nic->_shared_bpw = &actx->bpw;
            etran_nic->_shared_queues_ready = true;
        }
        printf("Created new BP.\n");
    }
    stage = report_slow_control_step("buffer_pool", *actx, stage);

    for (unsigned int i = 0; i < req.nr_nic_queues; i++)
    {
        unsigned int qid = qids[i];
        auto &nicq = etran_nic->_nic_queues[qid];

        bool new_ctrl_xsk = !nicq.xsk_info;

        nicq.actxs.push_back(actx);
        if (new_ctrl_xsk) {
            // Initialize the XSK for control plane
            nicq.bpw = &actx->bpw;
            nicq.xsk_info = xsk_configure_socket(&nicq, actx->proto);
            if (!nicq.xsk_info) {
                fprintf(stderr, "xsk_configure_socket failed for Queue#%d\n", qid);
                goto err;
            }
        }

        actx->nic_qid[i] = qid;

        actx->qid2idx[qid] = i;

        actx->umem_fd[i] = xsk_socket__fd(nicq.xsk_info->xsk);

        if (new_ctrl_xsk) {
            epoll_event ev = {};
            ev.events = EPOLLIN | EPOLLERR;
            ev.data.fd = xsk_socket__fd(nicq.xsk_info->xsk);
            if (epoll_ctl(xsk_epfd, EPOLL_CTL_ADD, xsk_socket__fd(nicq.xsk_info->xsk), &ev))
            {
                fprintf(stderr, "epoll_ctl failed: %s\n", strerror(errno));
                goto err;
            }
        }
        /* Get UMEM id for this application */
        if (getsockopt(xsk_socket__fd(nicq.xsk_info->xsk), SOL_XDP, XDP_OPTIONS, &xdp_opts, &optlen))
            goto err;
        actx->umem_id = xdp_opts.umem_id;
    }
    printf("init etran_nic->_nic_queues success\n");
    stage = report_slow_control_step("control_xsk", *actx, stage);

    // TX/RX XSKs
    for (unsigned int i = 0; i < req.nr_app_threads; i++)
    {
        for (unsigned int j = 0; j < req.nr_nic_queues; j++)
        {
            xsk_fd = socket(AF_XDP, SOCK_RAW, 0);
            if (xsk_fd < 0)
                goto err;
            actx->tctx[i].txrx_xsk_fd[j] = xsk_fd;

            if (setsockopt(xsk_fd, SOL_SOCKET, SO_BINDTODEVICE, etran_entrance->_ebpf.if_name.c_str(),
                           strlen(etran_entrance->_ebpf.if_name.c_str())))
            {
                fprintf(stderr, "setsockopt SO_BINDTODEVICE failed\n");
                goto err;
            }

            if (etran_nic->_socket_busy_poll) {
                apply_socket_busy_poll(xsk_fd);
            }

        }
    }
    printf("init TX/RX XSKs success\n");

    if (actx->proto == IPPROTO_TCP)
        tran_idx = 0;
    else if (actx->proto == IPPROTO_HOMA)
        tran_idx = 1;
    if (bpf_map_update_elem(etran_entrance->_umem_id_tran_map_fd, &actx->umem_id, &tran_idx, BPF_ANY))
    {
        goto err;
    }
    umem_registered = true;
    printf("init UMEM and transport mapping success\n");

    for (unsigned int i = 0; i < req.nr_nic_queues; i++)
    {
        unsigned int qid = qids[i];
        etran_nic->_nic_queues[qid].xsk_map_key = etran_nic->_available_xsk_keys.front();

        if (register_xsk_map(xsk_socket__fd(etran_nic->_nic_queues[qid].xsk_info->xsk), etran_nic->_nic_queues[qid].xsk_map_key, actx->proto))
            goto err;
        ctrl_registered[i] = true;
        etran_nic->_available_xsk_keys.erase(etran_nic->_available_xsk_keys.begin());

        if (register_slow_path_map(qid, etran_nic->_nic_queues[qid].xsk_map_key, actx->proto))
            goto err;
    }
    printf("register XSK map for slowpath success\n");

    for (unsigned int i = 0; i < req.nr_app_threads; i++)
    {
        for (unsigned int j = 0; j < req.nr_nic_queues; j++)
        {
            actx->tctx[i].txrx_xsk_map_key[j] = etran_nic->_available_xsk_keys.front();
            if (register_xsk_map(actx->tctx[i].txrx_xsk_fd[j], actx->tctx[i].txrx_xsk_map_key[j], actx->proto))
                goto err;
            ++registered_data[i];
            etran_nic->_available_xsk_keys.erase(etran_nic->_available_xsk_keys.begin());
        }
    }
    printf("register XSK map for application success\n");
    printf("req.nr_app_threads(%u), req.nr_nic_queues(%u)\n", req.nr_app_threads, req.nr_nic_queues);
    if (thread_bcache_create(&actx->bpw, &actx->iobuffer))
        goto err;
    report_slow_control_step("data_xsk_and_maps", *actx, stage);

    actx->resources_ready = true;
    if (actx->proto == IPPROTO_TCP && !actx->use_shared_queues) {
        RssJob job{actx->generation, 0, {}};
#ifdef ALLOCATION
        auto cpu = actx->usedCores.begin();
        for (auto qid : qids)
            job.placement.emplace_back(qid, *cpu++);
#else
        for (auto qid : qids)
            job.placement.emplace_back(qid, -1);
#endif
        actx->uds_state = UdsState::WaitMgmt;
        submit_rss_job(std::move(job));
        return -EINPROGRESS;
    }
    if (int error = create_rss_context(actx, qids)) {
        free_app_resources(actx);
        return error;
    }

    finish_registration(*actx);
    return 0;

err:
    for (unsigned int i = 0; i < req.nr_app_threads; i++) {
        for (unsigned int j = 0; j < req.nr_nic_queues; j++) {
            if (j < registered_data[i])
                unregister_xsk_map(actx->tctx[i].txrx_xsk_fd[j], actx->tctx[i].txrx_xsk_map_key[j], actx->proto);
            if (actx->tctx[i].txrx_xsk_fd[j] >= 0)
                close(actx->tctx[i].txrx_xsk_fd[j]);
        }
    }
    if (umem_registered && !actx->use_shared_queues)
        bpf_map_delete_elem(etran_entrance->_umem_id_tran_map_fd, &actx->umem_id);

    for (unsigned int i = 0; i < req.nr_nic_queues; i++)
    {
        unsigned int qid = qids[i];
        if (etran_nic->_nic_queues[qid].is_shared) {
            fprintf(stderr, "TODO: How to deal with a shared queue?\n");
            continue;
        }
        auto *xsk_info = etran_nic->_nic_queues[qid].xsk_info;
        if (xsk_info) {
            const int fd = xsk_socket__fd(xsk_info->xsk);
            epoll_ctl(xsk_epfd, EPOLL_CTL_DEL, fd, nullptr);
            if (ctrl_registered[i]) {
                unregister_slow_path_map(qid, actx->proto);
                unregister_xsk_map(fd, etran_nic->_nic_queues[qid].xsk_map_key, actx->proto);
            }
            xsk_delete_socket(xsk_info);
        }
        auto it = std::find(etran_nic->_nic_queues[qid].actxs.begin(), etran_nic->_nic_queues[qid].actxs.end(), actx);
        if (it != etran_nic->_nic_queues[qid].actxs.end()) {
            etran_nic->_nic_queues[qid].actxs.erase(it);
        }
        etran_nic->_nic_queues[qid].xsk_info = nullptr;
        etran_nic->_nic_queues[qid].bpw = nullptr;
        etran_nic->_nic_queues[qid].xsk_map_key = 0;
    }

    bp_free(&actx->bpw);

    for (unsigned int i = 0; i < req.nr_app_threads; i++) {
        shm_wrapper_destroy(actx->tctx[i].lrpc);
        if (actx->tctx[i].evfd >= 0)
            close(actx->tctx[i].evfd);
    }

    return_resources(*actx);
    return error;
}

// Return 1 for a complete frame, 0 for a partial frame, or a negative errno.
static int poll_uds_frame(app_ctx &app, void *buffer, size_t size, bool outgoing)
{
    if (app.uds_offset == size)
        return 1;
    auto *next = static_cast<char *>(buffer) + app.uds_offset;
    ssize_t n = outgoing ? send(app.fd, next, size - app.uds_offset, MSG_NOSIGNAL)
                         : recv(app.fd, next, size - app.uds_offset, 0);
    if (n < 0)
        return errno == EAGAIN || errno == EINTR ? 0 : -errno;
    if (n == 0)
        return -ECONNRESET;
    app.uds_offset += n;
    return app.uds_offset == size;
}

static int poll_uds_reply(app_ctx &app)
{
    const auto state = app.uds_state;
    int rc = poll_uds_frame(app, &app.uds_resp, sizeof(app.uds_resp), true);
    if (rc <= 0)
        return rc;

    if (state == UdsState::SendXsk && app.uds_fd_batch <= app.nr_app_threads) {
        const auto batch = app.uds_fd_batch;
        const auto *fds = batch == 0 ? app.umem_fd : app.tctx[batch - 1].txrx_xsk_fd;
        rc = transfer_fds(app.fd, fds, app.nr_nic_queues);
        if (rc)
            return rc == -EAGAIN || rc == -EINTR ? 0 : rc;
        ++app.uds_fd_batch;
        return 0;
    }
    if (state == UdsState::SendEvent && app.uds_fd_batch < app.nr_app_threads) {
        rc = transfer_fds(app.fd, &app.tctx[app.uds_fd_batch].evfd, 1);
        if (rc)
            return rc == -EAGAIN || rc == -EINTR ? 0 : rc;
        ++app.uds_fd_batch;
        return 0;
    }
    switch (state) {
    case UdsState::SendCpuReady:
        app.uds_state = UdsState::RecvCpu;
        app.uds_offset = 0;
        break;
    case UdsState::SendXsk:
        prepare_uds_response(app, UdsState::SendEvent, REG_EVENT_FD);
        break;
    case UdsState::SendEvent:
        prepare_uds_response(app, UdsState::SendDone, REG_DONE);
        break;
    case UdsState::SendDone:
        app.uds_state = UdsState::Ready;
        app.uds_offset = 0;
        break;
    case UdsState::SendError:
        return -ECONNABORTED;
    default:
        return -EINVAL;
    }
    return 1;
}

static bool uds_wants_write(UdsState state)
{
    return state != UdsState::RecvRequest && state != UdsState::RecvCpu &&
           state != UdsState::Ready && state != UdsState::WaitMgmt && state != UdsState::WaitQueue;
}

static void start_uds_registration(app_ctx &app)
{
    const int error = alloc_app_resources(app.uds_req, app.fd, app.uds_requested_cpu);
    if (error == -EINPROGRESS)
        return;
    if (error == -EAGAIN) {
        if (app.uds_state != UdsState::WaitQueue)
            app.uds_queue_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        app.uds_state = UdsState::WaitQueue;
    } else if (error) {
        prepare_uds_response(app, UdsState::SendError, REG_FAIL, error);
    } else {
        prepare_uds_response(app, UdsState::SendXsk, REG_XSK_FD);
    }
}

static int poll_uds_request(app_ctx &app)
{
    if (app.uds_state == UdsState::WaitMgmt || app.uds_state == UdsState::WaitQueue)
        return -EPROTO;
    if (app.uds_state == UdsState::RecvCpu) {
        int rc = poll_uds_frame(app, &app.uds_requested_cpu, sizeof(app.uds_requested_cpu), false);
        if (rc <= 0)
            return rc;
    } else {
        int rc = poll_uds_frame(app, &app.uds_req, sizeof(app.uds_req), false);
        if (rc <= 0)
            return rc;
        if (app.uds_state == UdsState::Ready)
            return app.uds_req.type == UNREG ? -ECONNABORTED : -EPROTO;
        if (app.uds_req.type == REG || app.uds_req.type == REG_CPU)
            return -EPROTONOSUPPORT;
        if (app.uds_req.type == REG_CPU_V2) {
            prepare_uds_response(app, UdsState::SendCpuReady, REG_CPU_READY);
            return 1;
        }
        if (app.uds_req.type != REG_V2)
            return -EPROTO;
    }
    start_uds_registration(app);
    return 1;
}

static int update_uds_interest(const app_ctx &app, bool was_writing)
{
    const bool writing = uds_wants_write(app.uds_state);
    if (writing == was_writing)
        return 0;
    struct epoll_event ev{};
    ev.events = (writing ? EPOLLOUT : EPOLLIN) | EPOLLRDHUP;
    ev.data.u64 = app.generation;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, app.fd, &ev) < 0)
        return -errno;
    return 0;
}

static void poll_rss_results()
{
    for (const auto &result : take_rss_results()) {
#ifdef ALLOCATION
        if (result.irq) {
            migration_executor.on_irq_result(result);
            continue;
        }
#endif
        auto &apps = active_apps.apps;
        auto it = std::find_if(apps.begin(), apps.end(), [&](const auto *app) {
            return app->generation == result.generation;
        });
        assert(it != apps.end());
        auto *app = *it;
        if (result.remove_context) {
            if (result.error) {
                fprintf(stderr, "RSS cleanup failed app_gen=%llu rss_context=%d, holding CPU and QID reservations\n",
                        static_cast<unsigned long long>(app->generation), result.remove_context);
                continue;
            }
            app->rss_ctx_id = 0;
            return_resources(*app);
            erase_app(app);
            continue;
        }
        app->rss_ctx_id = result.context;
        if (app->fd < 0) {
            retire_app(app);
            continue;
        }
        if (result.error) {
            free_app_resources(app);
            prepare_uds_response(*app, UdsState::SendError, REG_FAIL, result.error);
        } else {
            finish_registration(*app);
            prepare_uds_response(*app, UdsState::SendXsk, REG_XSK_FD);
        }
        if (update_uds_interest(*app, false))
            destroy_app(app->fd);
    }
}

static void poll_pending_queues()
{
    auto &apps = active_apps.apps;
    for (size_t i = 0; i < apps.size();) {
        auto &app = *apps[i];
        if (app.uds_state != UdsState::WaitQueue) {
            ++i;
            continue;
        }
        if (std::chrono::steady_clock::now() >= app.uds_queue_deadline)
            prepare_uds_response(app, UdsState::SendError, REG_FAIL, -ENOSPC);
        else if (etran_nic->_free_exclusive_qids.size() >= app.uds_req.nr_nic_queues)
            start_uds_registration(app);
        if (update_uds_interest(app, false))
            destroy_app(app.fd);
        else
            ++i;
    }
}

static int accept_app(void)
{
    int new_fd;
    struct app_ctx *app = nullptr;
    struct ucred cred;
    socklen_t ucred_len = sizeof(struct ucred);

    new_fd = accept4(uds_sockfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (new_fd < 0)
        return -1;

    if (active_apps.nr_app >= MAX_SUPPORT_APP)
        goto err;

    app = new app_ctx();
    if (!app)
        goto err;

    if (getsockopt(new_fd, SOL_SOCKET, SO_PEERCRED, &cred, &ucred_len))
    {
        perror("getsockopt");
        goto err;
    }

    app->pid = cred.pid;
    printf("app->pid = %d\n", app->pid);

    app->done = false;
    app->fd = new_fd;
    static uint64_t next_generation = 0;
    app->generation = ++next_generation;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.u64 = app->generation;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &ev) < 0)
        goto err;

    active_apps.apps.push_back(app);
    active_apps.nr_app++;

    return new_fd;

err:
    if (app)
        delete app;
    close(new_fd);
    return -1;
}

int ctx_init(void)
{
    xsk_epfd = epoll_create1(0);
    if (xsk_epfd < 0)
    {
        perror("epoll_create1 failed for xsk_epfd");
        return -1;
    }

    for (uint16_t i = PORT_MIN; i <= PORT_MAX; i++)
    {
        available_ports.push_back(i);
        port_is_available[i] = true;
    }

    return 0;
}

static int poll_uds(int timeout_ms)
{
    struct epoll_event events[MAX_SUPPORT_APP + 1];
    const int nfds = epoll_wait(epfd, events, MAX_SUPPORT_APP + 1, timeout_ms);

    if (nfds < 0)
        return -1;

    for (int i = 0; i < nfds; i++)
    {
        if (events[i].data.u64 == 0)
        {
            if (events[i].events & EPOLLERR)
            {
                perror("uds_sockfd error");
                return -1;
            }
            if (accept_app() < 0)
            {
                perror("accept_app");
            }
            continue;
        }
        const auto generation = events[i].data.u64;
        auto it = std::find_if(active_apps.apps.begin(), active_apps.apps.end(), [generation](const auto *app) {
            return app->generation == generation;
        });
        if (it == active_apps.apps.end())
            continue;
        auto &app = **it;
        const auto flags = events[i].events;
        const bool writing = uds_wants_write(app.uds_state);
        if (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            destroy_app(app.fd);
            continue;
        }
        int rc = 0;
        if (writing && (flags & EPOLLOUT))
            rc = poll_uds_reply(app);
        else if (!writing && (flags & EPOLLIN))
            rc = poll_uds_request(app);
        if (rc < 0 || update_uds_interest(app, writing))
            destroy_app(app.fd);
    }
    return 0;
}

struct CcJitter {
    // One microsecond per bucket, with a saturated overflow bucket.
    std::array<uint64_t, 2001> bins{};
    uint64_t previous = 0, samples = 0, maximum = 0, slow300 = 0, slow500 = 0;

    void observe(uint64_t now)
    {
        if (previous) {
            const auto interval = static_cast<uint64_t>(cycles_to_us(now - previous));
            ++bins[std::min<uint64_t>(interval, bins.size() - 1)];
            ++samples;
            maximum = std::max(maximum, interval);
            slow300 += interval > 300;
            slow500 += interval > 500;
        }
        previous = now;
    }

    size_t quantile(unsigned percentile) const
    {
        const auto rank = (samples * percentile + 99) / 100;
        uint64_t seen = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            seen += bins[i];
            if (seen >= rank)
                return i;
        }
        return bins.size() - 1;
    }

    void report() const
    {
        fprintf(stderr, "cc_jitter samples=%llu p50_bucket_us=%zu p99_bucket_us=%zu max_us=%llu gt300=%llu gt500=%llu\n",
                static_cast<unsigned long long>(samples), quantile(50), quantile(99),
                static_cast<unsigned long long>(maximum), static_cast<unsigned long long>(slow300),
                static_cast<unsigned long long>(slow500));
    }
};

static void *control_loop(void *arg)
{
    struct epoll_event ev;
    uint64_t tcp_s, tcp_e, homa_next_tsc = 0;
    const bool track_cc_jitter = collect_cc_jitter();
    CcJitter cc_jitter;

    epfd = epoll_create1(0);
    if (epfd < 0)
    {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN | EPOLLERR;
    ev.data.u64 = 0;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, uds_sockfd, &ev) < 0)
    {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }
    homa_next_tsc = get_cycles() + us_to_cycles(1000);
    
    while (!control_stopping.load(std::memory_order_relaxed))
    {
        /* poll unix domain socket */
        poll_uds(enrollment_to_ms);
        poll_rss_results();
        poll_pending_queues();

        /* poll lrpc channels */
        poll_lrpc();
#ifdef ALLOCATION
        if (scheduler_manual)
            poll_worker_migration();
#endif

        /* poll network packets */
        poll_network(network_to_ms);
        
        /* poll tcp handshake events */
        poll_tcp_handshake_events();

        /* poll tcp congestion control and timeout events */
        tcp_s = get_cycles();
        if (track_cc_jitter)
            cc_jitter.observe(tcp_s);
        poll_tcp_cc_to();
        tcp_e = get_cycles();

        /* poll homa timeout events */
        if (get_cycles() >= homa_next_tsc)
        {
            poll_homa_to();
            homa_next_tsc = get_cycles() + us_to_cycles(TICK_US);
        }

        /* decide how long to block */
        uint64_t t = cycles_to_us(tcp_e - tcp_s);
        uint64_t sleep_time = next_tcp_cc_to_tsc == UINT64_MAX ? TICK_US: CC_INTERVAL_US;
        if (t < sleep_time)
        {
            struct timespec ts = {
                .tv_sec = 0,
                .tv_nsec = (long int)(sleep_time - t) * 1000,
            };
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
        }
    }

    if (track_cc_jitter)
        cc_jitter.report();
    destroy_all_apps();
    close(epfd);
    return nullptr;
}

int thread_init(void)
{
    const auto &topology = etran_nic->_cpu_topology;
#ifdef ALLOCATION
    const char *mode = std::getenv("ETRAN_SCHED_MODE");
    const char *enable = std::getenv("ETRAN_ADMISSION_ENABLE");
    const char *policy = std::getenv("ETRAN_ADMISSION_POLICY");
    if ((enable && strcmp(enable, "0") && strcmp(enable, "1")) ||
        (policy && strcmp(policy, "reject") && strcmp(policy, "degrade"))) {
        fprintf(stderr, "Invalid scheduler admission configuration\n");
        return -EINVAL;
    }
    scheduler_admission = (mode && strcmp(mode, "admit_only") == 0) || (enable && strcmp(enable, "1") == 0);
    scheduler_manual = mode && strcmp(mode, "manual_actuate") == 0;
    scheduler_degrade = policy && strcmp(policy, "degrade") == 0;
    latUsingCores.clear();
    throughputUsingCores.clear();
    availableCores.clear();
    availableCores.insert(topology.pool.begin(), topology.pool.end());
    for (int i=0; i<MAX_CPU; i++) {
        cpuUseRef[i]=0;
        cpuLatUseRef[i]=0;
        cpuIrqRefs[i]=0;
        cpuThroughputAppRefs[i]=0;
    }
#endif

    uds_sockfd = init_uds_ctx();
    if (uds_sockfd < 0)
    {
        fprintf(stderr, "thread_init:init_uds_ctx failed\n");
        return -1;
    }

    int mgmt_cpu = topology.control;
    if (const char *value = std::getenv("ETRAN_SCHED_CPU")) {
        char *end;
        const auto parsed = strtol(value, &end, 10);
        if (end == value || *end || parsed < 0 || parsed >= CPU_SETSIZE ||
            !std::binary_search(topology.online.begin(), topology.online.end(), parsed) ||
            std::binary_search(topology.pool.begin(), topology.pool.end(), parsed) || parsed == topology.control) {
            fprintf(stderr, "Invalid ETRAN_SCHED_CPU for RSS management\n");
            destroy_uds_ctx(uds_sockfd);
            return -EINVAL;
        }
        mgmt_cpu = static_cast<int>(parsed);
    }
    try {
        rss_thread = std::thread(run_rss_worker);
    } catch (const std::system_error &error) {
        destroy_uds_ctx(uds_sockfd);
        return -error.code().value();
    }
    cpu_set_t mgmt_set;
    CPU_ZERO(&mgmt_set);
    CPU_SET(mgmt_cpu, &mgmt_set);
    if (int error = pthread_setaffinity_np(rss_thread.native_handle(), sizeof(mgmt_set), &mgmt_set)) {
        stop_rss_worker();
        destroy_uds_ctx(uds_sockfd);
        return -error;
    }

    cpu_set_t requested;
    CPU_ZERO(&requested);
    CPU_SET(topology.control, &requested);

    pthread_attr_t attr;
    int err = pthread_attr_init(&attr);
    if (err == 0) {
        err = pthread_attr_setaffinity_np(&attr, sizeof(requested), &requested);
        if (err == 0)
            err = pthread_create(&micro_kernel_thread, &attr, control_loop, nullptr);
        pthread_attr_destroy(&attr);
    }
    if (err != 0) {
        fprintf(stderr, "Cannot start control plane on CPU %d: %s\n",
                topology.control, strerror(err));
        stop_rss_worker();
        destroy_uds_ctx(uds_sockfd);
        return -1;
    }

    cpu_set_t actual;
    err = pthread_getaffinity_np(micro_kernel_thread, sizeof(actual), &actual);
    if (err != 0 || !CPU_EQUAL(&requested, &actual)) {
        fprintf(stderr, "Cannot verify control-plane affinity on CPU %d\n", topology.control);
        control_stopping.store(true, std::memory_order_relaxed);
        pthread_join(micro_kernel_thread, nullptr);
        destroy_uds_ctx(uds_sockfd);
        return -1;
    }
    printf("Control plane CPU: %d\n", topology.control);

    return 0;
}

void wait_thread(void)
{
    pthread_join(micro_kernel_thread, nullptr);
    destroy_uds_ctx(uds_sockfd);
}

void shutdown_thread(void)
{
    control_stopping.store(true, std::memory_order_relaxed);
}

static void dump_homa_rpc_map(void)
{
    int map_fd;
    bool empty = true;

    map_fd = etran_homa->_homa_rpc_fd;

    if (map_fd < 0)
    {
        fprintf(stderr, "ERROR: no map found: %s\n", strerror(map_fd));
        return;
    }

    empty = true;

    struct rpc_key_t key = {0}, next_key = {0};
    struct rpc_state value;
    std::cout << "\n===========================HOMA RPCs===========================\n";

    while (bpf_map_get_next_key(map_fd, &key, &next_key) == 0)
    {
        if (bpf_map_lookup_elem(map_fd, &next_key, &value) == 0)
        {
            if (value.state == BPF_RPC_INCOMING || value.state == BPF_RPC_IN_SERVICE) {
                printf("RPC#%llu "
                "state: %d "
                "length: %u "
                "incoming: %llu "
                "bytes_remaining: %llu\n"
                ,
                value.id, value.state, value.message_length, value.cc.incoming, value.cc.bytes_remaining);
                empty = true;
            } else if (value.state == BPF_RPC_OUTGOING) {
                printf("RPC#%llu "
                "state: %d "
                "length: %u "
                "granted: %llu "
                "next_xmit_offset: %llu\n"
                ,
                value.id, value.state, value.message_length, value.cc.granted, value.next_xmit_offset);
            } else if (value.state == BPF_RPC_DEAD) {
                printf("RPC is dead\n");
            }
            empty = false;
        }
        key = next_key;
    } 

    if (empty)
        std::cout << "Empty\n";
    std::cout << "===========================HOMA RPCs===========================\n";
}

static void dump_homa_port_map(void)
{
    int map_fd;
    bool empty = true;

    map_fd = etran_homa->_homa_port_tbl_fd;

    if (map_fd < 0)
    {
        fprintf(stderr, "ERROR: no map found: %s\n", strerror(map_fd));
        return;
    }

    empty = true;

    uint16_t key = {0}, next_key = {0};
    struct target_xsk value;
    std::cout << "\n===========================HOMA Ports===========================\n";

    while (bpf_map_get_next_key(map_fd, &key, &next_key) == 0)
    {
        if (bpf_map_lookup_elem(map_fd, &next_key, &value) == 0)
        {
            std::cout << "port: " << next_key << " ";
            for (int i = 0; i < MAX_CPU; i++)
            {
                std::cout << value.xsk_map_idx[i] << " ";
            }
            std::cout << std::endl;
            empty = false;
        }
        key = next_key;
    }

    if (empty)
        std::cout << "Empty\n";
    std::cout << "===========================HOMA Ports===========================\n";
}

static void dump_tcp_conn_map(void)
{
    int map_fd;
    bool empty = true;

    map_fd = etran_tcp->_tcp_connection_map_fd;

    if (map_fd < 0)
    {
        fprintf(stderr, "ERROR: no map found: %s\n", strerror(map_fd));
        return;
    }

    empty = true;
    struct ebpf_flow_tuple key = {0}, next_key = {0};
    struct bpf_tcp_conn value;
    std::cout << "\n===========================TCP Connections===========================\n";
    while (bpf_map_get_next_key(map_fd, &key, &next_key) == 0)
    {
        if (bpf_map_lookup_elem(map_fd, &next_key, &value) == 0)
        {
            std::cout << "bpf_tcp_conn: \n";
            std::cout << "local_ip: " << value.local_ip << " ";
            std::cout << "remote_ip: " << value.remote_ip << " ";
            std::cout << "local_port: " << value.local_port << " ";
            std::cout << "remote_port: " << value.remote_port << " ";
            printf("\n==============================================================\n");
            empty = false;
        }
        key = next_key;
    }
    if (empty)
        std::cout << "Empty\n";
    std::cout << "===========================TCP Connections===========================\n";
}

static void dump_xsk_map(int proto)
{
    std::map<int, int> *monitor_xsk_map;
    if (proto == IPPROTO_HOMA)
        monitor_xsk_map = &homa_monitor_xsk_map;
    else
        monitor_xsk_map = &tcp_monitor_xsk_map;
    printf("\n===========================XSK MAP(%s)===========================\n", proto == IPPROTO_HOMA ? "HOMA" : "TCP");
    if (monitor_xsk_map->empty())
    {
        std::cout << "Empty\n";
        printf("===========================XSK MAP(%s)===========================\n", proto == IPPROTO_HOMA ? "HOMA" : "TCP");
        return;
    }

    for (auto it = monitor_xsk_map->begin(); it != monitor_xsk_map->end(); it++)
    {
        std::cout << "key: " << std::setw(5) << it->first << " |value: " << it->second << std::endl;
    }
    printf("===========================XSK MAP(%s)===========================\n", proto == IPPROTO_HOMA ? "HOMA" : "TCP");
}

void dump_bpf_maps(void)
{
    dump_xsk_map(IPPROTO_HOMA);
    dump_xsk_map(IPPROTO_TCP);
    dump_tcp_conn_map();
    dump_homa_port_map();
    dump_homa_rpc_map();
}

void dump_ring_stats(void)
{
    for (unsigned int i = 0; i < etran_nic->_num_queues; i++)
    {
        if (!etran_nic->_nic_queues[i].xsk_info)
            continue;
        get_xsk_ring_stats(etran_nic->_nic_queues[i].xsk_info);
        dump_xsk_ring_stats(etran_nic->_nic_queues[i].xsk_info, std::to_string(i));
    }
}

void kick_napi(void)
{
    for (unsigned int i = 0; i < etran_nic->_num_queues; i++)
    {
        if (!etran_nic->_nic_queues[i].xsk_info)
            continue;
        kick_tx(etran_nic->_nic_queues[i].xsk_info);
        kick_fq(etran_nic->_nic_queues[i].xsk_info);
    }
}

static void process_cmd(struct app_ctx_per_thread *tctx, lrpc_msg *msg_in)
{
    switch (msg_in->cmd)
    {
    /***************** TCP *****************/
    case APPOUT_TCP_WORKER_TID: {
        appout_tcp_worker_tid_t identity;
        memcpy(&identity, msg_in->data, sizeof(identity));
        const auto *app = tctx->actx;
        if (app->proto == IPPROTO_TCP && identity.worker_idx < app->nr_app_threads &&
            tctx == &app->tctx[identity.worker_idx] && identity.os_tid > 0) {
            const auto task = "/proc/" + std::to_string(app->pid) + "/task/" +
                              std::to_string(identity.os_tid);
            if (access(task.c_str(), F_OK) == 0) {
                tctx->os_tid = identity.os_tid;
#ifdef ALLOCATION
                if (scheduler_manual)
                    tctx->os_start_time = read_worker_state(worker_task_path(*app, identity.os_tid)).start_time;
                auto cpu = app->usedCores.begin();
                std::advance(cpu, app->nr_nic_queues + identity.worker_idx);
                scheduler_register_worker_slot(app->generation, identity.worker_idx, *cpu, app->tcp_type);
#endif
                if (!app->use_shared_queues && app->nr_app_threads == 1)
                    for (unsigned int i = 0; i < app->nr_nic_queues; ++i)
                        scheduler_register_worker(app->nic_qid[i], app->pid, identity.os_tid);
            }
        }
        break;
    }
    case APPOUT_TCP_OPEN:
    case APPOUT_TCP_BIND:
    case APPOUT_TCP_LISTEN:
    case APPOUT_TCP_ACCEPT:
    case APPOUT_TCP_CLOSE:
        process_tcp_cmd(tctx, msg_in);
        break;
    /***************** Homa *****************/
    case APPOUT_HOMA_BIND:
    case APPOUT_HOMA_CLOSE:
        process_homa_cmd(tctx, msg_in);
        break;
    default:
        printf("Unknown command %ld\n", msg_in->cmd);
    }
}

static void poll_lrpc(void)
{
    struct app_ctx *actx;
    lrpc_msg msg = {0};

    for (auto it = active_apps.apps.begin(); it != active_apps.apps.end(); it++)
    {
        actx = *it;
        if (!actx->done)
            continue;

        for (unsigned int i = 0; i < actx->nr_app_threads; i++)
        {
            if (lrpc_empty(&actx->tctx[i].kernel_in))
                continue;
            lrpc_recv(&actx->tctx[i].kernel_in, &msg);
            process_cmd(&actx->tctx[i], &msg);
        }
    }
}

static struct nic_queue_info *find_nicq_with_fd(int fd)
{
    for (unsigned int i = 0; i < etran_nic->_num_queues; i++)
    {
        if (etran_nic->_nic_queues[i].xsk_info && xsk_socket__fd(etran_nic->_nic_queues[i].xsk_info->xsk) == fd) {
            return &etran_nic->_nic_queues[i];
        }
    }
    return nullptr;
}

static void replenish_fill_ring(app_ctx *actx)
{
    struct thread_bcache *bc = &actx->iobuffer;

    for (unsigned int i = 0; i < actx->nr_nic_queues; i++)
    {
        int qid = actx->nic_qid[i];
        if (!etran_nic->_nic_queues[qid].xsk_info->needfill)
            continue;
        if (!spin_lock_try(&actx->bpw.bp->fq_lock[qid]))
            continue;

        struct xsk_ring_prod *fq = &actx->bpw.bp->fq[qid];
        unsigned int idx = 0;

        if (xsk_ring_prod__reserve(fq, etran_nic->_nic_queues[qid].xsk_info->needfill, &idx) < etran_nic->_nic_queues[qid].xsk_info->needfill)
        {
            /* xxx */
            continue;
        }

        for (unsigned int j = 0; j < etran_nic->_nic_queues[qid].xsk_info->needfill; j++)
        {
            assert(thread_bcache_check(bc, 1) == 1);
            *xsk_ring_prod__fill_addr(fq, idx++) = thread_bcache_cons(bc);
        }
        xsk_ring_prod__submit(fq, etran_nic->_nic_queues[qid].xsk_info->needfill);

        etran_nic->_nic_queues[qid].xsk_info->needfill = 0;

        spin_unlock(&actx->bpw.bp->fq_lock[qid]);
    }
}

static void process_packet(int xsk_fd)
{
    polled_actx.reserve(IO_BATCH_SIZE); // avoid reallocation
    polled_actx.clear();
    unsigned int rcvd = 0;
    unsigned int idx_rx = 0;
    struct nic_queue_info *nicq = find_nicq_with_fd(xsk_fd);
    if (!nicq)
        return;
    bool is_shared = nicq->is_shared;

    struct app_ctx *excl_actx = nullptr;
    
    if (!is_shared && !nicq->actxs.empty()) {
        excl_actx = nicq->actxs[0];
    }

    struct xsk_socket_info *xsk_info = nicq->xsk_info;

    rcvd = xsk_ring_cons__peek(&xsk_info->rx, IO_BATCH_SIZE, &idx_rx);
    if (unlikely(!rcvd))
        return;

    for (unsigned int i = 0; i < rcvd; i++)
    {
        const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&xsk_info->rx, idx_rx++);
        uint64_t addr = desc->addr;

        addr = xsk_umem__add_offset_to_addr(addr);
        char *pkt = reinterpret_cast<char *>(xsk_umem__get_data(xsk_info->umem_area, addr));

        // this packet is recevied from which queue
        uint32_t qid = rxmeta_qid(pkt);
        etran_nic->_nic_queues[qid].xsk_info->needfill++;
#ifdef DEBUG_TCP
        fprintf(stdout, "Receive from NAPI ID: %d\n", qid);
#endif
        // fprintf(stdout, "Receive from NAPI ID: %d\n", qid);
        struct eth_hdr *eth = (struct eth_hdr *)pkt;
        struct ip_hdr *ip = (struct ip_hdr *)(eth + 1);
        auto tcp = (struct pkt_tcp *)pkt;
        if (ip->proto == IPPROTO_TCP)
        {
            // Try to locate the actx
            uint16_t sport = ntohs(tcp->tcp.src),
                     dport = ntohs(tcp->tcp.dest);
            app_ctx *actx = nullptr;
            struct thread_bcache *bc = nullptr;
            if (excl_actx) {
                actx = excl_actx;
            } if (auto it = dport_to_actx.find(dport);
                it != dport_to_actx.end()) {
                actx = it->second;
            } else if (auto it = sport_to_actx.find(sport);
                       it != sport_to_actx.end()) {
                actx = it->second;
            }
            if (!actx) {
                fprintf(stderr, "Could not locate actx for packet (sport=%u, dport=%u)\n", sport, dport);
            } else {
                bc = &actx->iobuffer;
                tcp_packet(actx, tcp, qid);
                thread_bcache_prod(bc, addr);
                if (!actx->need_refill) {
                    polled_actx.push_back(actx);
                    actx->need_refill = true;
                }
            }
        }
        else
        {
            fprintf(stderr, "Unknown protocol %d, pktlen=%u\n", ip->proto, desc->len);
        }
    }

    xsk_ring_cons__release(&xsk_info->rx, rcvd);

    // replenish fill ring
    for (auto actx : nicq->actxs) {
        replenish_fill_ring(actx);
    }
}

static int poll_network(int timeout_ms)
{
    /* traverse all NIC queues */
    for (unsigned int i = 0; i < etran_nic->_num_queues; i++)
    {
        if (!etran_nic->_nic_queues[i].xsk_info)
            continue;

        // reclaim completion buffers
        if (etran_nic->_nic_queues[i].xsk_info->outstanding)
        {
            for (auto actx : etran_nic->_nic_queues[i].actxs) {
                struct thread_bcache *bc = &actx->iobuffer;
                // don't compete with fastpath
                if (!spin_lock_try(&actx->bpw.bp->cq_lock[i]))
                    continue;
                struct xsk_ring_cons *cq = &actx->bpw.bp->cq[i];
                unsigned int idx_cq = 0;
                unsigned int rcvd = xsk_ring_cons__peek(cq, etran_nic->_nic_queues[i].xsk_info->outstanding, &idx_cq);
                for (unsigned int j = 0; j < rcvd; j++)
                {
                    uint64_t addr = *xsk_ring_cons__comp_addr(cq, idx_cq++);
                    thread_bcache_prod(bc, addr);
                }
                if (rcvd)
                {
                    xsk_ring_cons__release(cq, rcvd);
                    etran_nic->_nic_queues[i].xsk_info->outstanding -= rcvd;
                }
                spin_unlock(&actx->bpw.bp->cq_lock[i]);
            }
        }
    }

    epoll_event events[MAX_NIC_QUEUES];
    int nfds = epoll_wait(xsk_epfd, events, MAX_NIC_QUEUES, timeout_ms);
    if (!nfds)
        return 0;
    for (int i = 0; i < nfds; i++)
    {
        if (events[i].events & EPOLLERR)
        {
            fprintf(stderr, "EPOLLERR\n");
            continue;
        }
        if (events[i].events & EPOLLIN)
        {
            process_packet(events[i].data.fd);
        }
    }

    return 0;
}
