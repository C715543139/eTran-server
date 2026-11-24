# eTran 队列与 CPU 解耦设计

compat3 按源队列发送和回收 AF_XDP 时间轮帧，并在解除绑定前排空该 pool 的软件队列与 DMA 发送环。阶段 A 和 B 使用相同的 20 核拓扑、队列与流量口径。双机保留 compat2 的 20 核启动项，跨核和退出验收通过后再切换 24 核。实验通过 `ETRAN_CPU_POOL` 手动框选 CPU。驱动与控制面保持 NUMA 中立。[阶段顺序与容量设计](01-baseline-cleanup.md)

## 1. 设计目标

1. TCP 的 IRQ 核可与源网卡队列号不同，IRQ 核超过已启用通道数时也能出包。
2. 多条独占队列可共用 IRQ 核，各自保有独立的发送环和 completion ring。
3. 跨核提交、硬件发送环满和关闭 pool 都有明确的所有权移交。最后一个绑定 socket 返回后，同一 QID 可以立即绑定新 pool。
4. 普通 XDP 重定向、AF_XDP 直接发送和 XDP_GEN 维持各自的发送路径。Homa 跨核仍需单独改造并验收。

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
   - Homa 协议（`eBPF/homa/main.c:326, 484`）存在两处 `current_cpu != ctx->rx_queue_index` 丢包检查，若后续测试启用 Homa 跨核，须同步移除这两处检查。


---

## 3. 发送与完成回收

| 路径 | compat3 选队规则 | 完成与回收 |
| --- | --- | --- |
| 时间轮中的 `XDP_FLAGS_XSK_QUEUEING` | `frame->pool->queue_id` → `c[qid]->xsk_pace_sq` | 同一通道的 NAPI 使用该 SQ 的 `xsk_pool` 回收保留项。专用 SQ 随该 pool 关闭 |
| AF_XDP 直接发包 | `c[qid]->xsksq` | 同一 NAPI 完成，保持直接发包的 ring 和填充模式 |
| XDP_GEN | 专用标志携带生成队列号 → `c[qid]->xdpsq` | 普通 XDP frame 的 DMA 映射与回收 |
| 普通 XDP 重定向 | 按当前 CPU 散列到有效普通 XDP SQ | 普通 XDP frame 的 DMA 映射与回收 |

`mlx5e_xdp_xmit()` 先检查目标设备、QID、通道状态和活动 pool。持有 SQ 的生产者锁后再核对 pool 是否进入关闭态，然后才能使用 `dma_pages`。批次可含多个源队列，每个连续的 SQ 批段独立敲 doorbell。来自不同 CPU 的生产者通过同一 SQ 锁串行提交。各 pool 的 CQ 仍由各自通道 NAPI 串行产出。

`ndo_xdp_xmit` 返回已接管的连续前缀。当 SQ 满或帧拒收时，devmap 处理余下后缀。普通 XDP 帧走原回收路径。需要归还 completion 的 AF_XDP 帧将描述符地址交给该 pool 的 NAPI，由 NAPI 回填并发布 CQ。暂存表容量等于该 CQ 的槽位数，每项对应一个已预留槽。该路径无需 SQ 留出 NOP 空位。已经标记 `NO_COMP` 的帧未持有 completion 预留项。

## 4. 退出时序与 pacing

1. 最后一个绑定 socket 在 `rtnl` 下解除设备绑定。先设置 `pacing_stopped`，阻断新的时间轮入队和发送提交。已进入 NAPI 或 NET_TX 的代码由 `synchronize_net()` 等待退出。
2. 遍历仍存活的 `BPF_MAP_TYPE_PKT_QUEUE`，在桶锁内摘除目标 pool 的帧并同步更新计数。同核上其他 pool 的帧继续排队。清理后再次调用 `synchronize_net()`，等待清理期间已取到该 pool 帧的回调离开，包括网卡已关闭或 XDP 已分离的路径。处于关闭阶段的 pool 由整池清理回收。
3. mlx5 清除通道的 XSK 活动状态并同步 NAPI，再销毁 XSK RX、直接 TX 和 paced TX 的硬件队列。专用 `xsk_pace_sq` 关闭时清理已提交的 WQE。普通 `xdpsq` 始终不承载该 pool 的零拷贝 DMA。只有硬件退出后才执行 `xp_dma_unmap()`，释放本 pool 的 DMA 页表。
4. 驱动与 netdev 的 QID 关联在最后一个绑定 socket 的 close 路径内解除。ring、UMEM 和 pool 的最终释放可由原有工作队列处理。旧 socket 和工作队列保有各自的引用，下一次绑定可使用已释放的 QID。
5. TCP pacing 以 `pkt_queue_pending()` 查询内核的实时计数。在成功入队、出队和清池时修改同一个计数。定时器回调处理发送失败和 dynptr 异常时归还已取出的帧，照常 flush 已接管的批次。只要队列排空，后续回调就停止立即重调度。空轮首次入队重新设置时间游标。

RSS 权重按实际启用的通道数生成。阶段 C 的 CPU 拓扑改造从 sysfs 读取真实 CPU ID，用可用核池约束 TCP 应用和 IRQ 的注册放置。Homa 现有 `CPU == QID` 检查仍保留，本阶段的高核号数据面验收使用 TCP。[动态容量配置](01-baseline-cleanup.md)

## 5. 内核构建、安装和回退

以两机 compat2 对应的内核源码为基线，构建 `6.6.0-eTran-compat3.1` 的内核映像、模块和外部模块所需头文件。初版 compat3 用于 20 核对照，compat3.1 补齐清理后的同步。补丁同时更新 mlx5、AF_XDP、BPF 包队列和 kfunc，因此 eTran 用户态、内核与 pacing eBPF 对象配套部署。compat2 回退后使用阶段 A 的整套产物和 eBPF 对象。[内核补丁](../../kernel-patches/0003-decouple-xdp-queue-cpu-and-quiesce-xsk.patch)

52 和 51 分别用本机工具构建 initramfs 及 knem、MFT。51 还需本机编译 VirtualBox 模块，并在重启前生成和核验 finalrd。更新 GRUB 时保留已有 compat2 项，追加带 `nosmt nr_cpus=20 panic=30` 的 compat3 项。先通过单次启动切换并验收 52，再切换 51。每台核对版本、online CPU、驱动模块、BPF 加载、系统服务与双网连通。20 核通过后再追加并验证 compat3 的 24 核项。[双机安装检查](../fixs/cpu-registration-fix.md)

## 6. 20 核验收

| 用例 | 双端布局 | 验收指标 |
| --- | --- | --- |
| 0 | Q8 → CPU8、Q6 → CPU6，TP APP0、LAT APP2 | 重跑阶段 A 单跑和混跑，记录完整应答吞吐和 RTT |
| 1 | `-q20 -Q0`，Q18 → CPU16，TP APP0 | 连续发流至少 30 秒。按未跟踪窗口统计吞吐，按短采样核验时间轮执行核、CQ 在途水位和完成推进 |
| 2 | `-q10 -Q0`，Q8 → CPU16，TP APP0 | RSS 注册成功，低通道数下高 IRQ 核可持续收发 |
| 3 | Q8、Q6 → CPU16，APP0、APP2 | 两个活动 pool 分别推进。满载下循环退出与立即重绑 20 次，核对 QID、CQ、BPF/NAPI 活动与内核日志 |

各用例正式发流前核对实际 QID、IRQ 生效亲和性和应用核位。用例 3 同时观察活跃 pool 的吞吐与另一 pool 退出后的 NET_TX 活动，验证回收范围和定时器判空。跨核通过后再进入 24 核阶段。[试验记录](../../experiments/20261124-queue-core-decoupling/README.md)
