# eTran 动态核池调度 05 版收尾方案：双项门禁终结与 `active` 放行前校准

本文档承接 [04-gate-closure-and-active-rollout.md](04-gate-closure-and-active-rollout.md)，针对 `codex/dynamic-core-scheduling@b3f7e39` 的 Batch 4 实测结果（本地归档于 `experiments/20261126-dynamic-scheduler/README.md`），收口最后两处极窄偏差并吸收两项 `S1 / S2` 实测校准，确保两项门禁全绿后无缝转入 `ETRAN_SCHED_MODE=active`（`S0–S5`）。

---

## 1. Batch 4（`b3f7e39`）结论快照：只剩两处窄差量

- **已达标项**：
  - 32 B LAT 6 轮交替 `p99`：旧基线 `16.1655` / `off` `16.1387` / `counters` **`16.1746 µs`**（`+0.0091 µs` vs 旧基线、`+0.0359 µs` vs `off`），无探针画像 100% 准确；
  - 64 B 16 连接 `counters` vs 同版 `off`：**`-0.53%`**（`1.179085` vs `1.185330 Mops/s`），计数本身开销已明显优于 `< 1%`；
  - 异步生命周期（Track B）：`509–568 ms` 微内核阻塞全部清零，活跃 CC `p50 = 255 µs`、`p99 = 421–422 µs`，UMEM 100% 落 NUMA0，合作式自绑 `277–446 µs`，`G3-quiesce` 双端各 20/20 通过。
- **仅剩两处未通过项**：
  1. **Track A**：同版 `off`（`1.185330`）相对旧基线 `72a2e5e`（`1.194935`）存在 **`-0.80%`** 偏移，导致 `counters` 相对旧基线落在 `-1.33%`（此前 `22962ad` 与 `681fafb` 的 `off` 分别为 `1.19502` / `1.19901 Mops/s`）；
  2. **Track B**：无跟踪 6 端次中仍有 2 端次活跃 CC 最大间隔略超 5 ms（51 r1 **`5.769 ms`**、52 r2 **`5.992 ms`**）；52 跟踪已证实尖峰中 **`4.87–6.07 ms`** 是 `control_loop()` 在 `clock_nanosleep` 准时唤醒后排队等上 `CPU 14` 的 CFS 调度等待。

---

## 2. 收尾动作一：定位并消除 `off` 相对 `72a2e5e` 的 `-0.80%` 偏移（闭环 Track A）

1. **编译期彻底净化 `off` 目标 `main.o`**：
   - 在 [qid_stats.h](../../eTran/micro_kernel/eBPF/tcp/qid_stats.h#L5-L18)、[Makefile](../../eTran/micro_kernel/eBPF/tcp/Makefile#L4-L7) 与 [pacing.h](../../eTran/micro_kernel/eBPF/tcp/pacing.h#L161-L232) 中，将 `main.o` 显式按 `ETRAN_STATIC_QID_STATS=0` 编译，彻底移除 `off` 路径中的 `const volatile qid_stats_enabled`、冗余 `barrier_var` 与计数 map 引用（[trans_ebpf.cc](../../eTran/micro_kernel/trans_ebpf.cc#L228-L234) 仅在 `scheduler_collect_qid_stats()` 为真时查找并 `mmap` 这 3 个计数 map），使 `main.o` 在 Clang 编译期即与 `72a2e5e` 等价。
2. **三点二分 + `perf stat` 定位 `-0.80%` 根因**：
   - 对比 `off` 模式下 `681fafb`、`6e115a6` 与上述净化后的 `HEAD`（各 1–2 轮 64 B，同步采集 `perf stat -C 0,8` 的 `cycles / instructions / IPC / L1-icache-load-misses / dTLB-load-misses`），明确偏移来自 `CPU 8`（BPF 布局或 `6e115a6` 异步建 XSK/UMEM 差异）还是 `CPU 0`（[eTran_posix.cc](../../eTran/lib/eTran_posix.cc#L660-L674)、[#L1090-L1091](../../eTran/lib/eTran_posix.cc#L1090-L1091) 热路径变化）；
   - 顺手修正 [eTran_posix.cc#L660-L674](../../eTran/lib/eTran_posix.cc#L660-L674) 一处冗余全屏障：仅在确实置位了 `WORKER_FLAG_SLEEPING` 进入阻塞时才在返回后执行 `SEQ_CST` 清零，非阻塞快速路径的 `apply_worker_sched_command()` 使用 `__ATOMIC_ACQUIRE` 读取 `cmd_word` 即可。
3. **Track A 终验**：
   - 修复偏移使 `off` 回到 `72a2e5e` 的 `±0.3%` 内后，执行 3 轮交替 `baseline / off / counters` 64 B 与 6 轮 32 B LAT 复验：确认 **64 B 双基准均 `< 1%`**、**LAT `p99 < 0.5 µs`**、`off` 零计数引用。

---

## 3. 收尾动作二：坐实 `CPU 14` 占核者并启用实时控制线程（闭环 Track B）

1. **1 轮补采坐实 `CPU 14` 抢占进程与 51 端 7 ms 成因**：
   - 修正 `cc-trace.bt`：在 `sched_switch` 中去掉 `/cpu == 14/` 过滤（改按 `args->prev_pid == @worker || args->next_pid == @worker` 并打印 `cpu`）以免 51 端漏捕离核时间，同时记录 `CPU 14` 在 `@worker` 唤醒至上核之间运行过的 `prev_comm / prev_pid / next_comm / next_pid`，明确占核任务（含未绑核的 `collector` / 外部脚本或内核线程）。
2. **微内核控制面实时调度支持（生产级修复）**：
   - 在 [control_plane.cc](../../eTran/micro_kernel/control_plane.cc#L3175-L3200) `thread_init()` 为 `control_loop()` 增加可选环境变量 **`ETRAN_CP_RT_PRIO`**（默认尝试 `SCHED_FIFO` 优先级 `1`，设为 `0` 禁用；无权限 `EPERM` 时打印告警并优雅回退 `SCHED_OTHER`）。`control_loop()` 每拍仅执行 `< 15 µs` 即休眠 `200 µs`（CPU 占比 `< 5%`），低优 `SCHED_FIFO` 不会饿死 `CPU 14`，可将 CFS `4–6 ms` 唤醒等待压至 `~10 µs`。
3. **测试脚手架隔离 + 无跟踪 3 轮终验**：
   - 将 `collector` / `host_runner.py` 等实验辅助进程统一 `taskset` 绑定到非池、非控制、非采样核（如 NUMA1 `CPU 1/3`），避免测试框架干扰 `CPU 14`；
   - 关闭 `bpftrace` 复跑 3 轮 Track B 并发注册/建连/迁移/回收，验收 **6/6 端次 `cc_jitter_active max_us < 5 ms`**（目标 `< 1–2 ms`）、`p99 < 500 µs`、`numa_maps` 100% NUMA0、0 同步阻塞告警，并回归确认 `G3-quiesce` 通过。

---

## 4. 吸收 Batch 4 实测校准，直接放行 `ETRAN_SCHED_MODE=active`（`S0–S5`）

在按 [04 文档 §4](04-gate-closure-and-active-rollout.md#L121-L167) 接线 `active` 时，纳入以下两项前置校准（涉及 [intf.h](../../eTran/common/intf/intf.h) 时须双端先 `make -C eTran clean && make -C eTran`）：

1. **上调 `S1` 默认 PPS 合并上限 `C_pps_cap`**：
   - 实测单队列 10 MB `TP` 在 `20.7 Gbps` 时达 `1.77–1.80 Mpps`，原 `1.6 Mpps` 会误拦 `S1-merge`（两路 ~10 Gbps 约 `1.75 Mpps`）。将默认 **`ETRAN_SCHED_CAP_PPS`** 校准为 **`2.0e6`（`2.0 Mpps`）**（保持 `ETRAN_SCHED_CAP_BPS = 22e9`）：`~10G + ~10G` 合法合并，`~20.8G + 满载 TP2` 与 64 B 高 QPS（`2.58 Mpps`）双双超限拒并置 `DEGRADED`。
2. **新增每应用粒度的 `ETRAN_IRQ_CPU` 入口（服务 `S2` 单核占位 `BG`）**：
   - 现有 `ETRAN_LATENCY_CPU` 作用于微内核全部 `LATENCY` 应用（[control_plane.cc#L864-L874](../../eTran/micro_kernel/control_plane.cc#L864-L874)），无法仅固定 `BG` 而不影响被测 `LAT`；
   - 在 [intf.h](../../eTran/common/intf/intf.h#L59-L74) `register_request`（或 `REG_CPU_V2` 扩展负载）中增加可选 `int32_t requested_irq_cpu`（默认 `-1`），由 `libetran` 读取环境变量 `ETRAN_IRQ_CPU` 传入；微内核在 `bind_rss_core()` 中校验其合法且非隔离核，优先于全局 `ETRAN_LATENCY_CPU / ETRAN_THROUGHPUT_CPU` 仅作用于该应用。`S2` 中用 `ETRAN_APP_CPU=10 ETRAN_IRQ_CPU=10` 即可精确构造单核占位 `BG`。

---

## 5. 收尾执行顺序

1. 完成 **§2（Track A `off` 净化与二分修复）** → 复验 64 B 双基准 `< 1%` + LAT `p99 < 0.5 µs`；
2. 完成 **§3（Track B `CPU 14` 坐实 + `ETRAN_CP_RT_PRIO=1` + 脚手架绑核）** → 无跟踪 3 轮 6/6 端次 `cc_jitter_active max_us < 5 ms` + `G3-quiesce` 确认；
3. 门禁全绿后落地 **§4 两项校准** + **[04 文档 §4](04-gate-closure-and-active-rollout.md#L121-L167)** `active` 接线 → 依次跑通 **`S0 / S1-merge / S1-cap-degrade / S2 / S3 / S4 / S5`**，归档完整报告并恢复双机干净态。
