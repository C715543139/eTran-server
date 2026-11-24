# eTran 队列与 CPU 解耦设计

`compat3.1` 按源队列发送和回收 AF_XDP 时间轮帧，并在解除绑定前排空该 pool 的软件队列与 DMA 发送环。阶段 A 与阶段 B 用例 0 在双机 20 核环境下，沿用相同的队列布局和单跑、混跑口径进行基线对照；用例 1、2 为单条 10 MB 吞吐流，分别使用 20 通道和 10 通道；用例 3 使用两条 10 MB 吞吐流，验证独占 QID 共用 IRQ 核及应用退出后重绑；24 核验收使用 `compat3.1`。双机保留 `compat2` 的 20 核启动项，跨核和退出验收通过后再切换 24 核。实验通过 `ETRAN_CPU_POOL` 手动框选 CPU，驱动与控制面保持 NUMA 中立。[阶段顺序与容量设计](01-baseline-cleanup.md)

## 1. 设计目标与支持范围

1. TCP 的 IRQ 核可与源网卡队列号不同，IRQ 核超过已启用通道数时也能出包。
2. 多条独占硬件队列（`Q_EXCLUSIVE`）可共用同一颗 IRQ 核，各队列的 QID、发送环和 completion ring 各自独立。本轮验收使用预先设置的静态 IRQ 亲和性，运行期在线迁核留作后续阶段验证。
3. 跨核提交、硬件发送环满和关闭 pool 都有明确的所有权移交。最后一个绑定 socket 返回后，同一 QID 可以立即绑定新 pool。
4. 普通 XDP 重定向、AF_XDP 直接发送和 `XDP_GEN` 维持各自的发送路径。Homa 跨核与共享队列（`Q_SHARED`）的跨应用 IRQ 所有权仍需单独改造并验收：
   - **`Q_SHARED` 边界说明**：当前分配器可将同一共享 QID 分配给多个应用，而 `bind_rss_core()` 在每次应用注册时都会重新设置该 QID 的 IRQ 亲和性；若后注册应用选到不同 CPU，早先注册应用登记的 IRQ 核便会与实际亲和性失步。后续若纳入 `Q_SHARED`，须定义队列级 IRQ 所有权——新消费者继承该队列当前生效的 IRQ 放置，队列迁移时统一更新所有关联应用的登记记录（见 [control_plane.cc](../../eTran/micro_kernel/control_plane.cc) 中的 `try_allocate_queues()` 与 `bind_rss_core()`）。

---

## 2. 现状源码级根因与耦合点剖析

### 2.1 compat2 的 `mlx5` 单通道（`mlx5e_channel c[q]`）的三条 XDP 发送队列分工

在 `eTran-linux` 的 `mlx5` 驱动（`drivers/net/ethernet/mellanox/mlx5/core/en.h`、`en_main.c`、`en_txrx.c`）中，每个网卡队列 `q` 对应一个通道实例 `priv->channels.c[q]`，拥有独立的 MSI-X 完成中断（`mlx5_comp<q+1>`）和唯一的 `c[q]->napi`。其内部包含三条相互独立的 XDP 发送队列，且三条队列的 CQ 均**仅在 `c[q]->napi`（`mlx5e_napi_poll`）中被轮询清理**：

| 通道内发送队列 | 初始化 `sq->xsk_pool` | 触发入口与承载流量 | 完成清理上下文 |
| :--- | :--- | :--- | :--- |
| `c[q]->rq_xdpsq` | `NULL` | 收包 `mlx5e_xdp_handle()` 直接返回 `XDP_TX` 原地回弹 | `c[q]->napi` |
| `c[q]->xsksq` | `pool_q`（绑定 XSK 时创建） | `mlx5e_xsk_tx()`：AF_XDP 待发包经 `XDP_EGRESS` 直接返回 `XDP_TX`（旁路时间轮） | `c[q]->napi` |
| **`c[q]->xdpsq`** | **`NULL`**（通道开启时常驻创建） | `ndo_xdp_xmit` → **`mlx5e_xdp_xmit()`**：承载 **eBPF Pacing 时间轮出包（`tw_cb → bpf_packet_send`）** 与 **`XDP_GEN` 聚合 ACK 出包** | `c[q]->napi` |

### 2.2 卡点一（直接死锁根因）：`mlx5e_xdp_xmit()` 跨核触发 `XSK_NO_COMP` 漏槽

1. **前置预留契约（`net/xdp/xsk.c:752`）**：`xsk_tx_peek_desc()` 从用户态 TX Ring 取出描述符后，先调 `xskq_prod_reserve_addr(pool->cq, desc->addr)` 在容量为 2048 的 Completion Ring 中预留 1 个槽位（`cached_prod++`）。由于 `XDP_EGRESS` 与时间轮会改变实际出包顺序，所有未标记 `NO_COMP` 的帧在硬件发送完成后必须调用：
   - `xsk_ooo_cq(pool->cq, desc_addr)`：按实际完成顺序写入 `ring->desc[ooo_comp_prod++ & mask]`；
   - `xsk_tx_completed(pool, n)`：推进 `ring->producer += n` 向用户态发布完成事件。
2. **驱动层的错位实现（`mlx5/core/en/xdp.c:677–891`）**：
   - 发送侧（805–810 行）按当前执行 CPU 选发送队列：`sq_num = smp_processor_id(); sq = &priv->channels.c[sq_num]->xdpsq;`（例如 `Q18` 的中断绑到 `CPU 16` 后，`tw_cb()` 在 `CPU 16` 上运行，选定了 `c[16]->xdpsq`）。
   - 完成侧 `mlx5e_free_xdpsq_desc()`（682 行）与 `mlx5e_poll_xdpsq_cq()`（748 行）却硬编码写死了 `sq->channel->xsksq.xsk_pool`（即 **`c[16]` 绑定的 `pool_16`**，而非数据包自身所属的 `xdpf->pool` 即 `pool_18`）。若 `Q16` 未绑定 AF_XDP，`c[16]->xsksq.xsk_pool` 为 `NULL` 将直接导致内核空指针崩溃；若 `Q16` 绑定了其他应用，则会写坏对方的 Completion Ring。
   - 原作者为规避该崩溃，在 880–883 行加入了降级分支：一旦 `xdpf->pool->queue_id != sq_num`，强制压入 `MLX5E_XDP_XMIT_MODE_XSK_NO_COMP` 跳过回填。这导致 `pool_18->cq` 槽位只预留不归还，满载大流在约 **1.9 秒**耗尽全部 2048 个预留槽位后，`xsk_tx_peek_desc()` 永久返回 `false`，吞吐归零死锁。

### 2.3 卡点二（架构级并发与隔离陷阱）：保留 `sq_num = smp_processor_id()` 的三大次生问题

若仅在 `xdpi_fifo` 中保存 `xdpf->pool` 修复回填指针，而继续沿用 `sq_num = smp_processor_id()` 选择 `c[sq_num]->xdpsq`，会引发三项严重问题：

1. **队列数小于核号时越界报错（`-ENXIO`）**：`xdp.c:807` 检查 `if (sq_num >= priv->channels.num) return -ENXIO;`。一旦网卡开启队列数小于目标核号（例如配置 `-q 10`、`channels.num = 10`，将队列绑到 `CPU 12/14/16/18`），绑定在高编号核上的时间轮调用 `mlx5e_xdp_xmit()` 将 100% 返回 `-ENXIO` 失败。
2. **跨核硬件中断污染（破坏性能隔离）**：设吞吐队列 `Q18` 绑定至 `CPU 16`，时延队列 `Q16` 绑定至 `CPU 12`。若 `CPU 16` 上的时间轮将 `Q18` 的 20 Gbps 数据包投入 `c[16]->xdpsq`，硬件 DMA 完成后触发的是**通道 16 的 MSI-X 中断（落在 `CPU 12`）**，导致大流发送完成中断持续轰击时延应用的专属核 `CPU 12`。
3. **无锁 `pool->cq` 跨核数据竞争**：上例中，`c[18]->napi`（在 `CPU 16` 上执行 `xsksq` 完成与 `xsk_tx_peek_desc`）与 `c[16]->napi`（在 `CPU 12` 上执行 `c[16]->xdpsq` 完成）会从两颗物理核并发修改同一个 `pool_18->cq` 的非原子字段 `ooo_comp_prod++` 与 `ring->producer += n`（`net/xdp/xsk_queue.h:406, 452`），破坏单生产者无锁环语义，迫使 `net/xdp/` 热路径引入跨核自旋锁。

### 2.4 卡点三：XSK 关闭生命周期竞争与满环背压漏槽

1. **XSK 关闭瞬间残留 WQE 风险**：`mlx5e_close_xsk()`（`en/xsk/setup.c:175`）关闭 AF_XDP socket 时仅关闭并清零 `c->xsksq`，而 `c->xdpsq` 保持常驻开启；且 `mlx5e_free_xdpsq_descs()`（`xdp.c:786`）误用了恒为 `NULL` 的 `sq->xsk_pool`。若应用退出瞬间 `c->xdpsq` 尚有来自时间轮的在途 WQE 完成，必须防止访问正在销毁的 `pool->cq`。
2. **发送环满背压漏槽**：当 `mlx5e_xdp_xmit()` 因硬件 SQ 满未能发出批次尾部帧（`sent < to_send`）时，`bq_xmit_all()`（`kernel/bpf/devmap.c:400–404`）对 `XDP_FLAGS_XSK_QUEUEING` 帧直接静默丢弃，未归还其在 `pool->cq` 中预留的槽位。

### 2.5 卡点四：控制面 RSS 权重与 eBPF 协议层核查

1. **RSS 上下文硬编码队列数（`eTran/micro_kernel/control_plane.cc:698–720`）**：`create_rss_context()` 固定按编译期 `MAX_NIC_QUEUES` 循环生成 `ethtool -X ... weight` 参数串，当运行期开启队列数 `_num_queues != MAX_NIC_QUEUES`（如 `-q 10`）时会导致 `ethtool -X` 执行失败。
2. **eBPF 层核查结论**：
   - TCP 数据面（`eBPF/tcp/main.c`、`pacing.h`、`tcp.h`）无任何 `cpu == rx_queue_index` 断言；Per-CPU 时间轮 `tw_outer_map[cpu]` 按运行核索引，天然支持绑在同一核上的多条队列共享。
   - `XDP_GEN`（`net/xdp/xsk.c:1461` 与 `include/net/xdp.h:312`）在初始化时已将 `xgd->rxq.queue_index` 设为 `pool->queue_id`，转换出的 `xdp_frame` 天然携带正确的 `xdpf->queue_index == pool->queue_id`。
   - **Homa 跨核改造条件（待独立实现与验收）**：Homa 控制面在注册端口时以入口 `nic_qid` 作为 `xsk_map_idx` 下标写入（见 [homa.cc](../../eTran/micro_kernel/homa.cc)），而数据面在直接收包和 `CPUMAP` 路径中仍以 `current_cpu` 索引 `xsk_map_idx[current_cpu]`，并在两处检查 `current_cpu != ctx->rx_queue_index`（见 [eBPF/homa/main.c](../../eTran/micro_kernel/eBPF/homa/main.c)）。后续若启用 Homa 跨核，须在直接收包和 `CPUMAP` 路径中保留真实入口 QID 并据此查找 XSK，同步调整两处 `CPU == QID` 相等检查（保留 `current_cpu >= MAX_CPU` 索引边界检查），并专项验收 `QID != current_cpu` 的收包路径。


---

## 3. 发送、背压与完成回收

| 路径 | `compat3.1` 选队规则 | 完成与回收 |
| --- | --- | --- |
| 时间轮中的 `XDP_FLAGS_XSK_QUEUEING` | `frame->pool->queue_id` → `c[qid]->xsk_pace_sq` | 同一通道的 NAPI 使用该 SQ 的 `xsk_pool` 回收预留项。专用 SQ 随该 pool 同步关闭 |
| AF_XDP 直接发包 | `c[qid]->xsksq` | 同一 NAPI 完成，保持直接发包的 ring 和填充模式 |
| `XDP_GEN` | 由 `XDP_FLAGS_FROM_GEN` 标识来源，QID 取自 `frame->queue_index`（有效时选 `c[qid]->xdpsq`，无效时回退 `smp_processor_id() % channels`） | 普通 XDP frame 的 DMA 映射与回收 |
| 普通 XDP 重定向 | 按 `smp_processor_id() % channels` 散列到有效普通 `xdpsq` | 普通 XDP frame 的 DMA 映射与回收 |

`mlx5e_xdp_xmit()` 先检查目标设备、QID、通道状态和活动 pool。持有 SQ 的生产者锁后再核对 pool 是否进入关闭态，然后才能使用 `dma_pages`。单次批次可含多个源队列帧，每个连续的 SQ 批段独立封包并敲响 doorbell；来自不同 CPU 的生产者通过同一把 `sq->xmit_lock` 串行提交。

- **满环与拒收后缀归属回收**：`ndo_xdp_xmit()` 返回成功接管的连续前缀长度；遇到硬件 SQ 满或帧拒收时，由 `devmap`（`bq_xmit_all()`）处理余下后缀。被拒收后缀可能混合多个 QID 的帧：普通 XDP 帧走原回收路径；未标记 `NO_COMP` 的 `XSK_QUEUEING` 帧通过 `xsk_pacing_discard()` 将预留描述符地址放入各归属 `pool` 的暂存表（容量等于该 `pool->cq` 槽位数，无需硬件 SQ 预留 NOP 空位），唤醒对应通道 NAPI 由 `xsk_pacing_complete()` 串行回填并发布各自的 `pool->cq`。
- **完成发布与统计口径**：在线运行期由对应通道的 NAPI 发布该 `pool` 的 CQ；关闭专用 `xsk_pace_sq` 时，`mlx5` 先同步 NAPI、销毁硬件 SQ，再通过 `mlx5e_free_xdpsq_descs()` 清理残留 WQE。注意 **CQ 发布仅代表描述符槽位可被重用**，实际网络发包与客户端收到的完整业务应答应分别独立观测。对应实现见 [0003-decouple-xdp-queue-cpu-and-quiesce-xsk.patch](../../kernel-patches/0003-decouple-xdp-queue-cpu-and-quiesce-xsk.patch) 中的 `mlx5e_get_xdp_xmit_sq()`、`bq_xmit_all()`、`xsk_pacing_complete()` 和 `mlx5e_close_xdpsq()`。

## 4. 退出时序：区分单 `pool` 解绑与整套 BPF 卸载

### 4.1 单个 AF_XDP `pool` 解绑与时间轮排空

1. 最后一个绑定 socket 在 `rtnl` 下解除设备绑定：先设置 `pacing_stopped`，阻断该 `pool` 新的时间轮入队和发送提交，并通过第一次 `synchronize_net()` 等待已进入 NAPI 或 `NET_TX` 读侧临界区的执行流退出。
2. `pkt_queue_purge_pool()` 遍历仍存活的 `BPF_MAP_TYPE_PKT_QUEUE`，在桶锁内仅摘除目标 `pool` 的帧并同步扣减队列计数，**同核上其他活跃 `pool` 的帧继续正常排队与使用时间轮**。清理完成后执行第二次 `synchronize_net()`（`553a8e9` 补齐），等待清理遍历期间刚被定时器取出的帧回调彻底离开（即使此时网卡已关闭或 XDP 已分离）。
3. `mlx5` 清除通道的 XSK 活动状态并同步 NAPI，随后销毁 XSK RX、直接 TX（`xsksq`）和 paced TX（`xsk_pace_sq`）硬件队列并清理已提交 WQE；常驻普通 `xdpsq` 始终不承载零拷贝 UMEM DMA。仅在硬件队列彻底退出后才调用 `xp_dma_unmap()` 释放 DMA 页表。
4. 驱动与 `netdev` 的 QID 关联在最后一个绑定 socket 的 `close()` 路径内同步解除，ring、UMEM 和 `pool` 对象释放交由异步工作队列完成，保证返回用户态后同一 QID 可立即重绑新 `pool`。
5. TCP pacing（[pacing.h](../../eTran/micro_kernel/eBPF/tcp/pacing.h)）通过 `pkt_queue_pending()` 查询内核实时计数（入队、出队、清池统一维护同一计数）；回调遇到发送失败或 `dynptr` 异常时调用 `bpf_packet_drop()` 归还槽位并照常 flush 已接管批次，队列排空后立即停止重调度，空轮首次入队时重置时间游标。RSS 权重按实际启用通道数生成，CPU 拓扑与核池约束见 [01-baseline-cleanup.md](01-baseline-cleanup.md)。

### 4.2 整套 BPF 卸载时的定时器生命周期

需严格区分“保留微内核与 BPF Map 时的单个 `pool` 退出”与“微内核退出/卸载整套 BPF 资源”两条路径：

- **compat3.1 的源码风险**。 `BPF_F_TIMER_IMMEDIATE` 会将定时器内的 `&t->net_timer` 挂入每 CPU 的 `softnet_data.bpf_timer`，由 `NET_TX_SOFTIRQ` 异步取走。旧的 `bpf_timer_cancel_and_free()` 在取消 `hrtimer` 后直接 `kfree(t)`。如果 `tw_trigger_map` 卸载时还有待执行或正在执行的回调，存在释放后访问风险。阶段 B 排空验收保留了微内核和 BPF map，覆盖的是 pool 退出后的时间轮停转。

`compat3.2` 在释放网络定时器时执行以下步骤。普通 BPF hrtimer 保留既有清理路径。见 [0004 内核补丁](../../kernel-patches/0004-quiesce-nettx-timers-on-bpf-unload.patch)。

1. 在定时器锁内摘除 BPF map 元素中的定时器指针并清空回调入口，阻断之后的 helper 调度。再同步取消只负责入队的 hrtimer，确保它退出后不会再次把对象交给 `NET_TX`。
2. 原子匹配并清除每颗可能 CPU 的 `NET_TX` 槽位，在 pacer 锁下清除匹配的待执行对象。撤销只操作正在释放的定时器。两个 dispatcher 从取出指针到结束调用都处于 RCU 读侧临界区。
3. 通过 `call_rcu()` 等待已取出对象的读侧临界区退出，然后释放程序引用和定时器对象。回调所属程序引用保持其 map 存活。这使 map 的清理可以从回调自身发起，无需在回调里同步等待自身退出。见 [Linux RCU 文档](https://docs.kernel.org/RCU/whatisRCU.html)。

---

## 5. 内核版本、启动项与成套回退规范

双机切换严格按 **先单次启动验证 `108-52`、通过后再切换 `108-51`** 的顺序执行；两台机器各自使用本机工具生成 `initramfs` 并构建 `knem`、`MFT` 外部模块，`108-51` 额外在本机重编 `VirtualBox` 模块并执行重启前 `finalrd` 检查（详见 [双机安装规范](../fixs/cpu-registration-fix.md)）。

| 内核版本 | GRUB 菜单项 ID | 启动参数特征 | 已验证用途 |
| :--- | :--- | :--- | :--- |
| `6.6.0-eTran-compat2` | `etran-experiment` | `nosmt nr_cpus=20 panic=30` | 20 核阶段 A 清理基线及随时回退对照 |
| `6.6.0-eTran-compat3` | `etran-compat3-20` | `nosmt nr_cpus=20 panic=30` | 初版驱动解耦与生命周期补丁的 20 核对照 |
| `6.6.0-eTran-compat3.1` | `etran-compat3-1-20` | `nosmt nr_cpus=20 panic=30` | 最终补丁（`553a8e9` 补齐清池后二次网络同步）的 20 核基线复验、跨核与重绑验收 |
| `6.6.0-eTran-compat3.1` | `etran-compat3-1-24` | `nosmt panic=30`（解除 `nr_cpus=20`） | 24 核容量与高核号数据面验收，现为 24 核回退项 |
| **`6.6.0-eTran-compat3.2`** | **`etran-compat3-2-24`** | **`nosmt panic=30`** | **BPF 整栈卸载的 TCP 回归通过后设为双端默认启动项** |

由于补丁同时涉及 `mlx5`、`AF_XDP`、`BPF_MAP_TYPE_PKT_QUEUE` 与新增 kfunc，部署与回退时必须将 **内核、模块、eTran 用户态二进制/库与 pacing eBPF 对象**按对应版本成套切换；若回退至 `compat2`，须配套切回阶段 A（`182dd51`）的构建产物。

---

## 6. 验收矩阵、实测结论与观测边界

### 6.1 20 核必测用例定义

| 用例 | 双端布局 | 验收指标 |
| :--- | :--- | :--- |
| **0** | `Q8 → CPU8`、`Q6 → CPU6`，TP `APP0`、LAT `APP2` | 沿用阶段 A 队列布局复跑单跑与混跑，核对完整应答有效吞吐与 499,000 样本 RTT 分位数 |
| **1** | `-q20 -Q0`，单条 10 MB 流，`Q18 → CPU16`，TP `APP0` | 连续发流 ≥ 30 秒；按关探针窗口统计有效吞吐，短采样核验时间轮执行核、CQ 在途水位和完成推进 |
| **2** | `-q10 -Q0`，单条 10 MB 流，`Q8 → CPU16`，TP `APP0` | 验证动态队列数 RSS 注册成功，低通道数下高编号 IRQ 核可持续满载收发 |
| **3** | 两条 10 MB 流，`Q8`、`Q6 → CPU16`，`APP0`、`APP2` | 验证两个独占 QID 共用同一 IRQ 核独立推进；满载下强退 `Q6` 并立即同 QID 重绑 20 次，停止后验证 `NET_TX` 停转与内核日志零告警 |

### 6.2 `compat3.1` 双机实测结果汇总

以下有效吞吐按客户端收到的完整应用应答计算。用例 3 使用包含跟踪和重绑的全段记录，其余吞吐取关探针窗口。

| 验证阶段与场景 | 双端队列与核位布局 | 实测关键结果 |
| :--- | :--- | :--- |
| **20 核 · 用例 0（基线复验）** | `Q8 → CPU8`（TP `APP0`）、`Q6 → CPU6`（LAT `APP2`） | TP 单跑 **`19.84 Gbps`**（`0/14` 零窗口）；LAT 单跑 p99 / p99.9 为 **`14.51 / 17.71 µs`**；分核混跑 TP **`19.65 Gbps`**（`0/5` 零窗口）、LAT p99 / p99.9 为 **`19.84 / 24.66 µs`** |
| **20 核 · 用例 1（跨核满载）** | 20 通道，`Q18 → CPU16`，TP `APP0` | 32 秒关探针窗口有效吞吐 **`20.16 Gbps`**，31 个完整秒窗口均有应答；双端观测到 `CPU16` 推进 NAPI 与 CQ，`52` 短采样捕获 `bpf_packet_send()` 入口 |
| **20 核 · 用例 2（低通道高核）** | 10 通道，`Q8 → CPU16`，TP `APP0` | 有效吞吐 **`19.86 Gbps`**，14 个完整秒窗口均有应答，RSS 注册与高核号跨核运行通过 |
| **20 核 · 用例 3（共核与 20 轮重绑）** | `Q8`、`Q6 → CPU16`，两条 10 MB 流 | `Q6` 满载强退后同 QID 立即重绑 **20/20 轮全部成功**（每轮首个统计窗完成 69–76 次 10 MB 应答）；`Q8` 全段（含跟踪与重绑过程）有效吞吐 **`13.68 Gbps`**，55 个完整秒窗口均有应答 |
| **20 核 · 用例 3（停流空轮核验）** | 停止两条流后保留微内核与 BPF Map | 随后 2 秒内双端 `CPU16` 的 `NET_TX` 软中断计数增量均为 **`0`**，`dmesg` 无新增 warning 或错误 |
| **24 核 · 高核号验收（阶段 D）** | `compat3.1` 24 核（控制面 `CPU23`），`-q20` 兼容与 `-q24 -Q0` 高核对齐 / 交叉 | `-q20` 混跑 **`19.79 Gbps`**（LAT `19.88 / 30.08 µs`）；`Q22→CPU22, Q20→CPU20` 对齐混跑 **`20.23 Gbps`**（LAT `19.96 / 25.33 µs`）；`Q22→CPU20, Q20→CPU22` 交叉混跑 **`19.95 Gbps`**（LAT `20.08 / 27.80 µs`），全部 `0/6` 零窗口 |

### 6.3 CQ 水位口径与探针覆盖边界说明

1. **区分 CQ 总占用与未发布预留项**：在 `xsk_tx_completed()` 入口采样时，`cached_prod - consumer` 为 **CQ 总占用**（包含内核已发布但用户态尚未消费的完成项，用例 1 中该值曾触及 `2048`）；而 `cached_prod - producer` 才是驱动侧**已预留未发布项**（`52` 实测峰值为 **`366`**，逐 100 ms 窗口最大值的中位数为 **`128`**，未发生耗尽累积）。汇报时须分别报告这两项指标，并将硬件 SQ 水位单独标注。
2. **分支级观测边界**：本轮探针已覆盖 `bpf_packet_send()` 入口、NAPI、CQ 推进及停流后的 `NET_TX` 静默；硬件 SQ 满触发拒收后经 `xsk_pacing_discard()` 暂存回填的逐分支路径，以及双流共核下强退一条非空时间轮流时的逐帧摘除计数，尚未做专门故障注入观测。后续若需专项验收这两条慢分支，应分别打点记录“SQ 拒收 → 补偿暂存 → 原 `pool` CQ 发布”以及“退出时刻目标 `pool` 待发帧/在途 WQE 清理且同核另一 `pool` 持续推进”。
3. **原始实验归档位置**：本地详细日志与原始 JSON 归档位于 `experiments/20261124-queue-core-decoupling/`（该目录已被 `.gitignore` 忽略，关键结论与数值以本文档为准），包含 `compat31-stage-b/results.json`、`compat31-stage-b/cross-30s/completion.json`、`compat31-stage-b/same-irq-rebind/idle-with-maps.json`、`compat31-stage-d/results.json` 以及 `51-compat3.1-default-24.json`、`52-compat3.1-default-24.json`。

### 6.4 `compat3.2` 整栈卸载与重启回归

51 和 52 均通过 24 核启动检查。实验沿用同一套 eTran 用户态和 BPF 产物，双端使用 24 通道、单条 10 MB TCP 流、`Q22 → CPU20`、`TP APP0`。

| 场景 | 实测观察 |
| --- | --- |
| 发流时请求退出 | 双端在应用仍运行时送出 SIGINT。微内核正常退出，原有 TCP 和 Homa timer map ID 均消失，XDP 已分离 |
| 排空后退出 | 先停止两端应用。保留微内核和 BPF map 的 2 秒内，双端 CPU20 的 `NET_TX` 计数均无新增。随后正常退出并确认 timer map ID 消失 |
| 重新启动并发流 | 客户端关探针窗口内有效吞吐 20.59 Gbps，14 个完整秒窗口均有应答 |

复采时，52 在两种退出场景分别记录到 7,584 和 7,632 次调度与分派逐一匹配，均观察到一次定时器撤销调用和一次 RCU 回收。撤销入口未捕获已调度但尚未分派的回调，本轮 TCP 流也没有进入 pacer 队列。仍在排队时的撤销和 pacer 队列撤销留待独立验证。

各用例内核日志均无新增告警。两端均恢复 24 通道、`irqbalance` active 和双向连通。一次性启动验收后，双端已将 GRUB 的常规启动项设为 `etran-compat3-2-24`，保留 `etran-compat3-1-24` 和 `etran-experiment` 作为回退项。相关原始记录在 `experiments/20261124-queue-core-decoupling/` 下的 `compat32-runtime-regression` 和 `compat32-lifetime`。
