# eTran 内核补丁与配置片段索引

本目录保存在独立的 `eTran-linux` 内核源码树（基线提交 `045b25f1edeb276a17876961762e6e3f27747c54`）之上增量应用的补丁序列与配置片段。双机（`108-51` / `108-52`）当前默认生产实验内核为 **`6.6.0-eTran-compat3.3`**（GRUB 菜单项 `etran-compat3-3-24`，启动参数 `nosmt panic=30`）。

## 1. 补丁应用顺序与职责对照

所有补丁须按文件名前缀编号顺序依次应用：

| 序号与补丁文件 | 引入版本 | 解决的核心问题 | 对应设计/修复文档 |
| :--- | :--- | :--- | :--- |
| `0001-preserve-bpf-uapi-and-acpi-ipmi.patch` | `compat2` | 修复私有 `BPF_PROG_TYPE_XDP_{EGRESS,GEN}` 插入导致的标准 BPF UAPI 枚举偏移；补齐 Dell SMBIOS BMC 的 `acpi_ipmi` 匹配与模块软依赖 | [kernel-compatibility-fixes.md](../docs/fixs/kernel-compatibility-fixes.md) |
| `0002-initialize-af-xdp-egress-original-queue.patch` | `compat2` | 初始化 AF_XDP `XDP_EGRESS` 上下文的原始队列索引元数据，消除未初始化 `rx_queue_index` 导致的同核高负载停滞 | [first-run-fixes.md](../docs/fixs/first-run-fixes.md) |
| `0003-decouple-xdp-queue-cpu-and-quiesce-xsk.patch` | `compat3` / `compat3.1` | 为每通道引入独立加锁的 `xsk_pace_sq`、按源 `pool->queue_id` 发送与原通道 NAPI 回收、满环补偿暂存、单 `pool` 解绑时间轮精确清池与二次网络 RCU 同步 | [02-driver-decoupling-design.md](../docs/queue-core-decoupling/02-driver-decoupling-design.md) |
| `0004-quiesce-nettx-timers-on-bpf-unload.patch` | `compat3.2` | 在整套 BPF Map 卸载时先同步取消入队 `hrtimer`、清除每核 `NET_TX` 与 pacer 待执行槽位，再经 `call_rcu()` 安全释放定时器与程序引用 | [02-driver-decoupling-design.md](../docs/queue-core-decoupling/02-driver-decoupling-design.md#42-整套-bpf-卸载时的定时器生命周期) |
| `0005-mlx5e-use-async-icosq-for-aff-change-irq.patch` | `compat3.3` | 回移 Linux `6.6.y`（`c326f9c68921`）修复，将 `mlx5e_napi_poll()` 亲和性变更唤醒改为持锁操作 `c->async_icosq` | [03-live-irq-migration-fix.md](../docs/queue-core-decoupling/03-live-irq-migration-fix.md#31-补丁一0005-mlx5e-use-async-icosq-for-aff-change-irqpatch) |
| `0006-handoff-deferred-napi-on-irq-affinity-change.patch` | `compat3.3` | 在 `napi_defer_hard_irqs` 开启下原子封堵旧核 `napi_watchdog` 重排，并以目标核首次有效 `poll` 为完成条件建立每通道交接重试与停用闭环 | [03-live-irq-migration-fix.md](../docs/queue-core-decoupling/03-live-irq-migration-fix.md#32-补丁二0006-handoff-deferred-napi-on-irq-affinity-changepatch) |

## 2. 内核配置片段

在原 `eTran-linux` `.config` 基础上合并以下片段后执行 `make olddefconfig`：

| 配置文件 | 用途 |
| :--- | :--- |
| `abi1.config` | 启用 `CONFIG_ACPI_IPMI=m` |
| `compat.config` | 启用宿主机运行所需的 bridge netfilter、IPv4 filter/NAT/mangle/REJECT/CHECKSUM 以及 NFSv3/v4 客户端与服务端模块 |

## 3. 构建与成套部署约束

1. **用户态与内核成套匹配**：`0001` 涉及 BPF UAPI 扩展编号，`0003` 引入 `pkt_queue_pending()` 新 kfunc 与 `XDP_FLAGS_FROM_GEN`。切换或回退内核大版本（如 `compat2` ↔ `compat3.x`）时，必须同步切换配套编译的 `micro_kernel`、`libetran.so`、eBPF `.o` 与测试应用（详见 [01-baseline-cleanup.md](../docs/queue-core-decoupling/01-baseline-cleanup.md)）。
2. **双机安装与外部模块**：`108-51`（Ubuntu 20.04）与 `108-52`（Ubuntu 22.04）须各自在本机生成 `initramfs` 并针对目标内核头文件原生构建 `knem`、`MFT` 模块，`108-51` 还须重编本机 `VirtualBox` 模块并通过 `finalrd` 检查；升级时严格遵循“保留旧 GRUB 回退项、`108-52` 单次启动先行验证、通过后再切 `108-51`”的流程。
