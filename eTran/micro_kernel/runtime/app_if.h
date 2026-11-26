#pragma once

#include <string>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <list>
#include <set>

#include <base/lrpc.h>
#include <shm/shm_wrapper.h>
#include <xskbp/xsk_buffer_pool.h>
#include <intf/intf.h>
#include <runtime/defs.h>

#include <runtime/tcp.h>

struct app_ctx_per_thread
{
    struct app_ctx *actx;

    // TXRX-only XSK fd
    int txrx_xsk_fd[MAX_NIC_QUEUES];
    // TXRX-only XSK index in BPF_MAP_TYPE_XSKMAP
    int txrx_xsk_map_key[MAX_NIC_QUEUES];

    // per application thread lrpc channel
    // kernel recv_head_wb for channel_2
    // app recv_head_wb for channel_1
    // channel_1:    kernel --> app
    // channel_2:    app    --> kernel
    struct shm_wrapper *lrpc;

    struct lrpc_chan_in kernel_in;
    struct lrpc_chan_out kernel_out;
    worker_sched_ctrl *sched_ctrl = nullptr;
    int os_tid = 0;
    uint64_t os_start_time = 0;

    int evfd;

    // TCP
    std::list<struct tcp_listener *> listeners;
};

struct fd_rule_t {
    int id;
    uint16_t port;
    bool source;
};

enum class UdsState {
    RecvRequest, SendCpuReady, RecvCpu, WaitQueue, WaitMgmt, SendXsk, SendEvent, SendDone, SendError, Ready
};

// application context in kernel
struct app_ctx
{
    // the fd of Unix domain socket to communicate with the application
    int fd;

    // use it to kill the application
    int pid;
    uint64_t generation;
    UdsState uds_state = UdsState::RecvRequest;
    register_request uds_req{};
    register_response uds_resp{};
    size_t uds_offset = 0;
    unsigned int uds_fd_batch = 0;
    int uds_requested_cpu = -1;
    std::chrono::steady_clock::time_point uds_queue_deadline{};

    // transport protocol
    int proto;

    // RSS context id
    int rss_ctx_id = 0;

    // if we have allocated resources for this application
    bool done;
    bool resources_ready = false;

    // used in process_packet()
    bool need_refill = false;

    std::set<uint16_t> ports;

    std::vector<struct fd_rule_t> flow_director_rules;

    // application's per-thread context in kernel
    struct app_ctx_per_thread tctx[MAX_APP_THREADS];

    bool use_shared_queues;

    // UMEM id
    int umem_id;

    // The name is a bit ambiguous, because we only have one UMEM,
    // it actually refers to the first XSK fd bound to that UMEM
    int umem_fd[MAX_NIC_QUEUES];

    // the real nic queue ids the application is using
    unsigned int nic_qid[MAX_NIC_QUEUES];

    // convert nic queue id to index
    unsigned int qid2idx[MAX_NIC_QUEUES];

    // the number of queues the application is using
    unsigned int nr_nic_queues;

    // the number of threads the application is using
    unsigned int nr_app_threads;

    struct buffer_pool_wrapper bpw;

    struct thread_bcache iobuffer;

    #if defined(ALLOCATION)
    int tcp_type;//TCP type 0 for Latency, 1 for Throughput
    // IRQ CPUs first, then application CPUs, with one entry per reservation.
    std::list<int> usedCores;
    int requested_app_cpu = -1;
    #endif

    app_ctx() = default;
};

struct apps_info
{
    unsigned int nr_app;
    std::vector<struct app_ctx *> apps;
};
