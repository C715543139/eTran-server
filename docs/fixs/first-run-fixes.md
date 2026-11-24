# 首轮实验运行修复

记录 2026-11-17 首轮复现实验发现的问题、代码调整及验证结果。本轮为同核、同 NUMA 分核、跨 NUMA 分核三种布局提供固定配置，并处理连续启动、ARP 和 AF_XDP 转发路径中的错误。

## 实验条件

| 项目 | 设置 |
| --- | --- |
| 服务端与客户端 | 51 为 server，52 为 client |
| 系统 | 51 为 Ubuntu 20.04，52 为 Ubuntu 22.04 |
| 首轮内核 | `6.6.0-eTran-compat1`，启动参数含 `nosmt nr_cpus=20 panic=30` |
| 数据口 | `enp4s0f1np1`，地址分别为 `192.168.100.51`、`192.168.100.52` |
| 网卡与内存 | 100GbE、NIC 位于 NUMA0、20 个队列、RX/TX ring 为 8192，实验内存绑定 NUMA0 |
| 吞吐应用 | 单业务线程、单连接、单 outstanding，10,000,000 B 请求、100 B 短响应 |
| 时延应用 | 32 B ping-pong，500,000 次 RTT，分位数丢弃前 1,000 次 |

本轮吞吐 APP 使用 CPU0，queue18 的 IRQ 使用 CPU18。时延 APP 与 queue17 的 IRQ 分别绑定 CPU18、CPU16、CPU17，构成三种布局。控制面使用 CPU19，CPU3 可以参与分配。

吞吐 APP 和网络处理最初共用 CPU18，应用轮询占用了该核的大量执行时间，难以产生预期的高吞吐背景。将吞吐 APP 放到 CPU0 后，单流高负载预热吞吐约为 20Gbps。

## 连续启动与退出

| 问题 | 原因 | 修复方案 | 修改位置 |
| --- | --- | --- | --- |
| 第二次启动微内核失败 | irqbalance 已停止，继续执行 `killall` 返回失败 | 先通过 `pgrep -x irqbalance` 判断进程是否存在，再执行停止操作 | `eTran/micro_kernel/nic.cc` |
| hugetlbfs 挂载不断叠加 | 每次启动都直接执行挂载命令 | 使用 `mountpoint` 检查 `/dev/hugepages`，已有挂载时复用 | `eTran/micro_kernel/micro_kernel.cc` |
| 后台日志持续打印 `Unknown command` | 标准输入到达 EOF 后仍进入命令处理 | `std::getline` 失败时退出监控线程 | `eTran/micro_kernel/micro_kernel.cc` |
| 应用已全部退出仍打印等待超时 | 空应用列表也进入固定等待流程 | `destroy_all_apps()` 在列表为空时直接返回 | `eTran/micro_kernel/control_plane.cc` |

运行工具通过 FIFO 向微内核发送 `dump` 命令。结束实验时根据角色 PID 和进程启动时间停止本轮进程，并恢复 NIC、NAPI 和 IRQ 管理参数。首轮产生的 14 层重复 hugetlbfs 挂载已清理。

## ARP 无法应答

加载入口 XDP 程序后，两机的 ARP 探测均收不到回复。`xdp_sock_prog()` 将所有非 IPv4 Ethernet 帧返回为 `XDP_DROP`，ARP 请求因此无法进入 Linux 邻居解析路径。

在 `eTran/micro_kernel/eBPF/entrance/entrance.c` 中增加 ARP 分支。

```c
if (proto_type == bpf_htons(ETH_P_ARP))
    return XDP_PASS;
```

修复后，六个实验配置加载 XDP 时均通过 ARP 探测。首轮另一次 TCP 握手超时的直接触发因素仍待复现确认。

## 固定 CPU 与负载控制

首轮增加以下实验入口。

| 参数 | 首轮实现 |
| --- | --- |
| `ETRAN_THROUGHPUT_CPU` | 指定微内核对 throughput 类型调用 `getCore()` 时返回的 CPU |
| `ETRAN_LATENCY_CPU` | 指定微内核对 latency 类型调用 `getCore()` 时返回的 CPU |
| `ETRAN_APP_CPU` | 在用户库的 `pthread_create` 拦截逻辑中覆盖新线程亲和性 |
| `epoll_client -u` | 指定每次 `connection_send()` 后的休眠微秒数，默认 2000 |

本轮高负载使用 `-u0`，低负载使用 `-u8000`。低负载采用批量间歇发送，实际吞吐随发送批量和运行阶段变化。

`ETRAN_APP_CPU` 的首轮实现仅在用户库覆盖线程亲和性，微内核仍按 `getCore()` 的结果登记应用 CPU。例如 IRQ18、APP0 的实际布局会被登记为 `[18,18]`。这一偏差由后续 CPU 注册、登记与回收修复处理。

## 测量窗口对齐

`lat_client` 增加 `measurement_start_unix_us` 和 `measurement_end_unix_us`，记录 500,000 次 RTT 循环的起止时间。吞吐分析取该窗口内的完整 1 秒样本，分位数统计丢弃前 1,000 次 RTT。

此调整避免把连接准备阶段和时延测量结束后的吞吐混入结果。同核高负载在实际测量窗口内吞吐降至 0，已标记为异常配置。

涉及 `eTran/tcp_app/lat_client.cc` 和 `eTran/tcp_app/epoll_client.cc`。

## AF_XDP 重传通知返回失败

同核高负载出现吞吐停滞，撤去时延负载后仍无法恢复。排查确认 AF_XDP TX pool 的 `xdp_rxq_info` 存在字段初始化遗漏。

1. `xp_assign_dev()` 未初始化新增字段 `orig_queue_index`，该字段保持为 0。
2. `xsk_rcv_check()` 据此执行跨队列校验，检查 UMEM 是否拥有 queue0。
3. 本轮 UMEM 拥有 queue17 或 queue18，校验返回 `-EINVAL`，重传通知无法通过 XSKMAP 返回用户态。

52 的运行时 kprobe 捕获到同一次调用的以下信息。

```text
rx_queue=18 orig_queue=0
__xsk_map_redirect ret=-22
```

在内核 `net/xdp/xsk_buff_pool.c` 的 `xp_assign_dev()` 中补齐初始化，与常规 `xdp_rxq_info` 注册路径采用相同的标记。

```c
pool->rxq.orig_queue_index = U32_MAX;
```

内核补丁位于 `kernel-patches/0002-initialize-af-xdp-egress-original-queue.patch`，基于现有 compat1 源码构建 `6.6.0-eTran-compat2`。

## 验证结果与剩余问题

- 仓库用户态产物在两种系统上分别构建，动态依赖检查通过。
- 三种布局的高、低负载共六个配置完成数据采集，每个配置得到 499,000 个分位数样本。
- 同核高负载的持续吞吐背景丢失，该行数据已标记异常。
- 后续两机加载 compat2 后，独立时延流与分核高负载各完成 500,000 次 RTT，分核吞吐约 20Gbps。
- compat2 下同核高负载仍出现停滞，一次回归的时延流停在第 2,048 次 RTT，吞吐降至 0。本次内核日志未记录此前的 `xdp_do_redirect() failed` 信息，整体停滞原因仍需继续排查。
- NUMA 对照还受到 XDP 按运行 CPU 选择发送队列的影响，后续比较需要控制这一因素。

首轮数据位于本地 `experiments/20261117-first-run/`，两机远端目录为 `/home/xhcui/repos/eTran-experiments/20261117-first-run`。后续 compat2 回归记录位于 `experiments/20261117-cpu-fix/`。
