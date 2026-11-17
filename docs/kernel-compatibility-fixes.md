# 内核兼容性与 BPF ABI 修复

记录实验环境准备阶段的 BPF UAPI、Dell ACPI/IPMI 和内核配置调整，以及对应的用户态依赖同步。验证记录来自 2026-11-17 的 51、52 两机运行结果。

## 运行基线

| 项目 | 设置 |
| --- | --- |
| 服务器 | 51、52，Dell PowerEdge R730 |
| 系统 | 51 为 Ubuntu 20.04，52 为 Ubuntu 22.04 |
| 内核源码基线 | eTran-linux，提交 `045b25f1edeb276a17876961762e6e3f27747c54` |
| 已验证运行版本 | `6.6.0-eTran-compat2` |
| CPU 启动参数 | `nosmt nr_cpus=20 panic=30` |

内核源码位于独立的 eTran-linux 构建目录，本仓库通过 `kernel-patches/` 保存对应补丁和配置片段。

## BPF UAPI 编号偏移

### 问题与原因

原 eTran 内核把 `BPF_PROG_TYPE_XDP_EGRESS`、`BPF_PROG_TYPE_XDP_GEN` 插入标准 `bpf_prog_type` 枚举中间，后续标准项的数值随之偏移。`bpf_attach_type` 中也存在同类插入。

系统中的 systemd 等程序按标准头文件编译，通过 BPF 系统调用传递整数类型编号。内核中的编号偏移后，程序类型和附加类型的解释出现错位，启动时出现 cgroup BPF 过滤器加载失败。仅调整 eTran 用户库的头文件无法修复系统程序与内核之间的 ABI 偏差。

### 修复方案

保留当前基线的标准枚举编号，将 eTran 扩展移到枚举末尾并显式赋值。

| 扩展项 | 编号 |
| --- | --- |
| `BPF_PROG_TYPE_XDP_EGRESS` | 33 |
| `BPF_PROG_TYPE_XDP_GEN` | 34 |
| `BPF_XDP_EGRESS` | 56 |
| `BPF_XDP_GEN` | 57 |

内核补丁同时修改 `include/uapi/linux/bpf.h` 和 `tools/include/uapi/linux/bpf.h`。本仓库同步以下三份用户态头文件，使 eTran 依赖库和内核使用一致的扩展编号。

- `lib/libbpf/include/uapi/linux/bpf.h`
- `lib/xdp-tools/headers/linux/bpf.h`
- `lib/xdp-tools/lib/libbpf/include/uapi/linux/bpf.h`

内核和相关用户态组件需要配套重新构建。上述编号对应本次使用的内核基线，迁移到其他内核版本时需要重新核对标准枚举的分配情况。

## Dell ACPI/IPMI 接口匹配与加载顺序

### 问题与原因

ACPI power meter 通过 IPMI OpRegion 访问本机 BMC。当前 Dell 固件暴露 ACPI IPMI 设备，而可用 BMC 接口通过 SMBIOS 注册。原 `acpi_ipmi` 仅接受 `SI_ACPI` 来源，SMBIOS 接口无法进入可用设备列表。

另外，power meter 探测依赖 `ipmi_si` 和 `acpi_ipmi` 提前准备好接口与地址空间处理器，需要明确模块加载顺序。

### 修复方案

`drivers/acpi/acpi_ipmi.c` 增加受硬件条件约束的回退路径。

1. 对 `SI_ACPI` 来源继续取得并校验 ACPI handle。
2. 对厂商匹配 `Dell Inc.` 且来源为 `SI_SMBIOS` 的接口，允许以空 ACPI handle 注册本机 BMC。
3. 按 IPMI 接口编号去重，对有效 ACPI handle 同时检查重复注册。
4. 选择接口时优先使用带有效 ACPI handle 的接口。

`drivers/hwmon/acpi_power_meter.c` 增加软依赖。

```c
MODULE_SOFTDEP("pre: ipmi_si acpi_ipmi");
```

BPF UAPI 与 ACPI/IPMI 修改共同保存在 `kernel-patches/0001-preserve-bpf-uapi-and-acpi-ipmi.patch` 中。

## 补齐主机服务需要的内核模块

eTran 原配置中部分模块缺失，影响主机已有的 IPv4 过滤、NAT、桥接和 NFS 功能。配置片段在原内核配置上增量启用所需选项，再由 `olddefconfig` 解析依赖。

| 配置片段 | 内容与用途 |
| --- | --- |
| `kernel-patches/abi1.config` | `CONFIG_ACPI_IPMI=m`，构建 ACPI IPMI 模块 |
| `kernel-patches/compat.config` | 启用 bridge netfilter、IPv4 filter/NAT/mangle、REJECT、state、CHECKSUM 相关模块，以及 NFS 客户端、服务端与 ACL 支持 |

构建记录中检查了 `iptable_filter`、`iptable_nat`、`iptable_mangle`、`ipt_REJECT`、`xt_state`、`xt_CHECKSUM`、`br_netfilter`、`nfs`、`nfsd`、`sunrpc`、`acpi_ipmi` 和 `mlx5_core` 等模块。

两机使用各自系统的工具生成 initramfs。knem、MFT 等外部模块按目标内核准备，51 的 VirtualBox 模块在本机重新构建。启动文件中检查存储、管理网卡和高速网卡驱动，相关安装与回归过程见 [CPU 注册、登记与亲和性一致性修复](cpu-registration-fix.md)。

## 仓库配套调整

- `.gitmodules` 清理遗留的 libbpf、xdp-tools submodule 条目，两份依赖当前以普通源文件纳入版本管理。
- `.gitignore` 增加 `experiments/`，用于存放本地实验日志与构建、验证产物。
- `eTran/micro_kernel/eBPF/entrance/entrance.c` 整理结构体声明、分支缩进和空白格式。

## 已有验证结果

两机重启到 compat2 后，BPF 加载探针的记录如下。

```text
type=6 attach=37 loaded
type=33 attach=56 loaded
type=34 attach=57 loaded
```

留存的启动与服务检查记录还确认了以下结果。

- 两机均有 20 个在线 CPU，SSH 与高速数据口连通。
- 两机 systemd failed unit 为 0，启动内核错误级别日志没有记录条目。
- 51 的 VirtualBox、OpenIB 和 finalrd 服务检查通过。
- 独立时延流及同 NUMA 分核高负载完成收发回归。

同核高负载下的 TCP 停滞仍在后续回归中复现，相关故障证据和 AF_XDP 队列元数据补丁见 [首轮实验运行修复](first-run-fixes.md)。

本地汇总位于 `experiments/20261117-cpu-fix/verification-summary.json`，两机的 BPF 探针和启动日志摘要位于该目录下的 `host51/postboot-summary.json`、`host52/postboot-summary.json`。远端验证目录为 `/home/xhcui/repos/eTran-experiments/20261117-cpu-fix`。
