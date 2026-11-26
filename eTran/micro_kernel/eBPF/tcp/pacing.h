#pragma once

#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>

#include <intf/intf_ebpf.h>

#include "../ebpf_queue.h"
#include "../ebpf_utils.h"
#include "eTran_defs.h"
#include "qid_stats.h"

// send at most TW_BATCH_SIZE packets in one tw_cb
#define TW_BATCH_SIZE 32

/* BPF implementation for Carousel[SIGCOMM'17] */

/////////////////////////////// Timing Wheel (Per-CPU)  /////////////////////////////// 
// Timing Wheel
// logical ring
// bucket[0]                --> packet --> packet
// bucket[1]                --> packet --> packet --> packet
// ......
// bucket[MAX_BUCKETS-1]    --> packet

SEC(".bss.front_timestamp")
__u64 front_timestamp[MAX_CPU];
struct timing_wheel {
  __uint(type, BPF_MAP_TYPE_PKT_QUEUE);
  __uint(max_entries, NR_SLOT_PER_BKT);
  __uint(key_size, sizeof(__u32));
  __uint(value_size, sizeof(__u32));
  __uint(map_extra, MAX_BUCKETS);
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY_OF_MAPS);
    __uint(max_entries, MAX_CPU);
    __type(key, __u32);
    __array(values, struct timing_wheel);
} tw_outer_map SEC(".maps");

/* optimization: kick only once during each send batch */
SEC(".bss.has_kick")
__u64 has_kick[MAX_CPU];

static __always_inline __u32 tw_slot(__u32 cpu, __u64 ts, bool pending)
{
    ts = ts / SLOT_WIDTH_NS;

    // Reset the cursor when this wheel gets its first packet after draining.
    if (!pending)
        front_timestamp[cpu] = bpf_ktime_get_ns() / SLOT_WIDTH_NS;
    
    if (ts <= front_timestamp[cpu]) {
        // target time has passed, put it in the current bucket
        ts = front_timestamp[cpu];
    } else if (ts > front_timestamp[cpu] + MAX_BUCKETS - 1) {
        // target time is too far away, put it in the last bucket
        ts = front_timestamp[cpu] + MAX_BUCKETS - 1;
    }
    return ts % MAX_BUCKETS;
}

static __always_inline struct xdp_frame *tw_extract(struct bpf_map *tw_map, __u32 cpu, __u64 now, bool *cont)
{
    int ctrl_exit = 0;
    // convert timestamp to bucket index(abosulte value)
    now = now / SLOT_WIDTH_NS;

    while (now >= front_timestamp[cpu]) {
        // current bucket is ready to be processed
        __u32 idx = front_timestamp[cpu] % MAX_BUCKETS;
        
        if (!pkt_queue_empty(tw_map, idx, NULL)) {
            struct xdp_frame *frame = pkt_queue_dequeue(tw_map, idx, NULL);

            if (frame)
                return frame;
        }
        front_timestamp[cpu]++;
        if (++ctrl_exit >= 16) {
            break;
        }
    }

    *cont = now >= front_timestamp[cpu];

    return NULL;
}

//////////////////////////// Timing Wheel Trigger //////////////////////////// 
struct tw_trigger_t {
    struct bpf_timer t;
    __u8 ready;
}__attribute__((packed, aligned(64)));

// workaround: 
// kernel doesn't allow us to use BPF_MAP_TYPE_PERCPU_ARRAY with bpf_timer
struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, MAX_CPU);
  __type(key, __u32);
  __type(value, struct tw_trigger_t);
} tw_trigger_map SEC(".maps");

static int tw_cb(void *map, int *map_key, struct tw_trigger_t *twt);

// kick Timing Wheel to work in NET_TX_SOFTIRQ context
// if it has not been initialized, initialize it
static __always_inline void kick_tw(void)
{
    struct bpf_timer *t;
    struct tw_trigger_t *twt;
    __u32 current_cpu = bpf_get_smp_processor_id();
    if (unlikely(current_cpu >= MAX_CPU)) {
        log_panic("current_cpu >= MAX_CPU");
        return;
    }

    if (has_kick[current_cpu])
        return;

    twt = bpf_map_lookup_elem(&tw_trigger_map, &current_cpu);
    if (unlikely(!twt)) {
        log_panic("Failed to lookup tw_trigger_map");
        return;
    }

    t = &twt->t;
    
    if (unlikely(!twt->ready)) {
        bpf_timer_init(t, &tw_trigger_map, BPF_F_TIMER_NET_TX);
        bpf_timer_set_callback(t, tw_cb);
        twt->ready = true;
    }
    if (bpf_timer_start(t, 0, BPF_F_TIMER_IMMEDIATE))
        return;
    /* Preserve the index bound after the timer helpers and a stack reload. */
    barrier_var(current_cpu);
    if (current_cpu < MAX_CPU)
        has_kick[current_cpu] = true;
}

static int tw_cb(void *map, int *map_key, struct tw_trigger_t *twt) 
{
    struct xdp_frame *xdpf = NULL;
    struct bpf_dynptr ptr;
    struct tcp_timestamp_opt *ts_opt;
    int ctrl_exit = 0;
    __u64 now;
    struct bpf_map *tw_map = NULL;
    struct timing_wheel *tw = NULL;
    __u32 cpu = bpf_get_smp_processor_id();
    // make verifier happy
    if (unlikely(cpu >= MAX_CPU)) return 0;
    has_kick[cpu] = false;
    __u32 key = cpu;
    tw = bpf_map_lookup_elem(&tw_outer_map, &key);
    if (unlikely(!tw))
        return 0;
    tw_map = (struct bpf_map *)tw;

    if (!pkt_queue_pending(tw_map)) {
        barrier_var(cpu);
        if (qid_stats_enabled && cpu < MAX_CPU)
            tw_pending_by_cpu[cpu] = 0;
        return 0;
    }
    now = bpf_ktime_get_ns();
    
    bool cont = false;
    bool submitted = false;
    while (1) {
        if (ctrl_exit++ >= TW_BATCH_SIZE)
            break;
        xdpf = tw_extract(tw_map, cpu, now, &cont);
        if (!xdpf && !cont) break;
        if (!xdpf) {
            continue;
        }

        struct bpf_qid_tw_stats *stats = NULL;
        __u32 frame_len = 0;
        if (qid_stats_enabled) {
            stats = qid_tw_stats(xdpf->queue_index);
            frame_len = xdpf->len;
        }

        if (unlikely(bpf_dynptr_from_xdp_frame(xdpf, 0, &ptr))) {
            log_panic("bpf_dynptr_from_xdp_frame failed");
            if (stats)
                ++stats->tw_drop_pkts;
            bpf_packet_drop(xdpf);
            continue;
        }

        ts_opt = bpf_dynptr_slice_rdwr(&ptr, sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct tcphdr),
                            NULL, sizeof(*ts_opt));
        if (unlikely(!ts_opt)) {
            log_panic("bpf_dynptr_slice_rdwr failed");
            if (stats)
                ++stats->tw_drop_pkts;
            bpf_packet_drop(xdpf);
            continue;
        }

        ts_opt->ts_val = bpf_htonl((__u32)now);
        
        if (unlikely(bpf_packet_send(xdpf, xdpf->tx_ifindex, 0))) {
            if (stats)
                ++stats->tw_drop_pkts;
            bpf_packet_drop(xdpf);
            continue;
        }
        if (stats) {
            ++stats->tw_sent_pkts;
            stats->tw_sent_bytes += frame_len;
        }
        submitted = true;
    }

    if (submitted)
        bpf_packet_flush();

    bool pending = pkt_queue_pending(tw_map);
    barrier_var(cpu);
    if (qid_stats_enabled && cpu < MAX_CPU)
        tw_pending_by_cpu[cpu] = pending;
    if (pending)
        kick_tw();

    return 0;    
}

static __always_inline int xmit_packet_fib_lookup(struct xdp_md *ctx, struct ethhdr *eth,
                                       struct iphdr *iph) {
  int err = fib_lookup(ctx, eth, iph);
  if (unlikely(err)) {
    log_err("bpf_fib_lookup failed, check routing table in kernel.");
  }
  return XDP_TX;
}
