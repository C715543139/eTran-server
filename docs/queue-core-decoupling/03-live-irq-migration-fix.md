# eTran 运行期 IRQ 在线热迁移修补与验收记录

本文档承接 [02-driver-decoupling-design.md](02-driver-decoupling-design.md)，针对 `6.6.0-eTran-compat3.2` 运行期在线迁核微实验（本地归档于 `experiments/20261125-live-migration-probe/`）暴露出的**活跃流量下队列 IRQ 亲和性改写后 NAPI 滞留旧核**问题，记录源码级根因剖析、`compat3.3` 内核与微内核修补实现，以及双机闭环验收结果（本地原始数据归档于 `experiments/20261125-live-irq-handoff/`）。

> **范围边界说明**：本文档聚焦于完成**队列与 CPU 的运行期动态解耦闭环**（使 `IRQ` 与 `APP` 均可在不断流、不重绑 XSK 的前提下按需在线换核，并具备可验证的交接闭环）。实验同时揭示的“标准 netdev sysfs 不统计 AF_XDP 零拷贝流量”及“动态核压缩迟滞策略”属于下一阶段负载指标与动态调度策略范畴，不在本文档展开。

---

## 1. 实测现象回顾与待处理问题界定

在 `compat3.2`（`main@1c7ea5a`，24 物理核 / 24 通道，NUMA0 偶数核池）的 32 轮在线迁核实测中，`APP` 与 `IRQ` 两类在线迁移呈现出截然不同的结果：

1. **`APP` 线程在线热迁移验证通过（`25/25` 次即时生效）**：
   - 遍历 `/proc/<pid>/task/*` 更新线程亲和性后，`0.22–0.42 ms` 内 busy-poll worker `TID` 实际切换至目标核，全程 `0` 断流、`0` 内核告警（实验同时证实仅修改主进程 `PID` 不会移动 worker 子线程）。
   - 迁 `LAT APP` 解救 `③ LAT APP + TP IRQ` 在 `100 ms` 内将 `LAT p99` 从 `677.44 µs` 降至 `19.70 µs`；迁 `TP APP` 解救 `④ TP APP + LAT IRQ` 在 `67.8 ms` 内将端口速率恢复至后稳态 95%，后稳态应用吞吐从 `4.36 Gbps` 升至 `19.45 Gbps`。
2. **问题 1（阻塞项 · 已由 `compat3.3` 修复）：活跃流量下 `IRQ` 在线热迁移未完成目标核数据面交接**：
   - `compat3.2` 下 35 次将活跃队列（`Q8` 或 `Q6`）的 `/proc/irq/<irq>/smp_affinity_list` 改写至 `CPU10` 后，`configured` 与 `effective` 掩码虽显示为 `10`、旧核硬中断计数也在 50 ms 后归零，但**活跃跑流期间目标核 `CPU10` 上目标队列向量的硬中断增量全部为 `0`**（其中有 9 次观测到 `CPU10` 的系统聚合 `NET_RX` 出现少量非零增量，说明聚合软中断计数会混入非目标队列活动，不能单独代表目标队列已交接），而旧核 `CPU8` 的 `NET_RX` 仍以每秒约 10 万次持续运行。
   - 在 `4A-rescue-by-tp-irq` 中，将服务端 `TP IRQ (Q8)` 写往 `CPU10` 后，`LAT p99` 仍为 `624.48 µs`（解救前 `659.74 µs`），未能解除 `CPU8` 上的软中断抢占；同时 `52` 发端同一个 `QID` 在双核时间轮上的并发交接与回收边界也未被实际覆盖。
3. **问题 2（长时恶化共置异常 · 明确归因边界）：`4A-rescue-by-tp-irq` 第 2 轮出现一次 `LAT` 停滞**：
   - `compat3.2` 下由于该组 `TP IRQ` 未实际迁离，`LAT APP` 与 `TP IRQ` 持续处于 `CPU8` 共核恶化态；第 2 轮 `LAT` 进度停在 `677,524 / 800,000` 次往返至少 `24 s` 直至观测超时，同期 `TP` 有效吞吐仍维持 `19.92–20.32 Gbps`，微内核未报断连、内核无告警。
4. **问题 3（预热期单次异常 · 根因保留为未知）：`4A-rescue-by-lat-app` 第 3 轮首次硬件测量出现预热期 `TP` 断连**：
   - 核对编排脚本与双端时间线，脚本已先完成 `TP / LAT` 注册并释放 `Q7` 占位进程（双端 `Q7` 释放确认分别早于 `TP` 放行约 `249 ms` 与 `330 ms`），随后才放行 `TP` 发流；`51` 记录到 `Connection is closed by microkernel` 与 `EPOLLERR/HUP` 又早于本机 `LAT` worker 放行约 `170 ms`、早于在线迁核约 `4.9 s`。该次断连与在线迁核无关，具体触发原因保留为未知。

---

## 2. 问题 1 源码级根因剖析

对照 `compat3` 内核源码中的 `drivers/net/ethernet/mellanox/mlx5/core/en_txrx.c`、`en_main.c`、`net/core/dev.c` 与 `kernel/irq/migration.c`，活跃流量下队列 NAPI 无法稳定交接至目标核由以下四处机制共同造成：

### 2.1 `napi_defer_hard_irqs` 绑定旧核定时器截断了 CQ 重武装（`net/core/dev.c:6163–6232`）

[nic.cc](../../eTran/micro_kernel/nic.cc#L14-L24) 默认启用 `gro_flush_timeout = 200`（ns）与 `napi_defer_hard_irqs = 100`。在 `napi_complete_done()` 中：

1. 只要 `work_done > 0`，`n->defer_hard_irqs_count` 就被重置为 `100`；只要计数 `> 0`，每次轮询结束将其减 `1`、置 `timeout = 200 ns` 并将返回值设为 **`false`**。
2. 函数末尾执行 `hrtimer_start(&n->timer, ns_to_ktime(timeout), HRTIMER_MODE_REL_PINNED)`，在**当前旧核**挂起绑定核的 `napi_watchdog` 定时器并返回 `false`。
3. `mlx5e_napi_poll()`（`en_txrx.c:216–217`）见 `napi_complete_done()` 返回 `false` 后直接 `goto out`，**跳过全部 `mlx5e_cq_arm()` 以及强制中断触发**。`200 ns` 后 `napi_watchdog` 在旧核到期，再次通过 `__napi_schedule_irqoff(napi)` 将通道挂回旧核 `poll_list`。在持续 `TP` 或 `14 µs` 往返的 `LAT` 流量下，队列凑不够连续 100 次空轮询，导致旧核硬中断虽归零，软中断轮询却一直留在旧核。

### 2.2 非 `busy` 分支跳过亲和性检查与 `MISSED` 原地重排

1. **非满预算批次未置 `aff_change`（`en_txrx.c:205–214`）**：`mlx5e_channel_no_affinity_change(c)` 仅在 `if (busy)` 块内调用。当 `work_done < budget` 且 `!busy_xsk` 时，`aff_change` 保持 `false`，通道继续走常规 defer 路径留在旧核。
2. **`NAPIF_STATE_MISSED` 原地重排（`dev.c:6223–6226`）**：旧核轮询期间若另一颗核上的应用调用 `mlx5e_xsk_wakeup()`，`napi_if_scheduled_mark_missed()` 会置位 `NAPIF_STATE_MISSED` 而不发 `async_icosq` 中断；随后 `napi_complete_done()` 看到 `MISSED` 直接在**当前旧核**调用 `__napi_schedule(n)` 并返回 `false`。

### 2.3 旧核在途 `napi_watchdog` 回调的并发重排窗口（`dev.c:6450–6466`）

即便退出旧核轮询时不重新启动 `n->timer`，若前一次轮询留下的 `n->timer` 恰好在退出路径执行期间正在硬中断上下文运行 `napi_watchdog()`，非阻塞 `hrtimer_try_to_cancel(&n->timer)` 会返回 **`-1`**（回调仍在执行）。若退出路径紧接着直接清除 `NAPIF_STATE_SCHED`，仍在执行中的旧核 `napi_watchdog()` 随后运行到 `!test_and_set_bit(NAPI_STATE_SCHED, &napi->state)` 时仍可能成功抢回 `SCHED` 并调用 `__napi_schedule_irqoff(napi)`，把通道重新挂回旧核。同时，上一任核尾部的 `hrtimer_start(&n->timer)` 也可能与下一任核的 `hrtimer_try_to_cancel(&n->timer)` 交错。

此外，若退出 helper 因 `NAPIF_STATE_NPSVC | NAPIF_STATE_IN_BUSY_POLL` 拒绝完成而驱动却提前把返回值压到 `work_done < budget`，外层 `net/core/dev.c:__napi_poll()` 看到 `work < weight` 不会置 `*repoll = true`，将丢失下一轮合法重试机会。

### 2.4 无锁 `icosq` 触发竞争与唤醒—接管之间缺少确认闭环

1. **无锁 `c->icosq` 跨核竞争（Linux `6.6.y` 已修复项）**：`compat3.2` 的 `en_txrx.c:250–253` 在 `napi_complete_done()` 释放 `SCHED` 且武装 `rq->cq` 之后，无锁调用 `mlx5e_trigger_irq(&c->icosq)`。若 `rq->cq` 因已有未消费 CQE 立即在新核触发中断并并发进入 `mlx5e_napi_poll()`，旧核写 `c->icosq` 与新核轮询 `c->icosq` 会产生竞争；上游 Linux `6.6.y`（`c326f9c68921`）已将其替换为持 `c->async_icosq_lock` 的 `mlx5e_trigger_napi_icosq(c)`。
2. **单次发唤醒不等于目标核已完成接管**：
   - 若直接复用 `MLX5E_SQ_STATE_PENDING_XSK_TX` 门控，当该位因早先请求已置位时跳过新 kick，并不能保证早先的在途事件一定会落在本次最新目标核上并完成接管；
   - 在底层中断迁移侧（`kernel/irq/migration.c:irq_move_masked_irq`），若 `irq_do_set_affinity()` 返回 `-EBUSY`，Linux 6.6 会重新置位 `IRQD_SETAFFINITY_PENDING` 留待后续中断重试，单次中断不保证底层向量已完成搬迁。
   - 因此，必须在驱动通道内建立**从旧核退出到目标核首次有效 `poll` 确认接管**的闭环状态机，覆盖重试、超时降级与通道关闭路径。

---

## 3. 修补设计与落地实现（`compat3.3`）

内核侧按职责拆分为两个独立补丁（归档于 `e3fa926`），微内核用户态独立提交配套辅助接口（`72a2e5e`）。

### 3.1 补丁一（[0005-mlx5e-use-async-icosq-for-aff-change-irq.patch](../../kernel-patches/0005-mlx5e-use-async-icosq-for-aff-change-irq.patch)）

独立回移 Linux `6.6.y`（`c326f9c68921`）修复：将 `mlx5e_napi_poll()` 末尾 `aff_change && busy_xsk` 分支中的无锁 `mlx5e_trigger_irq(&c->icosq)` 替换为持 `spin_lock_bh(&c->async_icosq_lock)` 的 `mlx5e_trigger_napi_icosq(c)`，并校验 `MLX5E_SQ_STATE_ENABLED`。

### 3.2 补丁二（[0006-handoff-deferred-napi-on-irq-affinity-change.patch](../../kernel-patches/0006-handoff-deferred-napi-on-irq-affinity-change.patch)）

保留同网卡其余通道的 `gro_flush_timeout` / `napi_defer_hard_irqs` 配置与现有 XSK 绑定不变，仅对发生亲和性迁移的通道执行闭环交接：

1. **原子封堵旧核 `napi_watchdog` 重排与定时器启停串行化（`include/linux/netdevice.h` & `net/core/dev.c`）**：
   - 新增 `NAPI_STATE_SUPPRESS_WATCHDOG`（`NAPIF_STATE_SUPPRESS_WATCHDOG`）状态位及 `raw_spinlock_t watchdog_lock`；
   - `napi_watchdog()` 改为在 `napi->state` 同一机器字上执行 `try_cmpxchg`，遇到 `DISABLE | SCHED | SUPPRESS_WATCHDOG` 任一位即返回 `HRTIMER_NORESTART`；
   - `napi_complete_aff_change()` 在 `watchdog_lock` 保护下先调用 `hrtimer_try_to_cancel(&n->timer)`，再通过单次 `try_cmpxchg` **原子清除 `SCHED | MISSED | SCHED_THREADED | PREFER_BUSY_POLL` 并置位 `SUPPRESS_WATCHDOG`**，从根源消除旧核回调在 `SCHED` 释放后抢回队列或上一任核尾部误取消下一任核定时器的窗口；若命中 `NPSVC | IN_BUSY_POLL` 则不动 `SCHED` 返回 `false`；
   - 目标核后续调用常规 `napi_complete_done()` 时，在 `watchdog_lock` 下仅当 `hrtimer_try_to_cancel(&n->timer) >= 0`（确认旧核回调已不在排队也不在执行）时才清除 `SUPPRESS_WATCHDOG` 并在新核重新启用 `n->timer`；`napi_disable()` 与 `napi_enable()` 同步清理该标志。
2. **以目标核首次有效 `poll` 为完成条件的通道交接状态机（`mlx5/core/en.h`、`en_main.c`、`en_txrx.c`、`en_stats.*`）**：
   - 每个通道维护 `irq_data`、`handoff_lock`、`handoff_timer`（`HRTIMER_MODE_REL_PINNED_SOFT`，`1 ms` 周期，上限 `50` 次）、`MLX5E_CHANNEL_STATE_HANDOFF_{PENDING,FAILED,STOP}` 以及 `ethtool -S` 导出的通道级计数 `aff_handoff_{done,retry,timeout}`；
   - **完成条件**：在非线程化、非 `NPSVC / IN_BUSY_POLL` 的常规 `mlx5e_napi_poll()` 中，仅当 `!irq_pending && cpumask_test_cpu(smp_processor_id(), c->aff_mask)` 成立时视为到达目标核，清除 `HANDOFF_PENDING`、取消 `handoff_timer` 并递增 `aff_handoff_done`；
   - **安全返回与有界唤醒**：当 `mlx5e_handoff_poll(c)` 请求交接时，若 `napi_complete_aff_change()` 返回 `false`，驱动返回 `budget` 交由外层 `__napi_poll()` 安排下一轮合法重试；若返回 `true`，将返回值压为 `min(work_done, budget - 1)`，武装全部 CQ 后在 `handoff_lock` 与 `async_icosq_lock` 保护下检查 `mlx5e_wqc_has_room_for(&sq->wq, READ_ONCE(sq->cc), sq->pc, sq->reserved_room + 2)` 发送跨核接管中断（预留普通 XSK 唤醒与系统保留槽位，不与 `PENDING_XSK_TX` 互相干扰）；
   - **超时回退与通道停用**：若 50 次重试仍未在目标核完成首次 `poll`，置 `HANDOFF_FAILED`、打印告警并通过 `async_icosq` + `napi_schedule()` 保活数据面；`mlx5e_deactivate_channel()` 调用 `mlx5e_handoff_stop()` 置 `HANDOFF_STOP` 并同步 `hrtimer_cancel(&c->handoff_timer)`。

### 3.3 微内核用户态配套接口（`72a2e5e`）

1. **带动作名校验的队列 `QID → IRQ` 缓存（[nic.h](../../eTran/micro_kernel/nic.h) / [nic.cc](../../eTran/micro_kernel/nic.cc)）**：`create_nic()` 一次性解析缓存 `_queue_irqs`，`get_queue_irq(qid)` 读取 `/sys/kernel/irq/<irq>/actions` 核对 `mlx5_comp<qid+1>@pci:<pcie>` 名称一致后直接命中缓存，避免运行期 `set_queue_cpu()` 全表重扫 `/proc/interrupts`。
2. **带失败自动回滚的进程全线程绑核接口（[thread_affinity.h](../../eTran/micro_kernel/utils/thread_affinity.h) / [thread_affinity.cc](../../eTran/micro_kernel/utils/thread_affinity.cc)）**：`set_process_thread_affinity(pid, cpu)` 遍历 `/proc/<pid>/task/*` 记录旧掩码并逐线程调用 `sched_setaffinity`（忽略中途退出线程的 `-ESRCH`，遇到其他错误时自动将已迁移线程回滚至原掩码）。

---

## 4. 异常现象的归因与处置边界

1. **`4A-rescue-by-tp-irq` 长时共置 `LAT` 停滞**：`compat3.3` 下迁走 `TP IRQ` 缩短了 `③` 的共置时间，三轮 `800,000` 次 RTT 均无停滞跑满，完成了**在线迁走 `TP IRQ` 的解救效果验收**；但长时处于 `③` 共置恶化态时的协议栈/应用停滞（`FLAG_TO`、`RECOVERY_MASK` 或软中断饥饿线索）并未在此定位或推定已修复，保留为独立待查项。
2. **预热期单次断连与多流缓冲上限**：
   - `compat3.2` 中 `4A-rescue-by-lat-app` 预热期单次断连根因保留为未知；
   - `compat3.3` 时间轮探路时，单线程 8 连接（`8 × 10 MB`）在迁核前预热期发出约 `49.6 MB` 后停止增长（同期单次 `Q8` NAPI 交接本身仍正常完成），不计入迁移性能统计，原始双端日志已留存供后续独立排查。

---

## 5. `6.6.0-eTran-compat3.3` 双机实测验收结果

双机按“先 `108-52` 单次启动核验，通过后再切 `108-51`”完成升级，采用“短窗按 `QID` 诊断轮 + 关探针正式性能轮”分门禁验收。18 轮关探针正式测量中的 **55/55 次活跃期 `IRQ` 切换全部完成目标核接管确认（`aff_handoff_done` 全匹配，`aff_handoff_retry = 0`、`aff_handoff_timeout = 0`、零应答秒窗 `= 0`、端点错误 `= 0`、内核新增告警 `= 0`）**：

### 5.1 单向迁移、高频切换与纯 `LAT` 迁移汇总

| 验收阶段与用例 | 诊断轮按 `QID` 直接核验结果 | 关探针正式轮表现（中位数 / 极值） |
| --- | --- | --- |
| **`52` 单台先行试跑**（`Q8: CPU8 → 10`） | 写返回后 `7.012 µs` 目标核首次 `poll`，此后 ~950 ms 窗 `CPU8` 零 `Q8 poll` | 12/12 完整秒窗均有应答，`done=1, retry=0, timeout=0` |
| **门禁一 · `51` 收端单次**（`1A-single`） | 旧核末次 `poll` 至新核首次 `poll` 间隔 **`32.656 µs`**（写返回前 `3.039 µs` 已在新核首轮 `poll`），旧核彻底停轮询 | 3 轮：迁前/迁后 MAC Rx 为 `21.04 → 21.04 Gbps`，过渡最低 50 ms 为 `20.60–21.10 Gbps`，应用最低秒窗 `19.05–20.00 Gbps` |
| **门禁二 · `51` 收端高频往返**（`1A-burst`） | `200 ms × 20` 次切换，`20/20` 次目标核接管，新旧 `poll` 间隔 **`11.92–47.61 µs`** | 1 轮：`20/20` 完成，全序列最低 50 ms MAC Rx **`20.68 Gbps`**，最低应用秒窗 **`20.16 Gbps`** |
| **门禁二 · `52` 发端单流**（`1B-single`） | 新旧 `poll` 间隔 **`10.75 µs`**（写返回前 `3.432 µs` 已接管） | 3 轮：MAC Tx `21.14 → 21.18 Gbps`，过渡最低 50 ms 中位数 **`20.96 Gbps`**，重传增量 `0` |
| **门禁二 · `51` 纯 `LAT` 迁 `IRQ`**（`3A-lat-irq`） | `Q6: CPU6 → 10`，新旧 `poll` 间隔 **`13.98 µs`**（写返回前 `12.766 µs` 已接管） | 3 轮：迁前/迁后 `p99` 为 **`14.19 / 14.18 µs`**；`±50 ms` 最大 RTT 为 **`24.27–26.91 µs`**，**超 `30/50/100/500 µs` 样本数全为 `0`** |
| **门禁二 · `51` `TP APP` 回归**（`2B-reg`） | `set_process_thread_affinity` 调用返回后 `0.54 ms` 首个快照确认 worker 落入 `CPU4` | 1 轮：最低应用秒窗 **`20.00 Gbps`** |

### 5.2 混跑恶化现场 `③ LAT APP + TP IRQ` 在线解救对照

| 内核与在线动作（服务端 `51`） | 解救前 `LAT p99 / p99.9` (µs) | 过渡窗 `p99 / max` (µs) | 解救后 `LAT p99 / p99.9` (µs) | 迁前 → 迁后 `TP` 吞吐 (Gbps) | 完成 80 万次 RTT |
| --- | ---: | ---: | ---: | ---: | ---: |
| `compat3.2` 写 `TP IRQ`（未交接） | `659.74 / 758.17` | `627.65 / 866.32` | `624.48 / 741.98` | `19.88 → 19.87` | `2/3` |
| `compat3.2` 迁 `LAT APP`（`8 → 2`） | `677.44 / 762.99` | `53.47 / 756.06` | **`19.70 / 24.52`** | `19.84 → 19.57` | `3/3` |
| **`compat3.3` 迁 `TP IRQ`（`8 → 10`）** | `637.68 / 737.40` | `51.12 / 722.12` | **`19.63 / 24.54`** | `19.76 → 19.46` | **`3/3`** |

`compat3.3` 下三轮均在写后连续两个 50 ms 窗口内达到 `p99 ≤ 25 µs`（`100 ms` 确认恢复），解救效果与迁 `LAT APP` 完全相当。

### 5.3 `52` 发端双核时间轮重叠提交与停流回收专项（`1B-paced`）

- **四连接同队列（每流上限 `600 MB/s`）**：`200 ms × 20` 次 burst 诊断轮 `20/20` 完成交接，其中第 2 次（`10 → 8`）与第 15 次（`8 → 10`）直接捕获到旧核末次成功提交晚于目标核首次提交 **`117.733 µs` 与 `21.640 µs`** 的双核重叠；关探针单次 3 轮 MAC Tx 为 `16.85 → 最低 16.22 → 16.87 Gbps`（`49–94 ms` 恢复 95%），burst 正式轮最低 50 ms 为 `16.66 Gbps`。
- **同队列“一慢三快”四连接（`60 / 600 / 600 / 600 MB/s`）**：两轮单次诊断直接捕获到 `CPU8 → CPU10` 长达 **`314.831 µs` 与 `628.158 µs` 的双核时间轮重叠提交**，随后 `CPU8` 停发、`CPU10` 持续提交并回收；关探针三轮最大 50 ms 速率降幅仅 **`3.74%`**（`55.5–98.2 ms` 恢复 95%）。
- **零重传与零残留回收**：上述全部诊断与正式轮的 `cnt_tx_drops` 重传增量均为 **`0`**；连接关闭后实测 `xsksq`、`xsk_pace_sq`、`pool->cq` 未发布预留、软件暂存完成队列及双核 `pkt_queue` 时间轮待发计数**全部精确归零**。

### 5.4 运行期调度基线结论与默认引导项

1. **`APP` 与 `IRQ` 双通道热迁移均已就绪**：在本双机环境与负载下，`T_cooldown_irq` 与 `T_cooldown_app` 初始均可取 **`200 ms`**（覆盖 `< 100 ms` 的性能恢复与双窗口确认时间），每次调度动作后须核对目标核实际落核/接管计数与业务指标恢复。
2. **默认引导项更新**：`108-51` 与 `108-52` 的 GRUB 默认菜单项均已设为 **`etran-compat3-3-24`**（`6.6.0-eTran-compat3.3`），保留 `etran-compat3-2-24`、`etran-compat3-1-24` 与 `etran-experiment` 回退项，实验后双端已恢复 24 通道默认配置与 `irqbalance=active`。
