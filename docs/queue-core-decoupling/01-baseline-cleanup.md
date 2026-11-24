# 基线分支选型、仓库清扫与 24 核容量解锁方案

本文档确定阶段二开发的代码基线，统一“现阶段对所有 NUMA 节点一视同仁、实验期手动框选可用核池”的边界原则，并给出按 **“20 核纯清理基线 → 20 核驱动解耦对照 → 24 核扩容解锁”** 分步推进的工程规范。

---

## 1. 基线选型、远端同步与阶段边界原则

### 1.1 基线选型与双机仓库现状

- **开发基线**：直接基于本地 `f:/Repo/eTran-server` 的 **`2d2ce93`** 切出阶段二开发分支（如 `feat/queue-core-decouple`），不回退至官方仓库 `f:/Repo/eTran` 重搬补丁。
  - 官方仓在共同祖先 `b6e64fd` 之后仅有 `LICENSE` 与 `README.md` 改动（0 行代码更新）；
  - 当前仓的数据面核心（`eBPF/*`、`eTran_posix.cc`、`eTran_rpc.cc`、`xsk_if.cc`、`xsk_buffer_pool`）除 `entrance.c` 放行 ARP 与 `tcp/main.c` 的 `barrier_var(qid)` Verifier 修复外零逻辑污染；
  - 前置 `-Q` 共享/独占队列划分（`a2f3456..a425b37`）、注册协议骨架、内核兼容性补丁与严格绑核回收机制（`c78ec15..2d2ce93`，修复文档已整理至 `docs/fixs/`）均已就绪。
- **远端 `108-51` / `108-52` 同步记录**
  - 阶段 A 同步前，两端 `/home/xhcui/repos/eTran-server` 位于 `da27b79`，并各有未提交的 `control_plane.cc` 改动。同步时逐端与 `f187d73` 比对，归档差异后快进更新。
  - 两端的 `docs/ppt-outline-traffic-isolation.md` 与 `slides/` 均为未跟踪的本地资料，合入及同步时保留在原处。
  - 引用或复跑早期共置矩阵（如 `experiments/20261122-numa0-colocation`）结果时，明确记录其对应构建产物为 `13a7946-abi1`。

### 1.2 现阶段 NUMA 处理原则：机制一视同仁 + 实验手动框选

为严格控制实验变量并与后期研究阶段解耦，现阶段确立以下拓扑原则：

1. **代码机制层对所有 NUMA 节点一视同仁**：微内核控制面、网卡初始化、驱动解耦与创建期核心池分配器**不内置任何自动 NUMA 过滤、节点偏置或跨 Socket 代价逻辑**，池内所有可用核心平等对待。
2. **手动指定可用核池**。实验脚本统一设置微内核的环境变量 `ETRAN_CPU_POOL`，支持 CPU 列表及范围，例如 `ETRAN_CPU_POOL="0,2,6-8"`。`ETRAN_CP_CPU` 可单独指定控制面核，`ETRAN_THROUGHPUT_CPU` 和 `ETRAN_LATENCY_CPU` 分别指定吞吐和时延队列的 IRQ 核。
   - 未设置 `ETRAN_CPU_POOL` 时，可用核池包含除控制面保留核之外的所有在线 CPU。
   - 同 NUMA 受限核实验通过 `ETRAN_CPU_POOL` 选择同一节点的物理核子集。
   - 跨 NUMA 对照实验通过调整同一个核池列表选择不同节点的核。

### 1.3 推荐推进顺序：先在 20 核环境验证驱动解耦，再单独解锁 24 核

为避免将“驱动发包/回收逻辑改造”与“宿主机 CPU/队列规模变化”混在同一变量中，执行顺序调整为：

1. **Step 1 · 20 核纯清理与行为拆分基线**：在现有 20 核环境完成死代码/脏产物清理与原子化行为修正，复跑单跑/混跑冒烟确认基线零回退；
2. **Step 2 · 20 核驱动解耦对照验证**：保留 compat2 回退项，在相同 20 核拓扑和压测口径下切换 compat3，复验阶段 A，并验证 [驱动解耦与 AF_XDP/BPF 生命周期修复](02-driver-decoupling-design.md)；
3. **Step 3 · 动态 CPU 容量适配与 24 核环境解锁**：完成全栈动态 CPU ID 改造，先在 20 核冒烟通过，再新增保留 `nosmt`、移除 `nr_cpus=20` 的 24 核 GRUB 启动项（保留原 20 核启动项供随时回退对照），逐台切换验收高核号数据面。

---

## 2. 仓库清扫与行为修正规范（按职责拆分独立提交）

清扫阶段严禁将“无副作用的死代码/脏文件删除”与“会改变队列发号或发压节奏的行为调整”混在同一个 Git 提交中，分三步独立提交：

### 2.1 提交一：纯生成物移除与死代码清理（零运行时行为变化）

| 目标路径 | 具体问题 | 操作规范 |
| :--- | :--- | :--- |
| `eTran/tcp_app/2.txt`、`3.txt`、`4.txt`、`cpu_usage.log`、`eTran/xdp.log` | 调试时误纳入版本管理的运行日志 | `git rm` 移除；`.gitignore` 仅精确添加实际运行输出路径（不用宽泛通配符误伤源码或配置） |
| `batch_exp/` 及 `eTran/tcp_app/*.py`、`eTran/xdp_log_process.py` | `batch_exp/` 既含历史输出（`*.txt`、`metrics.log`、`*.pdf`）也含运行与绘图脚本（`batch_exp_server.py`、`batch_result.py`） | 先清理确认无用的运行输出文本/图表；对脚本文件先核查是否仍有复现参考价值，确认无需保留后再移除 |
| `eTran/lib/socket.cc` | 顶部未使用的 `#define latNum 2`、第 450–481 行从未调用的 `bind_core()`，以及第 515 行被误删的一处空行 | 删除 `latNum` 与 `bind_core()` 并恢复该空行，使 `git diff b6e64fd..HEAD -- eTran/lib/socket.cc` 严格为空 |
| `eTran/micro_kernel/eBPF/tcp/main.c` | 第 21–32 行与第 363–376 行注释掉的 `dedup` LRU Map 调试代码 | 删除调试注释块，仅保留 `barrier_var(qid)` 修复 |
| `eTran/tcp_app/epoll_server.cc` & `lat_server.cc` | 注释掉的 `log_fd` / `cpu_usage.log` 写入片段 | 删除注释代码，保留监听线程 `getpid():gettid()` 打印 |
| `eTran/micro_kernel/control_plane.cc` | 未使用的宏与全局变量（`latNum`、`TCP_LATENCY`、`TCP_THROUGHPUT`、`nxtThroughputCore`、`nxtLatencyCore`）及注释掉的 `create_new_cpu()` | 删除闲置定义与废弃注释块 |
| `eTran/micro_kernel/nic.cc`（仅格式无损替换） | `set_affinity()` 使用 32 位十六进制掩码写入 `smp_affinity` | 在驱动解耦前**保持业务队列与同号在线 CPU 的 1:1 配对关系不变**，仅将写入方式改为向 `/proc/irq/<irq>/smp_affinity_list` 写入十进制核号（消除 32 位左移在 `CPU >= 32` 时归零溢出的隐患，同时避免驱动未解耦前跨核触发 `XSK_NO_COMP` 或 Homa `cpu != qid` 丢包） |

### 2.2 提交二：移除 `lstQueuePoped` 伪补丁（改变独占队列发号起点，单独提交）

- **代码修改**：删除 `eTran/micro_kernel/control_plane.cc` 中 `try_allocate_queues()` 首次调用时无条件执行的 `etran_nic->_free_exclusive_qids.pop_back()` 及静态变量 `lstQueuePoped`。
- **配套脚本与测试注意事项**：
  1. 在 `-q20 -Q10` 配置下，独占队列池为 `Q0–Q9`：移除 `pop_back()` 后，首个注册应用分配到的队列将从原先的 `Q8` 变为 **`Q9`**；
  2. 在后续 24 核 `-q24 -Q0` 配置下，首个注册应用将拿到 `Q23`（若 `CPU 23` 作为控制面保留核，在未加载驱动解耦补丁的旧驱动环境下无法做同号 `Q23 → CPU 23` 测试）；
  3. 由于 `ETRAN_THROUGHPUT_CPU` / `ETRAN_LATENCY_CPU` 控制的是 **IRQ 目标核号**而非队列号，所有受控实验脚本在应用注册后、正式发流前，须在双端显式读取并核对实际分配到的 `QID` 与生效 IRQ 亲和性（或使用占位进程固定目标 `QID`）。

### 2.3 提交三：明确 `epoll_client` 发送间隔语义（独立行为提交）

- **现状与边界说明**（`eTran/tcp_app/epoll_client.cc`）：
  1. 当前全局默认 `send_interval_us = 2000`，且 `usleep(send_interval_us)` 位于每次 `connection_send()` 返回前——遇到内核/共享环缓冲满触发部分写回退时也会休眠阻塞整个 worker 线程。
  2. 注意 `c->pending_bytes` 初始化为 `message_bytes * max_outstanding`：`c->pending_bytes == 0` 代表当前批次全部 `max_outstanding` 字节已提交完毕，**仅在 `-o 1`（单 outstanding）时严格对应单条完整请求发完**；同时 `usleep` 阻塞的是整个 worker 线程，多连接（`-f > 1`）时并非逐连接独立节流。
- **修改规范**：
  1. 将默认 `send_interval_us` 改为 `0`（不节流，同步更新帮助文档）；
  2. 将休眠条件明确限定为当次调用从 `pending_bytes > 0` 推进至 `pending_bytes == 0` 时触发，并在代码注释与帮助信息中标明该参数适用于单连接、`-o 1` 下的批次间隔控制；
  3. 复跑历史矩阵或满载吞吐对照时，命令行始终显式传入 **`-o 1 -u 0`**，消除默认值变更带来的歧义。

---

## 3. 解除内核 `nr_cpus=20` 锁核限制与动态 CPU 容量适配方案

### 3.1 `108-51` / `108-52` 双机锁核现状

SSH 实测核查确认：
- **硬件真实拓扑**：双路 Dell PowerEdge R730，2 × Intel Xeon E5-2650 v4（每路 12 物理核 / 24 线程，整机共 **24 颗物理核 / 48 逻辑线程**；偶数核属 NUMA0，奇数核属 NUMA1）。
- **24 核容量由引导参数控制**：compat2 的 `CONFIG_NR_CPUS=8192` 已覆盖硬件容量，compat3 沿用此配置。
- **引导层锁核位置**：`/etc/grub.d/42_etran_trial`（由 `/etc/default/grub.d/99-etran-trial.cfg` 设为默认项 `etran-experiment`）在启动参数中硬编码了 `nosmt nr_cpus=20 panic=30`，将 `possible` 核截断为 `0-19`（屏蔽了 `CPU 20..23` 共 4 颗物理核，并将 `enp4s0f1np1` 的最大 Combined 通道数限制在 `20`）。

### 3.2 源码中受 CPU / 队列数量影响的全部耦合点与修复设计

本阶段以双机 **`nosmt` 下的 20 / 24 物理核 TCP 实测**作为正式兼容与验收范围（SMT 开启与异构稀疏核号拓扑留作后续专项验证）：

1. **按 sysfs 真实 CPU ID 集合建立运行期拓扑（严禁用计数 `0..N-1` 代替真实 CPU ID）**
   - 解析 `/sys/devices/system/cpu/possible` 与 `/sys/devices/system/cpu/online` 获取真实的在线 CPU ID 有序集合（`libbpf_num_possible_cpus()` 仅返回数量，不能直接用 `0..N-1` 当作有效 CPU ID 列表）。
   - **控制面保留核选定与核验**：`ETRAN_CP_CPU` 可指定一个在线 CPU，默认选在线集合中的最大编号 CPU（20 核下为 `19`，24 核下为 `23`）。创建线程时设置亲和性，随后回读核验。若默认或通过 `ETRAN_CPU_POOL` 指定的核池扣除控制面 CPU 后为空，微内核在启动期报错退出。
   - **按真实 CPU ID 初始化 BPF Map（`eTran/micro_kernel/trans_ebpf.cc`）**：
     - TCP 时间轮 `tw_outer_map`：遍历实际在线/可执行 CPU ID 集合创建并挂载内层 `pkt_queue`（校验最大 CPU ID `< MAX_CPU`）；
     - Homa `cpumap`：仅对实际在线的目标 CPU ID 执行 `bpf_map_update_elem`（内核 `cpu_map_update_elem` 对不存在的 CPU ID 会返回 `-ENODEV`）。
   - **`/proc/stat` 按真实 CPU ID 对齐求差（`eTran/micro_kernel/control_plane.cc`）**：
     - 移除 `assert(out.size() == MAX_CPU)`；解析行首 `cpu<id>` 提取真实 `id`（校验 `id < MAX_CPU`），两次采样严格按同一 `id` 求差更新负载。

2. **保留 `prev_conn` 全槽位显式范围初始化与值校验（`eTran/micro_kernel/eBPF/tcp/tcp.h`）**
   - 原代码手写了 20 个 `__UINT32_MAX__`，仅调大 `MAX_CPU` 会使 `prev_conn[20..]` 被默认置 `0`（误当成 `cc_idx == 0` 的待发聚合 ACK 候选，引发查表失败或错误 ACK 注入）。
   - 改为 `SEC(".data.prev_conn") __u32 prev_conn[MAX_CPU] = { [0 ... MAX_CPU - 1] = NULL_CONN };`，并在加载期逐槽位核对初始值均为 `NULL_CONN`（不只检查数据段字节长度）。

3. **受控扩容编译期常量、警惕 `__u8 qid2xsk` 截断并执行“成套构建部署”**
   - 分别定义清晰的编译期上界（如 `MAX_CPU = 64`、`MAXMAX_CPU = 64`、`MAX_NIC_QUEUES = 32` 或 `64`），保留现有 `MAX_APP_THREADS = 20` 不做无谓放大；运行期严格校验实际开启通道数 `_num_queues <= MAX_NIC_QUEUES` 且分配的 `qid < _num_queues`，`create_rss_context()` 按实际 `_num_queues` 生成 RSS 权重表。
   - **二进制布局与成套部署铁律**：修改 `MAX_NIC_QUEUES` 会直接改变按 `sizeof` 通信的 `register_response`（`intf.h`）、共享内存 `BufferPool`（`xsk_buffer_pool.h`）以及 `bpf_tcp_conn`（`intf_ebpf.h`）结构体大小。**每次调整头文件常量后，`micro_kernel`、`libetran.so`、全部 eBPF `.o` 与测试应用必须基于同一套头文件全量重编，并在服务端与客户端成套部署或成套回退，严禁混用新旧二进制！**
   - **重用构建目录时先清理**。修改 `capacity.h`、`intf.h` 等公共 ABI 头文件后，在两端各自的构建目录先执行 `make -C eTran clean`，再执行 `make -C eTran` 全量编译。微内核、`shared_lib` 和四个 TCP 基准应用使用 `.d` 跟踪头文件依赖，eBPF 通过 `BPF_HEADERS` 声明头文件依赖。统一清理可避免不同子目标复用旧 ABI 的产物。
   - **记录 `qid2xsk` 8 位索引约束**：`struct bpf_tcp_conn` 中的 `__u8 qid2xsk[MAX_NIC_QUEUES]` 为 8 位无符号整数，而 `MAX_XSK_FD` 为 `1024`；若多应用/多线程规模扩大使分配的 `xsk_map_key > 255`，写入 `qid2xsk` 将发生高位截断。在当前 24 核测试规模下运行期校验 `xsk_map_key <= UINT8_MAX`，后续若大规模扩展并发度再将 `qid2xsk` 拓宽为 `__u16`。

---

## 4. 分阶段部署与验收规范

1. **阶段 A · 20 核清理基线验收**
   - 依次合入 2.1（纯清理）、2.2（`lstQueuePoped`）、2.3（`epoll_client -u`）三个独立提交，在现有 20 核环境全量编译；
   - 使用固定分离核位（`Q8 → CPU 8`、`Q6 → CPU 6`，显式传 `-o 1 -u 0`）跑单跑与混跑冒烟，确认注册、有效吞吐（`~20 Gbps`）与 RTT 分位数无回退。

2. **阶段 B · 20 核驱动解耦对照验收**
   - 保留 compat2 的 20 核 GRUB 启动项。按 [02-driver-decoupling-design.md](02-driver-decoupling-design.md) 依次切换到带 `nosmt nr_cpus=20` 的 compat3，复跑阶段 A 后验证跨核、多队列共核和快速重绑。通过后开放 TCP 的队列与 CPU 映射约束。

3. **阶段 C · 24 核平滑切换（保留 20 核启动项 + 单台先行）**
   - 先将动态 CPU 容量改造后的成套产物部署在 20 核环境完成 BPF 加载、注册与收发冒烟；
   - 在 `/etc/grub.d/42_etran_trial` 保留 compat2 和 compat3.1 的 20 核菜单项，追加 compat3.1 的 24 核菜单项，保留 `nosmt` 并移除 `nr_cpus=20`。更新 GRUB 后先单次启动 `108-52`。
   - 切换后用 `cat /sys/devices/system/cpu/online` 与 `lscpu -e=CPU,ONLINE,CORE,SOCKET,NODE` 精确核对 24 颗在线物理核及其 Socket/NUMA 归属（注：`lscpu` 顶部的汇总 `CPU(s)` 可能计入离线 SMT 线程，以 `online` 和 `-e` 逐核表为准），并用 `ethtool -l enp4s0f1np1` 核实最大与当前 Combined 通道数支持 `24` 后，再切换另一台机器（`108-51`）。

4. **阶段 D · 高核号（`CPU 20 / 22`）数据面完备验收**
   - 先以 `-q20` 验证原队列兼容启动；
   - 再开启至少 23 条以上独占通道验收高核号（若网卡支持 24 通道，使用 **`-q24 -Q0`** 使 `Q20`、`Q22` 处于独占池——注意 `-q24 -Q10` 会将 `Q14–Q23` 划入共享池），测试对齐与跨核布局下的 `CPU 20`、`CPU 22`；
   - 双端逐项核验：分配到的 `QID`、IRQ 生效亲和性、实际 NAPI / 时间轮执行核、Completion Ring 持续回收无积压、完整应答口径有效吞吐（`~20 Gbps`）、LAT 尾时延及进程退出后的资源完整归还。
