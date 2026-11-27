#pragma once

#include <intf/intf_ebpf.h>

#ifdef ETRAN_STATIC_QID_STATS
#define qid_stats_enabled ETRAN_STATIC_QID_STATS
#else
const volatile __u32 qid_stats_enabled SEC(".rodata.qid_stats_enabled") = 0;
#endif
struct bpf_qid_napi_stats qid_napi_stats[MAX_NIC_QUEUES] SEC(".bss.qid_napi_stats");
__u32 tw_pending_by_cpu[MAX_CPU] SEC(".bss.qid_tw_pending");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, MAX_NIC_QUEUES);
    __type(key, __u32);
    __type(value, struct bpf_qid_tw_stats);
} tw_qid_stats_map SEC(".maps");

static __always_inline struct bpf_qid_tw_stats *qid_tw_stats(__u32 qid)
{
    return bpf_map_lookup_elem(&tw_qid_stats_map, &qid);
}
