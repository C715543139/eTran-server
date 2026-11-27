# eTran 动态核池调度 03 版实施计划：门禁闭环、协议修订与 Batch 3 分步规范

本文档承接 [01-metrics-and-scheduling-design.md](01-metrics-and-scheduling-design.md) 与 [02-g0-calibration-and-adjustments.md](02-g0-calibration-and-adjustments.md)，结合分支 `codex/dynamic-core-scheduling@7047771` 上 Batch 2A/2B 的完整实测数据（本地归档于 `experiments/20261126-dynamic-scheduler/README.md`）与源码审查结论，明确 **G0 未闭环项的处置口径、G1 精确数据口径、安全回滚与无竞态协议修订**，以及 **先只读 `shadow` / 协议与夹具、后单步受控验收、最后开启 `active` 自动迁核** 的分步实施规范。

---

## 1. G0 / G1 现状核定与前置门禁边界

### 1.1 G0 现状：按原 0.5 µs p99 门槛保留为“未通过”，须明确闭环后才默认启用计数与 `active`

核对 Batch 2A 每组 6 轮（LAT）及 3 轮（TP / 64 B）合并数据：

| 组别 | 10 MB TP (Gbps) | 64 B 并发 (Mops/s) | LAT `p50` (µs) | LAT `p99` (µs) | LAT `p99.9` (µs) | LAT 6 轮 `p99` 落点 |
| :--- | ---: | ---: | ---: | ---: | ---: | :--- |
| 旧基线（`72a2e5e`） | `20.080` | `1.1843` | `11.027` | `14.544` | `17.322` | 5 轮 `14.36–15.01` + 1 轮 `16.164` |
| 新版关闭（`off`, 0 计数指令） | `20.150` | `1.1903` | `11.110` (`+0.08`) | `15.540` | `17.503` (`+0.18`) | 3 轮 `14.71–14.92` + 3 轮 `16.164–16.269` |
| `.bss` 仅计数（`counters`） | `20.105` | `1.1823` (`-0.17%` / `-0.67%`) | `11.065` (`+0.04`) | **`16.105`** | `17.675` (`+0.17` / `+0.35`) | 2 轮 `14.83–15.27` + 4 轮 `16.04–16.23` |
| `.bss` + `softirq`（`probe`） | `20.070` | `1.1787` (`-0.47%` / `-0.97%`) | `11.553` (`+0.53`) | `14.982` | `18.211` (`+0.89`) | 6/6 紧聚 `14.80–15.30` |

1. **严格维持 G0 为“未通过 / 待闭环”**：
   - 虽然 64 B 小包回退已压至 `-0.17%`（vs 旧基线）/ `-0.67%`（vs `off`），满足 `< 1%` 目标，且 `counters` 的 `p50`（`+0.04 µs`）与 `p99.9`（`+0.17 µs` vs `off` / `+0.35 µs` vs 基线）变化微弱，但按 [02 文档](02-g0-calibration-and-adjustments.md) 原定 **LAT `p99 < 0.5 µs`** 门槛，6 轮合并后 `counters` 仍比同版 `off` 高 **`0.565 µs`**、比旧基线高 **`1.562 µs`**；
   - 多组命中同一个 `16.164 µs` 仅说明 `p99` 对样本分布敏感，**不足以证明计数未提高样本落入高延迟区间的比例**，`p50 / p99.9` 达标亦不能暗中替代原 `p99` 指标。
2. **G0 闭环前提（在开启默认计数与 `active` 自动迁核前二选一完成）**：
   - **路径 A（定位并压缩）**：先查明 `off (3/6)` 与 `counters (4/6)` 命中 `16.164 µs` 频次高于旧基线（`1/6`）的原因（含高延迟区间样本占比、50 ms 跨核读 `.bss` 缓存行干扰及重编布局因素），将 `counters` 的 `p99` 差距压回 `< 0.5 µs`；
   - **路径 B（显式口径变更）**：若分布分析证实差距来自极窄的高延迟区间波动且占比增量可接受，须在文档中**正式记录验收口径变更**（如改为 `p50 / p99.9` + 高延迟区间占比上限 + 64 B `< 1%`）并经人工确认，再正式置 G0 为通过。
3. **补齐无探针（`ETRAN_SOFTIRQ_PROBE=0`）画像准确率验证**：
   - Batch 2A 中的 `light-1g`、`light-2g` 与 `multi-q8-q6` 均在开启 `raw_tp/softirq` 下运行；由于计划将 `ETRAN_SOFTIRQ_PROBE=0` 作为默认配置（`probe` 在 6 轮中稳定引入 `+0.53 µs p50` / `+0.89 µs p99.9`），必须补跑上述 3 组在 **`ETRAN_SOFTIRQ_PROBE=0`** 下的实测，验证关闭探针后 1G/2G 轻载 `TP` 与多队列共核归因准确率同样达标。
4. **明确各模式的遥测采集契约（消除默认歧义）**：
   - `ETRAN_SCHED_MODE=off`（系统默认值）且未显式设置 `ETRAN_QID_STATS=1`：**严禁采集**（`qid_stats_enabled = 0`，加载后 BPF 指令核验 0 引用）；
   - `ETRAN_SCHED_MODE=shadow`、`admit_only`、`manual_actuate` 或 `active`：**强制启用 `.bss per-QID` 采集**（`qid_stats_enabled = 1`），`ETRAN_SOFTIRQ_PROBE` 默认为 **`0`**（保留手动置 `1` 诊断开关）。

### 1.2 G1 核定：保留 `app_to_irq` 为初始默认，并修正报告口径

- Batch 2B 消除客户端 `std::vector` 扩容伪影后（原始逐轮明细见 `experiments/20261126-dynamic-scheduler/batch2b-g1-1795684295/transition-summary.json`），两方向 12 次动作的 `> 100 µs` 与 `> 500 µs` 样本数均为 `0`：
  - **`app_to_irq` 收缩**：过渡窗逐轮最大 RTT 的中位数为 **`74.78 µs`**（单轮最高 **`79.17 µs`**），收缩过渡窗 `p99` 的中位数为 **`23.48 µs`**，折叠后稳态 `p99` 约为 **`24.80–25.03 µs`**，三轮均在写入后连续两个 50 ms 窗满足 `p99 ≤ 25 µs` 恢复判据（`100 ms` 确认），展开时 TP 恢复中位数 `94.95 ms`；
  - **`irq_to_app` 收缩**：过渡窗逐轮最大 RTT 中位数 **`66.47 µs`**（单轮最高 `66.82 µs`），动作确认中位数 **`83.49 µs`**，但折叠后两轮稳态 `p99` 为 `25.15 / 25.34 µs`（仅 1/3 命中 `≤ 25 µs` 判据），展开时 TP 恢复中位数 `135.53 ms`。
- **结论**：保留 **`ETRAN_FOLD_MODE=app_to_irq`** 作为初始默认方向并保留 `irq_to_app` 对照开关；合作式单 `TID` 执行器实现后，须按同一口径（逐轮最大 RTT、过渡窗 `p99`、折叠稳态 `p99` 及连续双窗 `≤ 25 µs`）复测验证。

---

## 2. 分步推进顺序（严格解耦“只读/协议底座”与“自动迁核”）

为避免在 G0 未闭环或回滚未验证前引入自动迁核风险，Batch 3 严格按三步交付：
1. **Step 1（立即执行，不开启自动迁核）**：
   - 完成 G0 尾分布排查（或显式口径确认）与 `ETRAN_SOFTIRQ_PROBE=0` 下 1G/2G/多队列画像补测；
   - 实现协议扩展（`AUTO`、`err_code`、单字序号化合作式自绑、多 worker 全槽位 `TID` 登记）、只读 `shadow` 决策引擎、统一 `MigrationExecutor`（含 `ROLLING_BACK` 与 `QUARANTINED` 强一致状态机）及单步受控验收入口。
2. **Step 2（门禁验收，仅使用独立准入与单步受控入口）**：
   - 依次验收 `G2-shadow`、`G2-admit-4c`、`G2-infeasible`（精确核验 `-ENOSPC`）、`G2-cc-jitter`、`G3-quiesce`（含失败回滚与迁移中强杀立即同 QID 重绑 20 轮）以及合作式 `G1-coop-check`。
3. **Step 3（G0 + G2 + G3 全部闭环后放行 `active`）**：
   - 启用 `ETRAN_SCHED_MODE=active`，执行 `G2-admit-3c` 运行期自动折叠闭环与全场景矩阵 `S1–S5`。

---

## 3. 核心工程规范修订（针对 4 项架构/并发隐患的精确修复）

> **ABI 重编纪律**：涉及 [intf.h](../../eTran/common/intf/intf.h) 与 `shm_lrpc` 结构变化，双端构建前必须先执行 `make -C eTran clean && make -C eTran`。

### 3.1 协议扩展与序号化合作式单 `TID` 自绑（修复发布竞态、入睡丢失与多 worker 槽位）

1. **三档 SLO 与错误码透传（[eTran/common/intf/intf.h](../../eTran/common/intf/intf.h)、[eTran/micro_kernel/control_plane.cc](../../eTran/micro_kernel/control_plane.cc#L836-L890)、[eTran/lib/eTran_common.cc](../../eTran/lib/eTran_common.cc#L518-L556)）**：
   - `enum tcp_type` 新增 `AUTO`（用户态对应 `ETRAN_TCP_AUTO`），`ETRAN_TCP_TYPE` 未设置或设为 `"AUTO"` 时默认 `AUTO`；
   - `struct register_response` 新增 `int32_t err_code;`（`REG_FAIL` 时写入标准负错误码如 `-ENOSPC`、`-EINVAL`、`-E2BIG`）；`eTran_common.cc` 收到 `REG_FAIL` 时直接返回 `resp.err_code < 0 ? resp.err_code : -EIO`（修复原代码统一返回 `-EIO` 导致无法验收 `-ENOSPC` 的问题）。
2. **单原子字一致命令快照 + 序号应答（消除 `desired_cpu / seq` 撕裂与迟到执行）**：
   - 在每线程 `shm_lrpc` 尾部追加双 cacheline 对齐的 `struct worker_sched_ctrl`（微内核只写 Cacheline 0、worker 只写 Cacheline 1，消除伪共享）：
     ```c
     #define WORKER_FLAG_SLEEPING 0x1u

     struct worker_sched_ctrl {
         /* Cacheline 0: microkernel writes, worker reads */
         uint64_t cmd_word;        // [63:32] seq (0=none), [31:0] (uint32_t)(int32_t)target_cpu
         uint8_t  pad0[56];
         /* Cacheline 1: worker writes, microkernel reads */
         uint64_t ack_word;        // [63:32] applied_seq, [31:0] result (0=OK, >0=errno)
         int32_t  applied_cpu;     // worker 自绑成功后的目标核
         uint32_t state_flags;     // bit0: WORKER_FLAG_SLEEPING
         uint8_t  pad1[48];
     } __attribute__((aligned(64)));
     ```
   - **按序号执行（不再只比 CPU 号）**：worker 仅在 `cmd_seq != 0 && cmd_seq != tctx->last_applied_seq` 时执行 `sched_setaffinity(0, ...)`，并立即原子 `store_release(&ctrl->ack_word, ((uint64_t)cmd_seq << 32) | rc)` 与刷新 `applied_cpu`、`last_applied_seq = cmd_seq`。即使发生“迁移至 `CPU4` → 撤销回 `CPU0`（同目标新序号）”或 worker 迟到读取，也能精确按最新 `seq` 应答，绝不漏单或错用旧指令。
3. **消除入睡竞争（Store-Load 双向校验）+ 严格 `eventfd` 1:1 契约**：
   - Worker 进入阻塞 `libc_epoll_wait(timeout != 0)` 前（[eTran/lib/eTran_posix.cc](../../eTran/lib/eTran_posix.cc#L583)）：
     1. 原子置位 `state_flags |= WORKER_FLAG_SLEEPING`（`__ATOMIC_SEQ_CST`）；
     2. **立即重读 `cmd_word`**：若此时已有未应答新 `cmd_seq`，先清位 `WORKER_FLAG_SLEEPING` 并原地执行自绑，不进入阻塞；
     3. 否则进入 `libc_epoll_wait`，返回后立刻清位 `WORKER_FLAG_SLEEPING` 并再次检查 `cmd_word`。
   - 微内核下发 `cmd_word`（`__ATOMIC_SEQ_CST`）后立即读取 `state_flags`：若命中 `WORKER_FLAG_SLEEPING`，或在 `T_wake_retry = 5 ms` 后仍未见 `ack_seq == cmd_seq`，向 `control_loop()` 提交一次轻量 `APPIN_TCP_SCHED_REBIND` 唤醒；
   - **`control_loop()` 严格纪律**：仅在 `lrpc_send(&tctx->kernel_out, &rebind_msg) == 0` **入队成功后**才调用 `notify_app(tctx->evfd)`；若队列满（返回非 0），保留待唤醒标记于下一循环重试，**严禁在 LRPC 未入队时写 `evfd`**，确保与 [eTran_posix.cc#L446-L580](../../eTran/lib/eTran_posix.cc#L446-L580) 中的 `nr_eventfd_work` / `pending_eventfd_work` 严格一致。
4. **多 worker 全槽位 `TID` 登记与清理**：
   - 修复 [control_plane.cc#L1794-L1804](../../eTran/micro_kernel/control_plane.cc#L1794-L1804) 仅接受 `nr_app_threads == 1 && worker_idx == 0` 的硬限制：放开为校验 `identity.worker_idx < app->nr_app_threads && tctx == &app->tctx[identity.worker_idx] && identity.os_tid > 0`，按 `(app_gen, worker_idx)` 独立登记与注销清理全部 worker 槽位；
   - 在 `alloc_tctx()` 中同步执行 `sched_setaffinity(0, ...)` 单 worker 首绑至 `tcp_state.cpus[idx]`；未完成 `TID` 登记的 worker 一律暂缓迁移，绝不对多 worker 进程回退整进程绑核。

### 3.2 统一 `MigrationExecutor`：确认、安全回滚（`ROLLING_BACK`）与核隔离（`QUARANTINED`）

所有迁移（含注册腾核、单步受控迁移、运行期重平衡）统一走同一个 6 态状态机：**`STEADY → MIGRATING → (CONFIRM | ROLLING_BACK) → (COOLDOWN | QUARANTINED) → STEADY`**。

1. **活跃 `IRQ` 的复合确认与禁止积压静默成功**：
   - 仅当存在活跃 `IRQ` 处于 `MIGRATING` 或 `ROLLING_BACK` 时，按整张网卡批量执行一次 `ioctl(SIOCETHTOOL, ETHTOOL_GSTATS)`（平时调用频次为 `0`）；
   - **活跃确认条件**：必须**同时**满足 `aff_handoff_done > base_done`、`aff_handoff_timeout == base_timeout` **且** `/proc/irq/<irq>/effective_affinity_list` 指向 `dst_cpu`；
   - **静默降级条件（收紧）**：仅当在 `T_handoff_quiet = 100 ms` 内无新包进展、`effective_affinity_list == dst_cpu`、`timeout == base_timeout`，**且确认无积压与停滞**（`bpf_cc->txp == 0`、`tw_pending_by_cpu[src_cpu] == 0`、源核无异常软中断活跃）时，才按静默成功确认；**存在积压或无进展停滞的队列严禁走静默成功**，超时直接进入 `ROLLING_BACK`。
   - **空闲 `IRQ` 队列**（`IDLE`）：写后核对 `smp_affinity_list` 与非 pending 时的 `effective_affinity_list` 即确认完成，不等待 `aff_handoff_done`。
2. **超时/失败安全回滚（绝不在未确认实机回位前释放 `dst_cpu`！）**：
   - 无论 `IRQ` 还是 `APP` 在 `MIGRATING` 超时（`T_handoff_max = 150 ms` / `T_app_max = 50 ms`）或命中 `aff_handoff_timeout > base_timeout`，**禁止直接清除 `dst_cpu` 的 `pending_in`**！必须先转入 **`ROLLING_BACK`** 并继续持有 `src_cpu` 与 `dst_cpu` **双核预留**：
     - **`IRQ` 回滚**：重新写 `/proc/irq/<irq>/smp_affinity_list` 为 `src_cpu` 并记录新基线 `rb_base_done`，按同一确认条件等待 `IRQ` 实际回到 `src_cpu`（限时 `T_rollback_max = 150 ms`）；
     - **`APP` 回滚**：发布新序号回滚命令 `{seq_rb, src_cpu}`（必要时唤醒），等待 `ack_seq == seq_rb && ack_rc == 0` 且只读 `/proc/<pid>/task/<tid>/status` 确认 `Cpus_allowed_list == src_cpu`（活跃线程确认 `processor == src_cpu`）；
     - **分支 A（确认已回到 `src_cpu`）**：释放 `dst_cpu` 预留，置退避冷却 `T_backoff = 1000 ms`，释放全局迁移槽；
     - **分支 B（回滚前实机已迟到完成并稳落 `dst_cpu`）**：若 `APP` 已回报原 `cmd_seq` 成功且实机核位稳在 `dst_cpu`（或 `IRQ` 实机已稳在 `dst_cpu` 且未改回 `src_cpu`），尊重实机事实**按已知实际核位落账 `dst_cpu`** 并释放 `src_cpu`，避免账本与实机脱节；
     - **分支 C（`T_rollback_max` 到期仍无法确认实机归属，或 worker 拒绝合作）**：保持已知实机核位记账，同时将存在残留风险的受影响核置为 **`QUARANTINED`（隔离核，禁止分配任何新任务）** 并输出严重告警，直至后续采样明确澄清核位或关联应用退出排空后解除隔离。
3. **注销排空栅栏（防 QID 复用 ABA）**：
   - [control_plane.cc#L715-L740](../../eTran/micro_kernel/control_plane.cc#L715-L740) 归还 `qid` 前必须同步调用 `scheduler_cancel_and_quiesce(app_gen)`，等待该 `app_gen` 的所有在途写入/回滚彻底退出、释放双核预留（含解除因该应用挂起的 `QUARANTINED` 标记）并递增 `qid_gen[qid]` 后，才将 `qid` 归还空闲池。

### 3.3 异步注册所有权边界与非阻塞 UDS 状态机（[control_plane.cc](../../eTran/micro_kernel/control_plane.cc)）

不再把整个 `alloc_app_resources()` 直接丢给管理线程，而是采用 **控制线程独占对象生命周期 + 管理线程无状态作业 + 非阻塞 UDS 状态机**：

1. **严格所有权划分**：
   - **`control_loop()` 独占**：`active_apps`、`app_ctx` 内存、UDS `app_fd` 读写与关闭、QID 借出/归还、`XSK` 创建/销毁、`BPF xsks_map` 更新、`LRPC` 通道收发与 `poll_tcp_cc_to()`；
   - **`mgmt_sched_thread`（绑于 `ETRAN_SCHED_CPU`）仅处理值拷贝任务**：接收带 `(job_id, app_gen, pid, qids[], nr_app_threads, slo, requested_cpus)` 的注册/注销慢作业，负责 **准入核决策、注册期腾核、调用 `ethtool -X` 创建/删除 RSS context、写 `smp_affinity_list`**，执行完毕通过线程安全完成队列把结果（`err_code`、`rss_ctx_id`、最终核位列表）交回 `control_loop()`；
   - **注册中断开与 FD 复用防线**：若 `app_fd` 在 `UDS_WAIT_MGMT` 期间触发 `EPOLLHUP/EPOLLERR`，`control_loop()` 立即关闭 `app_fd` 并将该 `app_gen` 记入撤销集合（不复用 `app_gen`）；当 `mgmt_sched_thread` 返回已撤销 `app_gen` 的完成结果时，`control_loop()` 不触碰任何 FD，直接投递清理任务销毁刚建好的 RSS context 并归还核预留与 `qid`。
2. **非阻塞 UDS 部分 I/O 与 `SCM_RIGHTS` 状态机**：
   - 将每个连接 `app_fd` 设为 `O_NONBLOCK`，在 `app_ctx` 中维护显式收发状态（`UDS_RECV_REQ`、`UDS_RECV_REQ_CPU`、`UDS_WAIT_MGMT`、`UDS_SEND_XSK`、`UDS_SEND_EVFD`、`UDS_SEND_FINAL`、`UDS_READY`）及逐字段进度（`buf_offset`、`fd_batch_idx`）；
   - 遇 `EAGAIN/EWOULDBLOCK` 时动态挂载 `EPOLLOUT` 等待下次可写，取代同步阻塞的 `read_all` / `write_all` / `transfer_fds`；
   - 在 `control_loop()` 内建 `poll_tcp_cc_to()` 相邻调用间隔统计（总次数、`p50 / p99 / max` 及 `> 300 µs / > 500 µs` 超期计数），直接验收并发注册与迁核时的 CC 环路稳定性。

---

## 4. 可执行门禁与全场景验收合同

### 4.1 独立开关与单步受控验收入口（消除 `shadow` 与 `active` 的门禁循环依赖）

新增两个正交控制开关及受控单步入口，使 `G2 / G3` 在开启全自动 `active` 之前即可独立闭环验收：
- **`ETRAN_ADMISSION_ENABLE=0|1`**：允许在不开启运行期自动重平衡（`ETRAN_SCHED_MODE=shadow` 或 `off`）时，独立启用新的二维准入算法验收 `G2-admit-*` 与 `G2-infeasible`；
- **单步受控迁移入口（仅 `ETRAN_SCHED_MODE=manual_actuate` 启用）**：通过微内核管理控制通道/命令触发**单次指定动作** `migrate(entity=qid|worker, dst_cpu)`，直接走正式 `MigrationExecutor` 与合作式单 `TID` 执行器，用于验收 `G3-quiesce`、`G1-coop-check` 与失败回滚，绝不提前放开全自动循环迁核。

### 4.2 门禁验收矩阵（Step 1 & Step 2 必过）

| 门禁编号 | 执行模式与前提 | 验收编排与通过标准 |
| :--- | :--- | :--- |
| **`G0-close`** | `off / counters` + 无探针 1G/2G/多队列 | ① 完成 16.164 µs 高延迟占比排查或正式确认 G0 验收口径变更；② 在 **`ETRAN_SOFTIRQ_PROBE=0`** 下补跑 `light-1g`、`light-2g`、`multi-q8-q6`，确认 100% 准确判为 `LIGHT` / `HEAVY+LIGHT` |
| **`G2-shadow`** | `ETRAN_SCHED_MODE=shadow` | 4 核与 3 核混跑制造 `0 / ② / ③ / ④`，核对 `sched_decision` 日志精确命中 `R1/R2` 或稳态无误报 |
| **`G2-admit-4c`** | `ADMISSION_ENABLE=1`, 非 `active` | 4 核池按双向到达顺序间隔 `< 50 ms` 连续注册 `TP + LAT`，零迁移落入 4 核全分离（`~19.6G / ~20 µs`），双端核位无堆叠 |
| **`G2-admit-3c-init`** | `ADMISSION_ENABLE=1`, 非 `active` | 3 核池“先 `TP` 后 `LAT`”由准入直接落入甜点 ②（`~19.7G / ~24–29 µs`）；“先 `LAT` 后 `TP`”准入落入 `TP APP` 独核 + `TP IRQ` 借 `LAT IRQ` 核（运行期自动折叠留待 `active` 门禁 `S0` 验收） |
| **`G2-infeasible`** | `ADMISSION_ENABLE=1`, 非 `active` | 3 核池满载 `TP + 重载 LAT`（或接入第 3 忙轮询应用）：① `reject` 下客户端精确收到 **`-ENOSPC`** 且既有业务零波动；② `degrade` 下按低 SLO 降级并记 `DEGRADED`，不破 `LAT` 红线 |
| **`G2-cc-jitter`** | 并发注册 + 迁核 | 报告 `poll_tcp_cc_to()` 调用间隔 `p50 / p99 / max` 与 `> 300 µs / > 500 µs` 超期次数，验证无 `ethtool -X` 同步阻塞尖峰 |
| **`G3-quiesce`** | `manual_actuate`（单步驱动） | ① 在 `MIGRATING` 窗口强杀应用并立即同 `QID` 重启新应用 **20 轮**，核对 `cancel_and_quiesce` 拦截迟到写入、账本零泄漏、`dmesg` 零告警；② 注入不合作/超时用例验证 `ROLLING_BACK` 确认回源核后再释放目标核（及无法确认时准确进入 `QUARANTINED`） |
| **`G1-coop-check`** | `manual_actuate`（单步驱动） | 正式合作式单 `TID` 执行器复测 3 轮 `app_to_irq` `4 → 3 → 4`（预分配时间戳缓冲），按统一口径汇报过渡窗最大 RTT（中位数与极值）、过渡窗 `p99`、折叠稳态 `p99`、连续双窗 `≤ 25 µs` 恢复耗时及 `T_actuate` |

### 4.3 `active` 模式全场景矩阵（Step 3：`G0 / G2 / G3` 全通后启动）

开启 `ETRAN_SCHED_MODE=active`（`ETRAN_QID_STATS=1, ETRAN_SOFTIRQ_PROBE=0, ETRAN_FOLD_MODE=app_to_irq`），双端全程记录生效核位，所有动态切换分列汇报 **`T_detect` / `T_actuate` / `T_recover`**：

| 场景编号 | 核池与精确编排 | 预期行为与验收目标 |
| :--- | :--- | :--- |
| **`S0` 3 核先 `LAT` 后 `TP` 自动折叠闭环** | 3 核池先启 `LAT`（占 2 核），再启满载 `TP` | 初始 `TP IRQ` 借 `LAT IRQ` 核；`TP` 升 `HEAVY` 后自动触发 `R4` 并按默认 `app_to_irq` 折叠至甜点 ②，稳定维持 `~19.7G` + `~25 µs` |
| **`S1-merge` 3 核 `AUTO` 容量内自动合并** | 3 核池先跑限速 **`~10 Gbps`** 的 `TP1`（占 2 核），再以无标签 `AUTO` 启动：① 32 B `LAT`；② 限速 **`~10 Gbps`** 的 `TP2`（两者之和 `~20 Gbps ≤ C_bps_cap 22 Gbps`） | ① `AUTO=LAT` 零迁移命中甜点 ②；② `AUTO=TP2` 安全起步零干扰 `TP1`，升 `HEAVY` 后经 `R1` 自动将 `TP2 IRQ` 合并至 `TP1 IRQ` 核（`S1` 容量校验通过） |
| **`S1-cap-degrade` 3 核 `AUTO` 超容保护** | 3 核池先跑不限速 **`~20.8 Gbps`** 的 `TP1`，再以 `AUTO` 启动不限速 `TP2`（合计超过 `C_bps_cap = 22 Gbps`） | 调度器核对目标核合计负载超限，**不执行超容合并**，按降级契约置 `DEGRADED` 并记录超容原因，保护 `TP1` 不被拖垮 |
| **`S2` 4 核单核占位变载解救与释放展开** | 4 核池：`t = 0` 时背景占位应用 `BG` 通过显式同核固定（`ETRAN_APP_CPU=10`、`ETRAN_LATENCY_CPU=10`，模拟外部单核占用）仅占 **`CPU 10` 1 核**，使剩余空闲池恰好为 **3 核**；随后注册 `LAT`（2 核）与节流 `1 Gbps` 的 `TP`（1 核 + IRQ 暂借 `LAT IRQ` 核）；`t = T1` 时 `TP` 阶跃至 `20 Gbps`；`t = T2` 时 `BG` 退出释放 `CPU 10` | `T1` 后记录 `T_detect`（阶跃目标 `~100 ms`）、`T_actuate`、`T_recover`（目标 `≤ 100 ms`），`LAT` 维持甜点水平；`T2` 后检测到空闲核自动无损展开为 4 核全分离（`LAT p99` 回到 `~20 µs`），零震荡 |
| **`S3` 多 worker 单 `TID` 精准迁核与不合作回滚** | `epoll_server -t 2 -q 2` 验证 `alloc_tctx()` 初始精准分核及双槽位 `APPOUT_TCP_WORKER_TID` 登记；迁移 `Worker 0`；另测屏蔽自绑不合作用例（多队列同应用折叠保持关闭） | `Worker 0` 迁核成功且 `Worker 1` 核位与吞吐零受扰；不合作超时经 `ROLLING_BACK` 准确回滚预留或隔离核、零 TOCTOU |
| **`S4` 双属性高 QPS 负载与 3 核无解降级** | 64 B 16 连接高并发（或 `FlexKVS`）作为 `LATENCY` 高 QPS 负载与 10 MB `TP` 在 4 核 / 3 核混跑 | 4 核下识别为 `(LATENCY, HEAVY)` 不误折叠；3 核无解时保高优 `LAT`、显式将 `TP` 置 `DEGRADED`；据此评估二期 `libetran` 用户态指标需求 |
| **`S5` 同底座横向对照** | **同一 `6.6.0-eTran-compat3.3` 内核与同一 `ETRAN_CPU_POOL`** 下横向对比：① `ETRAN_SCHED_MODE=off`（动态关闭）；② 手工最优静态布局（Oracle 上界）；③ `ETRAN_SCHED_MODE=active`（本方案）；辅以早期 `eTran+` 历史对照 | 量化对比 `LAT p99/p99.9`、`TP` 有效吞吐、核利用率与 `poll_tcp_cc_to()` 抖动，双端有效核位全量归档 |
