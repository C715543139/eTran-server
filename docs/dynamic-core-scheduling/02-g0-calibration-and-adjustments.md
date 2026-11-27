# eTran 动态调度 02 版调整方案：G0 实测标定、热路径优化与 G1 推进规范

本文档承接并修订 [01-metrics-and-scheduling-design.md](01-metrics-and-scheduling-design.md) 中的 §3.1、§3.2 与 §7.1，基于分支 `codex/dynamic-core-scheduling` 首批遥测实现（`2a31923`）及双机 `6.6.0-eTran-compat3.3` G0 短测结果（原始数据本地归档于 `experiments/20261126-dynamic-scheduler/README.md`），固化实测暴露的阈值偏差与小包热路径开销根因，明确下一批代码调整、正式 `G0` 复测与 `G1` 折叠方向对照的执行规范。

---

## 1. 首批 G0 短测实证发现

`2a31923` 在双端 24 核 / 24 通道基线上验证了 `ETRAN_QID_STATS=1`、`ETRAN_SOFTIRQ_PROBE=1`、`ETRAN_SCHED_CPU=1` 采样线程与时间轮提交口径（`tw_attempts == tw_submit`，`tw_drop = 0`，双端零内核告警）。同时，逐端 `microkernel.log` 与小包压测暴露出 **3 项必须先于决策器落地前修正的实测事实**：

### 1.1 实测画像对照与 `napi_defer` 软中断空转地板

| 实测场景（单队列单 worker） | 实测 `bps` | 实测 `pps` | 实测 `net_rx`（`52` / `51`） | `2a31923` 判定 | 期望判定 |
| :--- | ---: | ---: | ---: | :--- | :--- |
| **32 B 单流 `LAT` (`Q6`)** | `0.195 Gbps` | `279 kpps` | **`45.1%` / `48.0%`** | **`HEAVY`（误判）** | **`LIGHT`（3 核甜点 ② 候选）** |
| **10 MB 单流 `TP` (`Q8`)** | `20.9 Gbps` | `1.80–1.83 Mpps` | **`74.5%` / `85.1%`** | `HEAVY` | **`HEAVY`** |
| **64 B 16 连接高并发 (`Q8`)** | **`2.88 Gbps`**（`< 3.0G`） | **`2.55 Mpps`** | **`99.6%`** | `HEAVY` | **`HEAVY`** |

1. **`S_high = 35%` 被 `napi_defer_hard_irqs=100` 空轮询地板击穿**：
   - eTran 默认开启 `napi_defer_hard_irqs = 100` 与 `gro_flush_timeout = 200 ns`。单流 32 B `LAT` 约每 `11 µs` 到达一包，持续重置 defer 计数并驱动 `napi_watchdog` 空转，使其在几乎无业务压力（`0.195 Gbps`、`279 kpps`）下依然产生 **`45%–48%`** 的 `NET_RX` 软中断占比；
   - [scheduler.cc](../../eTran/micro_kernel/scheduler.cc#L206-L207) 沿用 `01` 版初值 `rx + tx >= 0.35`，导致双端单流 32 B `LAT` 全部被误判为 `HEAVY`。若不修正，后续 3 核甜点 ②（`S2`）将永远无法成立，还会持续误触发 `R1` 解救。
2. **多维指标（`pps` + `softirq`）的价值得到实锤**：
   - 64 B 高并发小包总带宽仅 `2.88 Gbps`（低于 `B_high = 3.0 Gbps`），但包率已达 `2.55 Mpps`、核软中断打满 **`99.6%`**；仅凭 `bps` 会彻底漏检此类双属性重载负载。

### 1.2 热路径开销分解：小包 `-1.9%` 来自 `percpu_array` 逐包查找，`softirq` 增量极低

| 配置（短测单轮） | 10 MB TP (Gbps) | 32 B LAT p99 / p99.9 (µs) | 64 B 16 连接完成量 (Mops/s) | 64 B 相对旧基线 |
| :--- | ---: | ---: | ---: | ---: |
| 旧基线（`72a2e5e`） | `20.08` | `15.64 / 17.52` | `1.190` | — |
| `per-QID` | `20.19` | `15.11 / 17.55` | `1.168` | **`-1.9%`** |
| `per-QID + softirq` | `19.99` | `14.80 / 17.99` | `1.165` | **`-2.1%`**（相对 `per-QID` 仅 **`-0.26%`**） |

- **`raw_tp/softirq` 性价比极高，决定保留为正式可选/默认对照模块**：因为 `softirq_entry/exit` 按 NAPI 批次触发而非逐包触发，即使在 `2.55 Mpps`、`99.6%` 满核极限下，额外增量开销仅 **`0.26%`**，却直接提供核饱和度与停滞 `NET_TX` 监测能力。
- **`per-QID` 的 `-1.9%` 根因**：在 `2.55 Mpps` 满核时单包全部 CPU 预算仅 `~392 ns`，`-1.9%` 约合每包 **`~7.5 ns`**。查看 [qid_stats.h](../../eTran/micro_kernel/eBPF/tcp/qid_stats.h#L7-L17) 与 [main.c](../../eTran/micro_kernel/eBPF/tcp/main.c)，当前在 `xdp_sock_prog` 与 `xdp_egress_prog` 的每个包上都执行了一次 `BPF_MAP_TYPE_PERCPU_ARRAY` 的 `bpf_map_lookup_elem()`，具备明确的优化空间。
- **单轮波动不足以判定 `0.5 µs` 门槛**：短测中两次旧基线 LAT p99 分别为 `14.41 µs` 与 `15.64 µs`（波动 `1.23 µs`），正式验收必须改为多轮交替轮换取中位数。

---

## 2. 针对 `2a31923` 的 4 项代码调整

### 调整 1：校准软中断阈值、补齐 EWMA 平滑与环境变量覆写

修改 [eTran/micro_kernel/scheduler.cc](../../eTran/micro_kernel/scheduler.cc)：

1. **抬高软中断高/低阈值，避开 `napi_defer` 地板**：
   - 将 `01` 版初值 `S_high = 0.35 / S_low = 0.20` 修订为 **`S_high = 0.65`（`65%`）、`S_low = 0.55`（`55%`）**，在 `32 B LAT (45%–48%)` 与 `10 MB TP (74%–85%)` / `64 B 高并发 (99.6%)` 之间留出约 `20%–25%` 隔离带；
   - 开放运行期覆写环境变量 `ETRAN_SCHED_B_HIGH`、`ETRAN_SCHED_B_LOW`、`ETRAN_SCHED_P_HIGH`、`ETRAN_SCHED_P_LOW`、`ETRAN_SCHED_S_HIGH`、`ETRAN_SCHED_S_LOW`，方便标定实验调参而无需重编。
2. **对 `net_rx / net_tx` 做 EWMA 平滑并消除 `-1` 哨兵隐式比较**：
   - 为每核维护 `ewma_net_rx` / `ewma_net_tx`（与 `bps/pps` 一致采用 `0.6 / 0.4` 系数），防止单 50 ms 窗口抖动误翻状态；
   - 用显式布尔值 `valid_probe` 守卫全部 `heavy`、`low`、`suspicious` 判定分支，不再让未初始化的 `-1.0` 参与 `rx + tx` 算术比较；
   - 保留低流量停滞保护：`idle` 态下（`bps < 1 Mbps && pps < 1 kpps`，此时正常空载队列无 `napi_defer` 续命，实测 `net_rx = 0.0000`），若 `valid_probe && (ewma_net_rx + ewma_net_tx > 0.20)` 或 `tw_drop_pkts` 增长，立即置为 `UNKNOWN` 禁止合并。
3. **补齐多队列共核软中断加权归因**：
   - 当 `only_irq == false`（多条活跃队列共享同一 IRQ 核）且 `valid_probe && (ewma_net_rx + ewma_net_tx >= S_high)` 时，按各队列等效包处理权重 `w(q) = pps(q) + bps(q) / (1500 * 8)` 分摊该核软中断占比 `share(q) = (ewma_net_rx + ewma_net_tx) * w(q) / sum(w)`，仅将 `share(q) >= S_high`（或自身 `bps/pps` 达标）的队列判为 `HEAVY`，防止轻载队列被同核大流连坐误判。

### 调整 2：拆分 `.bss` 直址热路径与 `PERCPU_ARRAY` 时间轮计数（压缩小包开销）

修改 [intf_ebpf.h](../../eTran/common/intf/intf_ebpf.h)、[qid_stats.h](../../eTran/micro_kernel/eBPF/tcp/qid_stats.h)、[main.c](../../eTran/micro_kernel/eBPF/tcp/main.c)、[pacing.h](../../eTran/micro_kernel/eBPF/tcp/pacing.h) 与 [trans_ebpf.cc](../../eTran/micro_kernel/trans_ebpf.cc)：

1. **热路径单核字段下沉至静态 `.bss` 64B 对齐数组**：
   - 独占队列的 `rx_xsk_pkts`、`rx_xsk_bytes`、`tx_direct_pkts`、`tx_direct_bytes`、`tw_enq_attempts` 严格在当前唯一 NAPI 核上串行写入（`compat3.3` 在线 IRQ 交接保证旧核退出后新核才进入首轮 `poll`，不并发写 NAPI 入口）；
   - 改为 `SEC(".bss.qid_napi_stats") struct bpf_qid_napi_stats qid_napi_stats[MAX_NIC_QUEUES];`（每元素 64B 对齐防伪共享），在 `xdp_sock_prog` 与 `xdp_egress_prog` 中仅做一次边界检查 `qid < MAX_NIC_QUEUES` 后直接数组下标累加，**彻底省去逐包 `BPF_MAP_TYPE_PERCPU_ARRAY` 的 `bpf_map_lookup_elem()` 调用**；
   - 微内核在 `eTranTCP::init_ebpf_maps()` 查找内部 `.bss.qid_napi_stats` Map FD（或直接 `mmap` 只读页），采样端读取极轻且零锁。
2. **跨核时间轮字段保留 `BPF_MAP_TYPE_PERCPU_ARRAY`**：
   - 仅将 `tw_cb()` 中可能在迁核瞬间双核重叠提交的 `tw_sent_pkts`、`tw_sent_bytes`、`tw_drop_pkts` 保留在 `tw_qid_stats_map`（`BPF_MAP_TYPE_PERCPU_ARRAY`），直发稳态下完全不触发，不增加直发小包热路径开销。
3. **成套重编提醒**：涉及 [intf_ebpf.h](../../eTran/common/intf/intf_ebpf.h) 结构变更，两端构建前先执行 `make -C eTran clean && make -C eTran`。

### 调整 3：收紧 `mode=off` 静默行为与日志开关

修改 [scheduler.cc](../../eTran/micro_kernel/scheduler.cc)：
- 仅当 `ETRAN_SCHED_MODE=shadow|active` 或显式设置 `ETRAN_SCHED_VERBOSE=1` 时周期打印 `sched_sample`；
- `ETRAN_SCHED_MODE=off` 且未开启 `VERBOSE` 时保持静默，确保关闭态对照与生产运行无多余 `stderr` I/O。

### 调整 4：补齐 `IDLE` 进展判据并为 `G1` 预留受控折叠开关

1. **接通进展信号解锁 `UNKNOWN → IDLE` 闭环**：
   - 采样线程只读读取对应应用活跃连接的 `bpf_cc`（`txp`、`cnt_rx_ack_bytes`）与 `/proc/softirqs`（作为 `ETRAN_SOFTIRQ_PROBE=0` 时的核级停滞兜底），以及 `/proc/<pid>/task/<tid>/schedstat`（`runq_wait_ns`）；
   - 满足“连续 10 窗低流量 + 无在途未确认 `txp` + 无异常高软中断/时间轮滞留”后，允许队列从 `LIGHT / UNKNOWN` 正常收敛至 `IDLE`；存在反压/停滞信号时仍严格锁死在 `UNKNOWN`。
2. **为 `G1` 预留双方向折叠开关**：
   - 在微内核（或实验触发路径）支持 `ETRAN_FOLD_MODE=irq_to_app|app_to_irq` 受控切换，确保下一批可直接完成 `G1` 折叠方向微对照并锁定默认方向。

---

## 3. 下一批验证矩阵（Batch 2A：正式 G0 复测 + 标定；Batch 2B：G1 折叠方向对照）

所有正式轮维持双端 `6.6.0-eTran-compat3.3`、`nosmt` 24 核、24 Combined 通道、NUMA0 偶数核池、控制面 `CPU14`、采样核 `CPU1`，全程双端记录生效核位与恢复核验。

### 3.1 Batch 2A：正式 `G0` 多轮交替复测与轻载/多队列标定

为消除单轮微秒级波动，以下前 3 组采用 **3 轮交替轮换（`old_baseline → new_off(QID_STATS=0) → per_qid(.bss) → per_qid+softirq` 循环，逐轮取中位数）**：

| 用例编号 | 负载配置 | 核心验收指标与判定目标 |
| :--- | :--- | :--- |
| **`G0-TP`** | 10 MB 单流 `TP` | 四组有效吞吐无回退；确认 `new_off` 相对旧基线 `0%` 偏差（验证 `.rodata` 死代码消除）；确认 `51/52` `Q8` 稳态稳定判为 `HEAVY` |
| **`G0-LAT`** | 32 B 单流 `LAT`（499,000 样本） | ① 验证抬高 `S_high = 0.65` 后，双端 `Q6` 稳态 **100% 稳定判为 `LIGHT`**（修复误判）；② 3 轮交替中位数验证 `per_qid` 与 `per_qid+softirq` 相对 `old_baseline` / `new_off` 的 `p99` 回退 `< 0.5 µs` |
| **`G0-SHORT`** | 64 B、16 连接、单 Q8 高并发小包 | 3 轮交替中位数量化 `.bss` 优化后的 `Mops/s` 回退幅度（目标将 `per-QID` 与 `per-QID+softirq` 相对基线的回退压至 `< 1%`），并核对 `HEAVY` 画像与 `~99.6%` 饱和度 |
| **`G0-CAL-LIGHT-TP`** | 节流轻载 `TP`（1 Gbps 与 2 Gbps 各 1 轮，通过 `epoll_client -u` 控制完整请求间隔） | 实测记录 `1 Gbps` 与 `2 Gbps` 轻载 `TP` 的 `bps / pps / net_rx` 读数，确认 `1 Gbps` 稳落 `LIGHT`、`2 Gbps` 落点明确，验证 `S_high = 0.65 / S_low = 0.55` 无边界误翻 |
| **`G0-MULTI-Q`** | 多队列共核归因验证：满载 `TP (Q8)` 与 32 B `LAT (Q6)` 同绑一颗 IRQ 核 `CPU8`（APP 分核 `CPU0/CPU2`） | 验证启用 `raw_tp/softirq` 后，按等效包处理权重分摊软中断占比：`Q8` 判为 `HEAVY`，而轻载 `Q6` **不被连坐误判为 `HEAVY`** |

### 3.2 Batch 2B：`G1` 折叠方向微对照（`irq_to_app` vs `app_to_irq`）

固定客户端 `52` 为 4 核全分离（`TP: Q8→8, APP=0`；`LAT: Q6→6, APP=2`），在服务端 `51` 4 核稳态（`TP` 预热 ≥ 7 s + `LAT` 90 万次往返）下分别跑 3 轮：

| 对照组 | `t = 3.5 s` 收缩动作（`4 → 3`） | `t = 7.5 s` 扩容动作（`3 → 4`） | 核心对比指标与决策准则 |
| :--- | :--- | :--- | :--- |
| **`G1-app-to-irq`** | 迁 `LAT APP`: `CPU2 → CPU6` | 迁 `LAT APP`: `CPU6 → CPU2` | 复测 `4C` 基线（历史约 `1.62 ms` 收缩毛刺、`45 µs` 扩容毛刺） |
| **`G1-irq-to-app`** | 迁 `LAT IRQ (Q6)`: `CPU6 → CPU2` | 迁 `LAT IRQ (Q6)`: `CPU2 → CPU6` | 对比 `[-50, +200] ms` 过渡窗 `max_rtt_us`、`> 50/100/500 µs` 样本数、`T_actuate`、`T_recover` 与 TP 50 ms 最低吞吐；**以过渡毛刺更低、恢复更快者锁定为 `ETRAN_FOLD_MODE` 默认值** |

G1 复测发现，历史 `4C` 的约 1.62 ms 收缩尖峰出现在客户端 RTT 时间戳 `vector` 扩容边界。计时前预分配并触碰缓冲区后，双方向各三轮的收缩和展开窗均无超过 100 µs 的 RTT。Batch 3 将 `ETRAN_FOLD_MODE` 初始默认设为 `app_to_irq`。迁 IRQ 的动作确认更快且过渡最大 RTT 更低，迁 APP 的折叠后 p99 更稳定，扩容时 TP 恢复更快。合作式单 TID 上线后以实际执行器复核（本地实测记录归档于 `experiments/20261126-dynamic-scheduler/README.md`）。

---

## 4. 衔接后续门禁（Batch 3 范围预告）

Batch 2A（正式 `G0` 指标与开销达标、阈值标定完毕）与 Batch 2B（默认折叠方向锁定）通过后，即可直接按 [01-metrics-and-scheduling-design.md](01-metrics-and-scheduling-design.md) §5–§7 进入 Batch 3：
1. 实现完整 `shadow` 模式决策日志（`R1–R4` 红线检测、`S1/S2` 容量评估与 `reject/degrade` 准入模拟）；
2. 实现合作式单 `TID` 自绑（`desired_cpu` + `evfd` + `APPOUT_TCP_WORKER_TID`）、统一 `MigrationExecutor`（含按需批量 `ETHTOOL_GSTATS`、`app_gen/qid_gen` 双预留、注销 `cancel_and_quiesce`）以及 `control_loop()` 异步 RSS 剥离 + CC 间隔埋点；
3. 依次通过 **`G2`（4 核 / 3 核双向顺序 / 无解准入）** 与 **`G3`（迁移中强退立即同 QID 重绑）** 门禁，随后开放 `ETRAN_SCHED_MODE=active` 执行全矩阵 `S1–S5` 验收。
