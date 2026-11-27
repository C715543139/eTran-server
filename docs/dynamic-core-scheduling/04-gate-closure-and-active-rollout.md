# eTran 动态核池调度 04 版推进方案：G0/G2 门禁彻底闭环与 `active` 全场景放行

本文档承接 [01-metrics-and-scheduling-design.md](01-metrics-and-scheduling-design.md)、[02-g0-calibration-and-adjustments.md](02-g0-calibration-and-adjustments.md) 与 [03-batch3-scheduler-implementation-plan.md](03-batch3-scheduler-implementation-plan.md)，基于分支 `codex/dynamic-core-scheduling@22962ad` 的 Batch 3 实测与源码核查（本地归档于 `experiments/20261126-dynamic-scheduler/README.md`），制定 **`G0` 64 B 小包开销压降**、**`G2-cc-jitter` 全量异步准备与回收** 两项最终门禁的彻底闭环方案，以及门禁通过后 **`ETRAN_SCHED_MODE=active` 接线与 `S0–S5` 完整验收规范**。

---

## 1. Batch 3（`22962ad`）门禁核定与双项闭环决定

### 1.1 现状总览

| 门禁项 | `22962ad` 实测结果 | 裁决 |
| :--- | :--- | :--- |
| **`G0` 32 B LAT & 无探针画像** | 6 轮交替 `p99`：基线 `16.210` / `off` `16.203` / `counters` **`16.101 µs`**（`< 0.5 µs`）；BPF 核验 `off` 0 引用、`counters` `1/2` 引用；`ETRAN_SOFTIRQ_PROBE=0` 下 `light-1g (0.944G)`、`light-2g (1.808G)`、`multi-q8-q6` 100% 准确 | **通过** |
| **`G1-coop-check`** | 6/6 worker 按序号自绑确认，`276–441 µs`（快 10×）；收缩过渡窗 `p99 = 23.32 µs`，最大 RTT 中位数 `76.87 µs`（极值 `90.89 µs`），`> 100 µs = 0`；折叠稳态 `p99 = 24.76–24.79 µs`（3/3 双窗 `≤ 25 µs`） | **通过** |
| **`G2-shadow` / `admit-4c` / `admit-3c-init` / `infeasible`** | 只读零误报；4 核全分离（`19.47–19.61G / 19.73–19.75 µs`）；3 核 TP 先到直落甜点 ②（`19.32G / 28.82 µs`）；无解 `reject` 双端精确返 `-ENOSPC (-28)`（既有 LAT 变动 `-0.23% / 0.00%`），`degrade` 标 `DEGRADED` 保 LAT 分核 | **通过** |
| **`G3-quiesce`** | 双端各 20 轮（共 40/40）迁移中 `SIGKILL` + 立即同 `QID` 重绑均命中 `CANCELLED_QUIESCENT` 并恢复 `Q0@CPU8`，内核告警 0；APP 暂停注入验证超时 `ROLLING_BACK` 回源核及持续暂停 `QUARANTINED`；实流 `Q6` `CPU6→CPU2→CPU6` 均确认 `aff_handoff_done +1`（`15.13 / 15.19 ms`） | **通过** |
| **`G0-short`（64 B 16 连接）** | 3 轮交替中位数：基线 `1.19035` / `off` `1.19502` / `counters` **`1.18091 Mops/s`**（相对基线 `-0.79%`，相对同版 `off` **`-1.18%`**，超 `< 1%` 门槛 `0.18%`） | **待闭环（Track A）** |
| **`G2-cc-jitter`（并发注册/回收 CC 间隔）** | RSS 异步后，活跃/完整周期 `poll_tcp_cc_to()` `p50 = 255 µs`、`p99 = 423–1,217 µs`、**`max = 509–568 ms`**；定位到控制线程同步执行 `bp_init()`（`468–480 ms`）、控制 `xsk`（`34–44 ms`）、卸载回收（`115–141 ms`）与 `ethtool -U` 流规则删改（`3.9–21.7 ms`） | **待闭环（Track B）** |

### 1.2 工程决策（不放宽门槛、不做局部凑合）

在开启 `ETRAN_SCHED_MODE=active` 之前，**严格走完善路线同时闭环 Track A 与 Track B**：
1. **Track A（`G0-short`）**：不放宽“相对同版 `off` `< 1%`”门槛，通过消除跨 Socket 采样缓存行抖动、按需读 `tw_fd` / `bpf_cc` 与精简热路径 BPF 指令，将 64 B 退化稳定压至 **双基准（`baseline` 与 `off`）均严格 `< 1%`**，同时维持 LAT `p99 < 0.5 µs`；
2. **Track B（`G2-cc-jitter`）**：走完整异步路线（方案 B），将 `bp_init()`、控制/数据 `XSK` 建毁、`RSS` 增删与 `ethtool -U` Flow Director 增删**全部移出 `control_loop()`**，并补齐 NUMA0 内存首次触碰隔离与活跃流 `CcJitter` 观测口径；
3. **两项门禁闭环并回归 `G3-quiesce` 后**，立即接线 `ETRAN_SCHED_MODE=active` 并执行全场景矩阵 `S0–S5`。

---

## 2. 闭环 Track A：`G0-short` 64 B 小包开销压降至 `< 1%`

### 2.1 实证根因拆解

在 `2.59 Mpps`（单核 `99.6%` 饱和、`386 ns/pkt`）下，`-1.18%` 仅相当于 **`~4.5 ns/pkt`**，距离 `< 1%` 安全区（目标 `-0.5% ~ -0.8%`）仅需收回 **`~1.5–2.5 ns/pkt`**。源码核查与 Batch 3 消融（`batch3-g0-reader-ablation.json`、`bpf-check-counters.json`、`microkernel.log`）直接指向两处可量化开销：

1. **读侧跨 Socket + 全核缓存行嗅探（已由 reader 消融证实）**：
   - **NUMA1 采样核跨 Socket 抢行**：Batch 3 日志显示 `sched_start ... cpu=1`（`ETRAN_SCHED_CPU=1` 为奇数核 **NUMA1**），而数据池 `0,2,6,8` 与控制核 `14` 全在 **NUMA0**。采样线程每 `50 ms` 跨 UPI 拉取 NUMA0 热缓存行（`.bss.qid_napi_stats` 与 `bpf_cc`），造成高频 `M → S → M` 跨片失效；
   - **无 pacing 仍全机嗅探 24 个 per-CPU 槽**：[scheduler.cc#L429](../../eTran/micro_kernel/scheduler.cc#L429) 每 `50 ms` 无条件调用 `bpf_map_lookup_elem(tw_fd, &qid, per_cpu.data())`，即使 64 B 走 `BYPASS_RL`（`tw_enq_attempts == 0`），内核仍循环遍历全部 24 核（含满载 IRQ 核与 APP 核）拷贝 `PERCPU_ARRAY`；
   - **全表 65,536 槽扫描 + 高速进展下跨核抢读 `bpf_cc`**：[scheduler.cc#L207-L225](../../eTran/micro_kernel/scheduler.cc#L207-L225) 每 `50 ms` 扫描 `MAX_TCP_FLOWS = 65536` 个原子槽（`512 KB` L3 流量），并跨核读取 16 条活跃连接的 `bpf_cc` 热缓存行（每包写 `cnt_rx_ack_bytes / txp`），即使本周期该队列已完成 ~13 万包正常进展；
   - **全表系统调用拷贝 `.bss`**：[scheduler.cc#L396](../../eTran/micro_kernel/scheduler.cc#L396) 每 `50 ms` 通过 `bpf_map_lookup_elem(napi_fd, ...)` 拷贝全部 32 槽（`2 KB`），而非仅读已注册活跃 `qid`。
2. **写侧热路径冗余指令（已由 `bpf-check-counters.json` 证实）**：
   - [eTran/micro_kernel/eBPF/tcp/main.c#L254-L260](../../eTran/micro_kernel/eBPF/tcp/main.c#L254-L260) 中，`xdp_egress_prog` 执行到该处已必返回 `XDP_TX`，却仍保留冗余 `ret == XDP_TX` 判断，且两次从栈重载 `ctx` 并重新计算 `ctx->data_end - ctx->data`；
   - [eTran/micro_kernel/eBPF/tcp/main.c#L378-L386](../../eTran/micro_kernel/eBPF/tcp/main.c#L378-L386) 中，`xdp_sock_prog` 在已判定 `qid < MAX_NIC_QUEUES` 后跨 `bpf_redirect_map` 再次执行 `barrier_var(qid)` 与范围检查，增加寄存器栈溢出与加载指令。

### 2.2 分层优化改造（不弱化任何遥测语义）

1. **采样与管理辅助核迁至 NUMA0 空闲物理核**：
   - 双机（`nosmt`，偶数核为 NUMA0）实验中，将 `ETRAN_SCHED_CPU` 从 `1` 改为 **NUMA0 空闲核（如 `CPU 12`）**，避开池核 `0,2,6,8(,10)` 与控制核 `14`，彻底消除采样跨 Socket 缓存行失效；微内核代码继续保持 NUMA 中立。
2. **读侧按需采样（[scheduler.cc](../../eTran/micro_kernel/scheduler.cc)）**：
   - **`.bss.qid_napi_stats` 只读 `mmap`**：在 [trans_ebpf.cc#L237](../../eTran/micro_kernel/trans_ebpf.cc#L237) 对 `.bss.qid_napi_stats`（`.bss` 在 Linux 6.6 默认 `BPF_F_MMAPABLE`）建立 `PROT_READ, MAP_SHARED` 映射（失败时回退 `bpf_map_lookup_elem`），采样线程只读已注册活跃 `qid` 的对应 64 B 缓存行；
   - **按需查 `tw_fd`**：仅在队列首拍基线、`current.tw_enq_attempts != state.napi.tw_enq_attempts`、`state.paced == true` 或 `have_pending && pending[app.irq_cpu] != 0` 时调用 `bpf_map_lookup_elem(tw_fd, &qid, ...)`；其余周期直接沿用上一拍 `state.wheel`，彻底消除无 pacing 流量下的 24-CPU 嗅探；
   - **活跃 `cc_idx` 压缩列表 + 高速进展免读 `bpf_cc`**：由 `scheduler_register_flow` / `scheduler_unregister_flow` 维护紧凑活跃流索引（或仅扫描已登记的最大高水位活跃集合），取代 65,536 槽全表扫描；且当某队列本周期已明确满足高进展（`state.bps >= limits.b_high || state.pps >= limits.p_high`）且该队列**无在途 `IRQ` 迁移**时，跳过读取该队列下连接的 `bpf_cc`（`inflight / stalled` 仅用于防止低流量停滞队列被误判为 `IDLE/LIGHT` 或静默迁移误确认，高进展 `HEAVY` 队列跳过读取零语义损失）；
   - **按需读 `/proc/softirqs`**：仅当存在任一队列处于低流量/待判 `IDLE` 或待确认静默迁移时解析 `/proc/softirqs`。
3. **写侧 BPF 热路径精简（[eTran/micro_kernel/eBPF/tcp/main.c](../../eTran/micro_kernel/eBPF/tcp/main.c)）**：
   - `xdp_egress_prog`（`#L254-L261`）：去掉冗余 `ret == XDP_TX` 检查，改用已持有的 `data` / `data_end`（或局部 `pkt_len`）避免二次栈重载 `ctx`；
   - `xdp_sock_prog`（`#L378-L386`）：精简 `bpf_redirect_map` 后的重复 `qid` 边界判定（利用 `MAX_NIC_QUEUES == 32` 为 2 的幂使用 `qid & (MAX_NIC_QUEUES - 1)` 配合前置边界判断），减少栈溢出与指令数；
   - 加载后继续通过 `bpftool prog dump xlated` 核验：`off` 仍严格保持 **`0` 引用**，`counters` 热路径指令数较 `22962ad` 下降。
4. **分层消融与保底预案**：
   - 优化后先跑 1 组 64 B `off / write_only / counters`（NUMA0 `ETRAN_SCHED_CPU=12`）核对写侧与读侧各自占比；
   - 若仅靠上述 1–3 项已使 `counters` 相对 `off` 与 `baseline` 均稳定在 `> -0.9%`，则不引入额外改动；仅若 `write_only` 自身仍逼近 `-1%`，再进一步评估在 `bpf_qid_napi_stats` 中精简 64 B 热路径写操作（例如仅对 RX/TX 采用单字段紧致更新或校验 `tx_direct_bytes` 复用已有长度），并重测确认不损 `bps / pps` 精度。

### 2.3 Track A 验收标准

- **64 B 16 连接 3 轮交替（`baseline / off / counters`）**：`counters` 有效完成量中位数相对 `baseline` **与**相对同版 `off` **双双严格 `> -1.0%`**；
- **32 B LAT 6 轮交替（`baseline / off / counters`）**：`counters` `p99` 相对 `baseline` 与同版 `off` 均保持 **`< 0.5 µs`** 门槛内；
- **无探针画像回归**：`ETRAN_SOFTIRQ_PROBE=0` 下 `light-1g / light-2g / multi-q8-q6` 仍 100% 准确。

---

## 3. 闭环 Track B：`G2-cc-jitter` 全量异步准备与回收（方案 B）

### 3.1 `22962ad` 实测阻塞源完整清单

根据 `batch3-admission-live-1795699653/host51/4c-tp-first/microkernel.log` 与 [control_plane.cc](../../eTran/micro_kernel/control_plane.cc) 逐行核查，除已异步化的 `ethtool -X` RSS 外，控制线程 `control_loop()` 中剩余的全部慢操作共有 **4 类**：

| 阶段 | 代码位置 | 实测耗时 | 阻塞根因 |
| :--- | :--- | ---: | :--- |
| 注册 `buffer_pool` | [control_plane.cc#L1956](../../eTran/micro_kernel/control_plane.cc#L1956) → [xsk_buffer_pool.cc#L32](../../eTran/common/xskbp/xsk_buffer_pool.cc#L32) | **`467.8–480 ms`** | `shm_open` + 512 MB `mmap(MAP_POPULATE)` + `memset(0)` + `xsk_umem__create` |
| 注册 `control_xsk` | [control_plane.cc#L1983](../../eTran/micro_kernel/control_plane.cc#L1983) → [ebpf.cc#L44](../../eTran/micro_kernel/ebpf.cc#L44) | **`34–44 ms`** | `xsk_socket__create_shared()` 绑定 UMEM 与 NIC 队列 |
| 建连/回收 `ethtool -U` | [control_plane.cc#L348-L413](../../eTran/micro_kernel/control_plane.cc#L348-L413) (`record_port` / `unrecord_port`) | **`3.9–21.7 ms`** | 同步 `popen("ethtool -U ...")` 增删 Flow Director 规则 |
| 注销 `socket_cleanup` | [control_plane.cc#L1498-L1528](../../eTran/micro_kernel/control_plane.cc#L1498-L1528) | **`115–142 ms`** | `xsk_delete_socket()` + `bp_free()`（512 MB `munmap`/`shm_unlink`）+ `lrpc/evfd` 回收 |

> **若漏掉 `ethtool -U`，`max_us` 仍会停在 `20+ ms`**。因此本批将以上 4 类阻塞操作**一次性全部下沉**到统一管理线程 `mgmt_thread`（即扩展现有 `rss_thread`），使 `control_loop()` 彻底零阻塞系统调用。

### 3.2 单线程所有权契约与 `mgmt_thread` 四类作业

保持 **`control_loop()` 独占全部核心对象与状态机**、**`mgmt_thread` 单线程 FIFO 顺序执行纯值拷贝作业** 的零共享可变状态架构：

- **`control_loop()` 独占**：`active_apps`、`app_ctx` 内存、非阻塞 `app_fd` 状态机、`etran_nic->_nic_queues[]` 挂载与摘除、`xsk_epfd`、`BPF maps`（`xsks_map / slow_path_map / umem_id_tran_map`）、`LRPC`、`tcp_connections / tcp_listeners`、`sport_to_actx / dport_to_actx`、QID/CPU/Port 账本与 `poll_tcp_cc_to()`。
- **`mgmt_thread` 独占（单 FIFO 队列天然保序）**：全部 `libxdp` 建毁（独占 TCP 路径无多线程并发调 `libxdp`）、`bp_init / bp_free`、`shm_wrapper_destroy`、`ethtool -X`、`ethtool -U`、`set_queue_cpu` 与 `ETHTOOL_GSTATS`。

#### 1) 作业 1：`PrepJob`（独占 TCP 异步准备）

1. **`control_loop()` 同步前置（`< 0.5 ms`）**：
   - 校验参数、保留 XSK map keys 校验、借出独占 `qids[]`、执行准入 `bind_rss_core(..., true)` 保留 `usedCores`、初始化轻量 `LRPC` 通道（或同步放入 `PrepJob` 均可），置 `app->uds_state = UdsState::WaitMgmt`，提交值拷贝 `PrepJob{app_gen, pid, proto, qids[], irq_cpus[], nr_app_threads, socket_busy_poll}`。
2. **`mgmt_thread` 异步执行（全成或全撤）**：
   - 调用 `bp_init(&res.bpw, pid, SHM_BP_PREFIX, SHM_UMEM_PREFIX)`；
   - 对每个 `qid` 调用不触碰 `etran_nic->_nic_queues` 的独立接口 `xsk_configure_socket_for_queue(&res.bpw, qid, proto)`（仅读取不可变的 `etran_tcp->_ebpf` 与局部 `res.bpw`），并通过 `getsockopt(fd, SOL_XDP, XDP_OPTIONS, ...)` 读取 `res.umem_id`；
   - 创建全部 `nr_app_threads * nr_nic_queues` 个数据 `AF_XDP` socket（`socket()` + `SO_BINDTODEVICE` + 可选 `apply_socket_busy_poll`）；
   - 执行 `set_queue_cpu(qid, cpu)` 与 `create_tcp_rss_context(qids)`；
   - **全撤保障**：任一步失败时，在 `mgmt_thread` 内逆序销毁已创建的 RSS / 数据 XSK / 控制 XSK / `bpw`，返回 `PrepResult{error != 0}`，绝不向 `control_loop()` 移交半成品。
3. **`control_loop()` 回包发布（`< 0.5 ms`）**：
   - 若 `result.error != 0`：清理 `LRPC`（或移交 `TeardownJob`）、调用 `return_resources(*app)`；若 `app->fd >= 0` 则发 `REG_FAIL(result.error)`，否则直接 `erase_app(app)`；
   - 若 `result.error == 0 && app->fd < 0`（等待期间客户端断开）：**绝不挂载 BPF map / `xsk_epfd` / `_nic_queues`**，直接将 `result` 中已建资源打包提交 `TeardownJob`（保持 QID/CPU 预留直至 `TeardownResult` 完成）；
   - 若 `result.error == 0 && app->fd >= 0`：
     - 先值拷贝 `actx->bpw = result.bpw;`、`actx->umem_id = result.umem_id;`、`actx->rss_ctx_id = result.rss_ctx_id;`；
     - **指针稳定性约束**：**必须先完成 `actx->bpw` 赋值**，再调用 `thread_bcache_create(&actx->bpw, &actx->iobuffer)` 与挂载 `nicq.bpw = &actx->bpw; nicq.xsk_info = result.ctrl_xsks[i];`（确保 `thread_bcache` 与 `nic_queue_info` 永不持有指向临时 `PrepResult` 的悬垂指针）；
     - 挂载 `xsk_epfd`、更新 `umem_id_tran_map / xsks_map / slow_path_map`、置 `actx->resources_ready = true`、调用 `finish_registration(*actx)` 并切入 `UdsState::SendXsk`。

#### 2) 作业 2 & 3：`AddFlowRuleJob` / `DeleteFlowRulesJob`（异步 `ethtool -U` Flow Director）

- **`AddFlowRuleJob`**（[tcp.cc#L1302](../../eTran/micro_kernel/tcp.cc#L1302) `tcp_open` 与 [#L1393](../../eTran/micro_kernel/tcp.cc#L1393) `tcp_bind`）：
  - 若 `dport_to_actx` / `sport_to_actx` 尚未登记该端口，先在 `control_loop()` 占坑表项（防止重复投递同端口规则），提交 `AddFlowRuleJob{app_gen, tctx_idx, conn_ptr_or_token, fd, is_bind, local_port, remote_port, rss_ctx_id}`；
  - 回包时按 `app_gen` 与连接有效性校验：有效则记录 `flow_director_rules` 并完成 `reg_tcp_conn_slowpath + notify_app_tcp_status_bind`（`bind`）或将连接推入 `tcp_handshake_list`（`open`）；若等待期间应用已退出或连接关闭，立即对新返回的 `rule_id` 投递 `DeleteFlowRulesJob` 防规则泄漏；若规则添加失败，释放占坑并返回失败通知给应用。
- **`DeleteFlowRulesJob`**（活跃应用单连接关闭时的 `unrecord_port`）：
  - `control_loop()` 立即摘除 `sport_to_actx / dport_to_actx` 中的映射并收集待删 `rule_ids` 与 `local_port`，**暂不调用 `free_port()`**；
  - `mgmt_thread` 执行完 `ethtool -U delete` 回包后，`control_loop()` 再调用 `free_port(local_port)`，杜绝端口在网卡规则未删完前被复用的竞态。

#### 3) 作业 4：`TeardownJob`（异步全量回收与注销排空栅栏）

1. **`control_loop()` 同步摘除（`< 0.5 ms`）**：
   - 调用 `migration_executor.cancel(*app)`、清空 `irq_owner_tokens[qid]`、`scheduler_unregister_queue / workers`；
   - 调用 `free_tcp_resources(actx)` 释放内存连接与监听，**收集全部 `flow_director_rules` ID 与占用 `ports`（不调用同步 `ethtool -U`）**；
   - 从 `xsk_epfd` 摘除控制 XSK、`unregister_xsk_map`、`unregister_slow_path_map`、`bpf_map_delete_elem(umem_id_tran_map)`，将 `etran_nic->_nic_queues[qid].xsk_info = nullptr; bpw = nullptr;` 并移出 `actxs`（此后 `control_loop()` 永不再解引用该应用的 `xsk_info / bpw / lrpc`）；
   - 将 `app` 置为 `UdsState::WaitTeardown`，**继续保留 QID、CPU、Port 与 `migration_executor` 隔离预留**，值移交 `{app_gen, rule_ids, ports, rss_ctx_id, ctrl_xsks[], txrx_xsk_fds[], bpw, lrpcs[], evfds[]}` 到 `TeardownJob`。
2. **`mgmt_thread` 严格保序执行**：
   - ① 若存在同 `app_gen` 更早排队的 `IrqWork` / `AddFlowRuleJob`，因单 FIFO 队列已在其前完成（若回包生成了迟到 `rule_id`，由控制线程即时补删，或在 `TeardownJob` 前已被 FIFO 排空）；
   - ② **先删全部 `rule_ids`（`ethtool -U delete`），再删 `rss_ctx_id`（`ethtool -X delete context`）**（严格满足网卡驱动“规则依赖 RSS context”的删除顺序）；
   - ③ 依次执行 `xsk_delete_socket(ctrl_xsk)`、`close(txrx_xsk_fd)`、`bp_free(&bpw)`、`shm_wrapper_destroy(lrpc)`、`close(evfd)`。
3. **`control_loop()` 完结释放**：
   - 收到 `TeardownResult` 后断言 `!irq_action || irq_action->app != app`；
   - 若 RSS 删除失败则维持 CPU/QID 隔离不泄漏；成功则依次 `free_port()`、`return_resources(*app)`（含 `migration_executor.release(*app)`）、`erase_app(app)`，并立即触发 `poll_pending_queues()` 唤醒等待中的后继注册。
4. **`WaitQueue (-EAGAIN)` 补全与 `SIGINT` 优雅退出**：
   - 在 `alloc_app_resources()` 中，当 `try_allocate_queues` **或** `bind_rss_core / place_new_tcp_app` 遇到 `-ENOSPC` 时，若存在处于 `WaitTeardown`（或 `fd < 0` 待回收）的独占 TCP 应用，返回 `-EAGAIN` 进入 `UdsState::WaitQueue`（超时 `2 s`），保障 `G3-quiesce` 强杀后毫秒级同 `QID` 重绑零误拒；
   - `destroy_all_apps()` 必须先让 `mgmt_thread` 排空并完成已入队的回收作业（释放全部 UMEM/RSS/规则），再 `stop_mgmt_worker()` 并执行 `ebpf_exit()`。

### 3.3 关键防线：NUMA0 首次触碰隔离（严防 512 MB UMEM 漂移到 NUMA1）

源码核查确认 [shm_wrapper.cc#L40](../../eTran/common/shm/shm_wrapper.cc#L40) 使用 `mmap(..., MAP_SHARED | MAP_POPULATE)` 且 [xsk_buffer_pool.cc#L102](../../eTran/common/xskbp/xsk_buffer_pool.cc#L102) 执行 512 MB `memset(..., 0)` 触发首次触碰页分配。原方案因 `bp_init()` 跑在 `ETRAN_CP_CPU=14`（NUMA0）才自然落于 NUMA0；若移入管理线程而沿用 `ETRAN_SCHED_CPU=1`（NUMA1），全部 131,072 页 UMEM 将漂移至 NUMA1 导致远端 DMA 劣化。

- **新增可选变量 `ETRAN_MGMT_CPU`**（[control_plane.cc#L2606](../../eTran/micro_kernel/control_plane.cc#L2606)）：未设置时回退到 `ETRAN_SCHED_CPU`（再回退 `topology.control`），校验在线且不在 `pool` 与 `control` 内，保持代码 NUMA 中立；
- **NUMA0 实验标准核位分配（双机 24 核 `nosmt`，偶数核为 NUMA0）**：
  - 数据池 `ETRAN_CPU_POOL`：`0,2,6,8`（4 核）或 `0,6,8`（3 核）或 `0,2,6,8,10`（含占位 `BG`）
  - 控制面 `ETRAN_CP_CPU=14`
  - 调度采样 `ETRAN_SCHED_CPU=12`（NUMA0 独立核，零跨 Socket 嗅探）
  - 异步管理 `ETRAN_MGMT_CPU=16`（NUMA0 独立核，既保证 UMEM 首次触碰 100% 落 NUMA0，又避免 `470 ms` `bp_init` `memset` 延迟 `50 ms` 采样线程）
  - 启动脚本辅以 `numactl --membind=0` 兜底，并在验收脚本中核对 `/proc/<mk_pid>/numa_maps`（`UMEM_` 131,072 页、`BufferPool_`、`LRPC_` **100% 位于 NUMA0**）。

### 3.4 `CcJitter` 活跃流观测口径修正

[control_plane.cc#L2554](../../eTran/micro_kernel/control_plane.cc#L2554) 在无活跃 TCP 连接（`next_tcp_cc_to_tsc == UINT64_MAX`）时本就会休眠 `TICK_US = 1000 µs`，而在活跃流期才按 `CC_INTERVAL_US = 200 µs` 节拍运行。因此将 `CcJitter` 拆为两组独立统计输出：
- **`cc_jitter_active`**：仅在 `next_tcp_cc_to_tsc != UINT64_MAX` 时统计相邻调用间隔（空载→活跃首拍重置 `previous = now`，不把空载 `1 ms` 算作抖动），作为 `G2-cc-jitter` 主判据；
- **`cc_jitter_all`**：保留原全周期统计作辅佐对照。

### 3.5 Track B 验收标准

1. **并发注册/建连/迁移/回收实测**：保持 10 MB `TP` 活跃流持续运行，期间并发触发第 2 个应用完整注册、建连（触发 `ethtool -U`）、单步迁移与退出回收（3 轮）：
   - 活跃流 `cc_jitter_active`：`max_us` 从 `509–568 ms` 压降至 **`< 5 ms`**（目标 **`< 1–2 ms`**），`p50 ~ 200–260 µs`，`p99 < 500 µs`；
   - `cc_jitter_source` 不再出现 `buffer_pool / control_xsk / flow_cleanup / socket_cleanup` 阻塞告警；
2. **NUMA0 内存零漂移核验**：双端 `numa_maps` 核对 `UMEM_ / BufferPool_ / LRPC_` **100% 落于 NUMA0**；
3. **`G3-quiesce` 全量回归**：双端各 20 轮迁移中 `SIGKILL` + 立即同 `QID` 重绑全部 20/20 `CANCELLED_QUIESCENT`、恢复 `Q0@CPU8`、账本与内核告警零异常。

---

## 4. 门禁闭环后立即放行：`ETRAN_SCHED_MODE=active` 接线与 `S0–S5` 验收

Track A 与 Track B 验收全部通过后，立即完成 `active` 模式接线并执行完整场景矩阵 `S0–S5`。

### 4.1 `ETRAN_SCHED_MODE=active` 精准接线清单（基于 `22962ad` 差量）

`22962ad` 已具备全部观测状态机、二维准入与 `MigrationExecutor` 双端执行器，`active` 模式只需补齐以下 5 处接线：

1. **模式与开关打通（[scheduler.cc](../../eTran/micro_kernel/scheduler.cc)、[control_plane.cc](../../eTran/micro_kernel/control_plane.cc)）**：
   - `ETRAN_SCHED_MODE=active` 同时开启 `qid_enabled=1`、`scheduler_admission=1`、`scheduler_active=1`、`resp.sched_cooperative=1`、`os_start_time` 记录与 `irq_owner_tokens` 维护；
   - **修复槽位追踪条件**：[scheduler.cc#L639](../../eTran/micro_kernel/scheduler.cc#L639) 与 [#L653](../../eTran/micro_kernel/scheduler.cc#L653) 原先仅在 `settings().shadow` 时维护 `workers` 槽位列表，必须扩展为 `settings().shadow || settings().active`；
   - 新增开关：`ETRAN_FOLD_MODE=app_to_irq|irq_to_app`（默认 `app_to_irq`），`ETRAN_SCHED_CAP_BPS`（默认 `22e9`），`ETRAN_SCHED_CAP_PPS`（默认 `1.6e6`）。
2. **单槽自动动作队列与冷却契约**：
   - `scheduler.cc` 在 `active` 下每 `50 ms` 周期基于全池快照（队列画像 + worker 槽位 + CPU 引用计数 + 隔离核掩码）最多产出 1 条合法动作（`WorkerMigrationRequest` 或 `IrqMigrationRequest`），通过线程安全单槽投递给 `control_loop()`；
   - `control_loop()` 在全局无在途动作、非退避期（`T_backoff = 1000 ms`）且目标实体不在成功冷却期（`T_cooldown = 200 ms`）时提交 `MigrationExecutor` 执行，执行前复核 `app_gen / os_start_time / quarantine_refs` 防止陈旧决策。
3. **红线解救动作精确分派（`R1–R4` 2 拍确认 = `100 ms`，严格区分两种折叠形态）**：
   - **`R4`（`HEAVY` `TP/AUTO IRQ` 与 `LIGHT/IDLE` `LAT IRQ` 同核，对应 `S0` 与 `S2@T1`）**：
     - 若存在可用空核：优先将 `HEAVY TP IRQ` 迁至空核；
     - 若无空核且同核 `LAT` 满足 `Q=1, T=1`、`LIGHT/IDLE` 且其 `LAT APP` 核独占（`cpuUseRef == 1`）：**一步将 `LAT IRQ` 迁至 `lat_app_cpu`**，单步直达甜点 ②（避免把 `LAT APP` 迁入含 `HEAVY TP IRQ` 的核产生瞬时 `R1`）；
     - 若 `LAT` 自身独享 2 核且因新应用准入/容量收缩需要主动腾出 1 核：按 **`ETRAN_FOLD_MODE`** 执行（默认 `app_to_irq` 迁 `LAT APP → lat_irq_cpu`；对照 `irq_to_app` 迁 `LAT IRQ → lat_app_cpu`）。
   - **`R1`（`HEAVY` `AUTO/TP IRQ` 与 `APP` 同核，对应 `S1-merge` 与 `S1-cap-degrade`）**：
     - 有可用空核：将 `IRQ` 迁至空核；
     - 无空核：检查 **`S1` 容量校验合并**——寻找仅承载非 `LATENCY` `IRQ` 的目标核（`cpuIrqRefs[dst] > 0 && cpuUseRef[dst] == cpuIrqRefs[dst] && cpuLatUseRef[dst] == 0 && !quarantine_refs[dst]`），核算 `sum_bps(dst) + my_bps <= C_bps_cap` 且 `sum_pps(dst) + my_pps <= C_pps_cap`（若探针开启亦需 `sum_irq(dst) + my_irq <= 0.85`）；
       - 通过：执行 `IRQ` 合并（同步在 [control_plane.cc#L996](../../eTran/micro_kernel/control_plane.cc#L996) `MigrationExecutor::start(IrqMigrationRequest)` 中放行此合法 `S1` 共享目标核）；
       - 超容或无合法目标：**禁止超容合并**，仅在状态切换时输出一次 `sched_decision mode=active state=DEGRADED reason=irq_cap_exceeded` 并维持当前放置保护既有 `TP1`。
   - **`R2`（`LIGHT IRQ` 与 `THROUGHPUT APP` 同核）**：迁 `LIGHT IRQ` 至空核或自身 `Q=1, T=1` 非 `THROUGHPUT` `APP` 核；
   - **`R3`（多忙轮询 `APP` 同核）**：优先迁出 `LATENCY` worker（否则迁出次优 worker）至空核。
   - **多队列 / 多 worker 安全护栏（对应 `S3`）**：`S2` 同应用自折叠严格门控于 `nr_nic_queues == 1 && nr_app_threads == 1`；未登记 `os_tid` 的 worker 一律暂缓迁移。
4. **空闲核机会扩容（对应 `S2@T2`，防震荡滞后）**：
   - 当池中存在可用空闲核且连续 **`T_expand_hold = 500 ms`（10 拍）** 无任何红线与在途迁移时，按如下优先级执行无损扩容：
     1. 解除 `DEGRADED`（将自折叠的 `TP/AUTO IRQ` 迁至空核，清 `DEGRADED`）；
     2. 展开甜点 ② 自共置 `LAT`（迁 `LAT APP` 至空核，复用 `~300 µs` 合作式自绑，恢复 4 核全分离）；
     3. 拆分 `S1` 合并的 `HEAVY TP IRQ`（将其中一条 `IRQ` 迁至空核）。

### 4.2 `S0–S5` 全场景验收矩阵

所有场景统一在双机 `6.6.0-eTran-compat3.3`、24 通道、`ETRAN_SCHED_MODE=active`、`ETRAN_QID_STATS=1`、`ETRAN_SOFTIRQ_PROBE=0`、`ETRAN_FOLD_MODE=app_to_irq`、`ETRAN_CP_CPU=14`、`ETRAN_SCHED_CPU=12`、`ETRAN_MGMT_CPU=16`、`numactl --membind=0` 下执行；双端全程采集有效核位、`cc_jitter_active`、与分阶段耗时 **`T_detect` / `T_actuate` / `T_recover`**：

| 场景 | 池配置与精确编排 | 预期动作与核心验收指标 |
| :--- | :--- | :--- |
| **`S0` 3 核先 `LAT` 后 `TP` 自动折叠** | 3 核池 `0,6,8`：先启 32 B `LAT`（2 核），再启 10 MB 满载 `TP` | 初始 `TP IRQ` 暂借 `LAT IRQ` 核；`TP` 升 `HEAVY` 后经 `R4` 一步将 `LAT IRQ` 迁至 `LAT APP` 核落入甜点 ②，稳态维持 `~19.5–19.7G` + `~25–29 µs`；记录 `T_detect / T_actuate / T_recover` |
| **`S1-merge` 3 核 `AUTO` 容量内自动合并** | 3 核池 `0,6,8`：先跑限速 `~10 Gbps` `TP1`（2 核），再以无标签 `AUTO` 启动：① 32 B `LAT`；② 限速 `~10 Gbps` `TP2`（合计 `~20G ≤ 22G`） | ① `AUTO=LAT` 零迁移命中甜点 ②；② `AUTO=TP2` 自折叠安全起步，升 `HEAVY` 后经 `R1 + S1` 容量校验自动将 `TP2 IRQ` 合并至 `TP1 IRQ` 核，总吞吐无塌陷 |
| **`S1-cap-degrade` 3 核 `AUTO` 超容保护** | 3 核池 `0,6,8`：先跑不限速 `~20.8 Gbps` `TP1`，再以 `AUTO` 启不限速 `TP2`（合计超 `22G`） | 命中容量上限，**禁止超容合并**，输出 `DEGRADED (reason=irq_cap_exceeded)`，保护既有 `TP1` 吞吐不被拖垮 |
| **`S2` 4 核单核 `BG` 占位变载解救与释放展开** | 4 核池 `0,6,8,10`：`t=0` `BG` 显式单核占位（`ETRAN_APP_CPU=10, ETRAN_LATENCY_CPU=10`）仅占 `CPU 10`；随后启 `LAT`（2 核）与限速 `1 Gbps` `TP`（1 核 + IRQ 暂借 `LAT IRQ`）；`t=T1` `TP` 阶跃至 `20 Gbps`；`t=T2` `BG` 退出释放 `CPU 10` | `T1` 后 `T_detect ~100 ms`，自动解救并维持 `LAT` 甜点水平（`T_recover ≤ 100 ms`）；`T2` 后经 `500 ms` 稳态防抖自动无损展开为 4 核全分离（`LAT p99` 回到 `~20 µs`），零来回震荡 |
| **`S3` 多 worker 单 `TID` 精准迁核与不合作回滚** | `epoll_server -t 2 -q 2`：验证双槽位初始分核与全槽位 `TID` 登记；触发 `Worker 0` 迁核；另跑屏蔽自绑不合作用例（多队列自折叠关闭） | `Worker 0` 精准迁核且 `Worker 1` 核位与吞吐零受扰；不合作超时经 `ROLLING_BACK` 确认回源或精确 `QUARANTINED`，零 TOCTOU |
| **`S4` 双属性高 QPS 负载与 3 核无解降级** | 64 B 16 连接高并发（`LATENCY` 高 QPS，`~2.58 Mpps`）与 10 MB `TP` 在 4 核 / 3 核混跑 | 4 核下判为 `(LATENCY, HEAVY)` 零误折叠；3 核无解下 `reject` 返 `-ENOSPC`、`degrade` 保 `LAT` 分核并将 `TP` 置 `DEGRADED`；据此形成是否需要二期 `libetran` 用户态指标的实证结论 |
| **`S5` 同底座横向对照** | **同一 `6.6.0-eTran-compat3.3` 内核与同一池**下横向对照：① `ETRAN_SCHED_MODE=off`；② 手工最优静态布局（Oracle 上界）；③ `ETRAN_SCHED_MODE=active`（本方案）；附早期 `eTran+` 历史对照 | 统一汇报 `LAT p50/p99/p99.9`、`TP` 有效吞吐、核利用率与 `cc_jitter_active`，双端有效核位与 `numa_maps` 全量归档 |

---

## 5. 执行顺序与交付检查表

1. **Step 1（Track A：`G0-short` 闭环）**：落地 `scheduler.cc` 读侧优化 + `main.c` 热路径精简 + NUMA0 `ETRAN_SCHED_CPU=12` → 跑 `off / write_only / counters` 消融 → 跑 3 轮 64 B 与 6 轮 LAT 复验 + `bpftool` 核验 → 确认双基准 `< 1%` 与 LAT `< 0.5 µs`；
2. **Step 2（Track B：`G2-cc-jitter` 闭环）**：落地 `PrepJob / AddFlowRuleJob / DeleteFlowRulesJob / TeardownJob` + `ETRAN_MGMT_CPU=16` + `cc_jitter_active` + `WaitQueue (-EAGAIN)` → 实测活跃流并发注册/建连/迁移/回收 `cc_jitter_active` 与 `numa_maps` 100% NUMA0 → 回归 `G3-quiesce` 双端各 20 轮；
3. **Step 3（`active` 放行与 `S0–S5`）**：接线 `ETRAN_SCHED_MODE=active`（含 `workers` 槽位修复、`R4` 一步解救、`S1` 放行、500 ms 防抖扩容）→ 跑通 `S0 / S1-merge / S1-cap-degrade / S2 / S3 / S4 / S5` → 归档完整实验报告并恢复双机至干净态（24 通道、`irqbalance=active`、XDP 分离、规则归零、内核告警 0）。
