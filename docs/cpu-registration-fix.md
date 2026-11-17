# CPU 注册、登记与亲和性一致性修复

本次修复将应用 CPU 申请接入注册流程，使微内核的资源登记、用户态实际亲和性和退出回收使用同一份分配结果。验证日期为 2026-11-17。

## 问题与影响

首轮实验通过 `ETRAN_APP_CPU` 在用户库中覆盖应用线程亲和性，微内核仍按 `getCore()` 的结果登记资源。吞吐应用的实际布局为 IRQ CPU18、APP CPU0，微内核记录为 `[18,18]`。

由此产生以下偏差。

- CPU0 的应用占用没有进入引用计数和已用集合，后续应用注册时可能再次获得 CPU0。
- CPU18 多登记了一份应用引用，应用退出时按错误记录递减。
- `/proc/stat` 能反映 CPU0 的负载，资源归属信息仍缺失，影响后续共享与隔离决策。
- 原 `pthread_create` 拦截逻辑还会把硬编码 CPU 范围加入允许集合，使实际亲和性超出微内核返回的应用 CPU 集合。

首轮采集保留了线程和 IRQ 的实际亲和性快照，固定布局可以据此核对。动态 CPU 池需要完成登记、分配和回收的一致性修复。

## 注册时提交 CPU 需求

用户库在 `eTran_init()` 注册前读取 `ETRAN_APP_CPU`，校验整数格式后，将需求交给微内核处理。

| 场景 | 注册流程 |
| --- | --- |
| 未指定应用 CPU | 使用原有 `REG` 请求，由微内核自动分配 |
| 指定应用 CPU | 发送 `REG_CPU`，收到 `REG_CPU_READY` 后发送指定的 CPU 编号 |
| 对端未支持指定 CPU 请求 | 用户库返回协议不支持错误，结束本次注册 |

`REG_CPU` 和 `REG_CPU_READY` 追加到现有枚举末尾，原注册请求、响应结构大小保持一致。微内核使用完整读取处理注册消息，避免将截断消息交给分配流程。

用户库完成格式校验，微内核进一步检查 CPU 是否在线、是否位于支持范围，以及是否占用控制面保留 CPU19。无效申请返回注册失败。

涉及 `eTran/common/intf/intf.h`、`eTran/lib/eTran_common.cc`、`eTran/micro_kernel/runtime/app_if.h` 和 `eTran/micro_kernel/control_plane.cc`。

## 分别分配 IRQ 与 APP CPU

微内核在 `bind_rss_core()` 中先为各 NIC queue 分配 IRQ CPU，再为各业务线程槽位分配 APP CPU，并将每次预留追加到 `usedCores`。

| 参数 | 修复后的用途 |
| --- | --- |
| `ETRAN_THROUGHPUT_CPU` | 指定 throughput 类型的 IRQ CPU |
| `ETRAN_LATENCY_CPU` | 指定 latency 类型的 IRQ CPU |
| `ETRAN_APP_CPU` | 由应用提交指定的 APP CPU |

未指定的部分通过 `getCore()` 按现有负载和候选集合选择 CPU。显式指定允许复现实验中的同核共享布局。

IRQ18、APP0 的分配结果为以下记录。

```text
usedCores = [18, 0]
CPU18 增加一份 IRQ 引用
CPU0 增加一份 APP 引用
```

引用按申请的队列和业务线程槽位计数。IRQ 与 APP 共用同一 CPU 时，各自登记一份引用。

`reserve_cpu()` 统一更新总引用、latency 引用、可用集合及两类已用集合。正常自动分配的 throughput 候选集合避开已被 latency 使用的 CPU。对应的已用候选集合为空且没有可用 CPU 时，分配返回失败。

## 按返回结果设置实际亲和性

注册响应中的 CPU 列表继续采用 IRQ 在前、APP 在后的顺序。用户库验证列表长度和指定 CPU 的返回结果，从中提取 APP CPU 集合。

- 当前注册线程设置为该 APP CPU 集合。
- 后续通过 `pthread_create` 创建的线程设置为同一集合，覆盖业务线程和辅助线程。
- 本地环境变量的作用收敛到注册申请，线程亲和性使用微内核返回的结果。
- 去除额外硬编码 CPU 范围。

单业务线程申请 APP0 时，主线程、业务线程和统计线程的允许集合均为 `[0]`。多个业务线程槽位使用微内核下发的 APP CPU 集合。

实现位于 `eTran/lib/eTran_common.cc`。

## 统一回收与失败处理

CPU 释放逻辑由 `tcp.cc` 移到控制面的 `release_app_cpus()`，正常退出与 CPU 分配阶段失败共用这一入口。

1. 遍历 `usedCores`，按每次预留递减总引用及适用的 latency 引用。
2. latency 引用归零后，移出 latency 已用集合。
3. 仍有 throughput 引用时，加入 throughput 已用集合。
4. 总引用归零时，清理已用集合并放回可用集合。
5. 清空本应用的预留记录，防止重复回收。

CPU 分配阶段出错时，释放已经准备的应用资源和 CPU 引用。分配日志记录 IRQ/APP 用途、PID、CPU 和引用计数，释放日志记录剩余引用及可用状态。

同时初始化用户态缓存指针和 RSS context ID，修正未完成注册时的退出清理，并使用 `delete` 释放 `new` 创建的应用上下文。

涉及 `eTran/micro_kernel/control_plane.cc`、`eTran/micro_kernel/tcp.cc`、`eTran/micro_kernel/runtime/app_if.h`、`eTran/lib/include/app_if.h` 和 `eTran/lib/eTran_common.cc`。

## 运行验证

51 与 52 分别构建本机 `micro_kernel` 和 `libetran.so`，并通过动态依赖检查。以下检查在两机均通过。

| 检查 | 结果 |
| --- | --- |
| 固定 IRQ18、APP0 | 登记与实际亲和性一致，所有应用线程允许集合为 `[0]` |
| throughput 与 latency 显式共用 APP0 | CPU0 总引用为 2，其中 latency 引用为 1 |
| 再注册自动分配的 throughput 应用 | APP CPU 避开 CPU0 和控制面 CPU19 |
| latency 应用退出 | CPU0 总引用降为 1，继续处于已用状态 |
| 全部应用退出 | CPU0、CPU18 引用归零，回到可用集合 |
| 申请 CPU19 后重新注册 | 无效申请被拒绝，微内核继续运行，随后正常注册成功 |

本次自动分配检查覆盖注册时的决策、候选集合更新和退出回收。持续负载变化下的迁移与完整性能隔离评估仍需后续实验。

## compat2 启动与收发回归

52、51 依次完成安装和重启，两机运行 `6.6.0-eTran-compat2`，默认实验启动项指向该版本，启动参数含 `nosmt nr_cpus=20 panic=30`。

- 52 使用本机生成的 initramfs 及 knem、MFT 外部模块。
- 51 使用本机工具生成 initramfs，并在本机重新构建 knem、MFT、VirtualBox 模块。关机前生成并验证 finalrd 依赖。
- 两机均有 20 个在线 CPU，SSH、高速口连通，BPF 程序类型 6、33、34 加载检查通过，systemd failed unit 为 0。
- 51 的 VirtualBox、OpenIB 和 finalrd 服务检查通过。
- 独立时延流和同 NUMA 分核高负载各完成 500,000 次 RTT。分核高负载末尾吞吐样本为 20.35、20.56、20.57、20.58、19.74Gbps。

同核高负载仍出现 TCP 停滞，时延客户端停在第 2,048 次 RTT，吞吐降至 0。CPU 登记与实际亲和性一致，本次内核日志未记录此前的 `xdp_do_redirect() failed` 信息。AF_XDP 字段初始化补丁已加载，整体停滞原因仍需排查。

实验结束后，进程已退出，NIC、NAPI 与 IRQ 管理参数已恢复。两机保持 compat2。

## 验证记录

本地数据目录为 `experiments/20261117-cpu-fix/`，两机远端对应目录均为 `/home/xhcui/repos/eTran-experiments/20261117-cpu-fix`。

- `registration-before-boot/` 保存 CPU 分配、实际亲和性、共享引用、回收与重试记录。
- `verification-summary.json` 汇总两机启动、CPU 登记和收发结果。
- `same-core-after-compat2/` 保存仍然停滞的压力场景证据。
- `latency-alone-after-compat2/`、`split-core-after-compat2/` 保存完成的收发回归。

首轮问题与 AF_XDP 内核补丁的来由见 [首轮实验运行修复](first-run-fixes.md)。
