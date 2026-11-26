#pragma once

#include <intf/intf_ebpf.h>

const volatile __u32 qid_stats_enabled SEC(".rodata.qid_stats_enabled") = 0;

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, MAX_NIC_QUEUES);
    __type(key, __u32);
    __type(value, struct bpf_qid_stats);
} qid_stats_map SEC(".maps");

static __always_inline struct bpf_qid_stats *qid_stats(__u32 qid)
{
    return bpf_map_lookup_elem(&qid_stats_map, &qid);
}
