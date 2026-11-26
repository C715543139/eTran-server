// SPDX-License-Identifier: GPL-2.0
#include <intf/intf_ebpf.h>

char LICENSE[] SEC("license") = "GPL";

/* Linux softirq vector numbers on the compat3.x baseline. */
#define NET_TX_VECTOR 2
#define NET_RX_VECTOR 3

struct softirq_sample {
    struct bpf_softirq_stats total;
    __u64 rx_entry_ns;
    __u64 tx_entry_ns;
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct softirq_sample);
} softirq_samples SEC(".maps");

static __always_inline struct softirq_sample *sample_for(__u64 vector)
{
    __u32 key = 0;
    if (vector != NET_RX_VECTOR && vector != NET_TX_VECTOR)
        return NULL;
    return bpf_map_lookup_elem(&softirq_samples, &key);
}

SEC("raw_tp/softirq_entry")
int track_softirq_entry(struct bpf_raw_tracepoint_args *ctx)
{
    __u64 vector = ctx->args[0];
    struct softirq_sample *sample = sample_for(vector);
    if (sample) {
        if (vector == NET_RX_VECTOR)
            sample->rx_entry_ns = bpf_ktime_get_ns();
        else
            sample->tx_entry_ns = bpf_ktime_get_ns();
    }
    return 0;
}

SEC("raw_tp/softirq_exit")
int track_softirq_exit(struct bpf_raw_tracepoint_args *ctx)
{
    __u64 vector = ctx->args[0];
    struct softirq_sample *sample = sample_for(vector);
    if (!sample)
        return 0;

    if (vector == NET_RX_VECTOR && sample->rx_entry_ns) {
        sample->total.net_rx_ns += bpf_ktime_get_ns() - sample->rx_entry_ns;
        sample->rx_entry_ns = 0;
    } else if (vector == NET_TX_VECTOR && sample->tx_entry_ns) {
        sample->total.net_tx_ns += bpf_ktime_get_ns() - sample->tx_entry_ns;
        sample->tx_entry_ns = 0;
    }
    return 0;
}
