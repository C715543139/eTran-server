# eTran 动态核池调度 06 版总结：最终门禁与全场景验收汇总

本文档汇总动态核池调度在分支 `codex/dynamic-core-scheduling`（合并至 `main`）上的最终工程交付状态、默认配置及双机实测验收结果（原始日志本地归档于 `experiments/20261126-dynamic-scheduler/README.md`），作为 [01-metrics-and-scheduling-design.md](01-metrics-and-scheduling-design.md) 至 [05-gate-final-closure-and-active-handoff.md](05-gate-final-closure-and-active-handoff.md) 的收官记录。

---

## 1. 交付能力与默认运行配置

1. **零干扰真实负载遥测**：
   - 关闭态 `main.o` 按 `ETRAN_STATIC_QID_STATS=0` 编译，BPF 指令与重定位与基线 `72a2e5e` 一致，零计数引用、不加载计数 map；
   - 启用态 `counted.o` 将每方向包数与字节数打包于单 64 位字段（低位立即数包计数），配合 `.bss` 只读 `mmap`、按需查 timing wheel 与高进展免读 `bpf_cc`，默认 `ETRAN_SOFTIRQ_PROBE=0`（保留 `=1` 诊断开关）。
2. **二维准入、合作式单 `TID` 与统一 `MigrationExecutor`**：
   - 支持 `SLO ∈ {LATENCY, AUTO, THROUGHPUT}` × `Load ∈ {IDLE, LIGHT, HEAVY, UNKNOWN}`、`reject | degrade` 准入契约及每应用 `ETRAN_IRQ_CPU`；
   - 合作式单 `TID` 自绑采用单原子字 `cmd_word / ack_word` 序号握手，保持 [eTran_posix.cc](../../eTran/lib/eTran_posix.cc#L660-L666) 非阻塞忙轮询语义；
   - `MigrationExecutor` 统一承接 APP 与 IRQ 迁移：活跃 IRQ 同时支持驱动延迟交接确认（`aff_handoff_done`）与硬中断直切双拍确认（[irq_migration_io.cc](../../eTran/micro_kernel/irq_migration_io.cc#L108-L184) `/proc/interrupts` 目标核增长 + 源核连续两拍静默），具备 `ROLLING_BACK`、`QUARANTINED` 与注销 `CANCELLED_QUIESCENT` 排空栅栏。
3. **全异步控制面生命周期与实时 CC 环路**：
   - 独占 TCP 的 `bp_init`、`XSK` 建毁、`ethtool -X` RSS 与 `ethtool -U` Flow Director 规则全部下沉至单 FIFO 管理线程（`ETRAN_MGMT_CPU`）；
   - 控制线程（`ETRAN_CP_CPU`）默认启用 `ETRAN_CP_RT_PRIO=1`（`SCHED_FIFO` 优先级 1，无权限时优雅降级），将活跃流 `200 µs` DCTCP CC 环路的最大抖动压至亚毫秒级。
4. **默认开关与阈值（双机 24 核 `6.6.0-eTran-compat3.3`、`nosmt`、NUMA0 偶数核实测配置）**：
   - `ETRAN_SCHED_MODE=off`（默认关闭，支持 `off | shadow | admit_only | manual_actuate | active`），`ETRAN_FOLD_MODE=app_to_irq`，`ETRAN_ADMISSION_POLICY=reject`；
   - `B_high/low = 3.0 / 1.5 Gbps`、`P_high/low = 350 / 200 kpps`、`S_high/low = 65% / 55%`、`C_bps_cap = 22 Gbps`、`C_pps_cap = 2.0 Mpps`；
   - 推荐 NUMA0 辅助核分配：`ETRAN_CP_CPU=14`、`ETRAN_SCHED_CPU=12`、`ETRAN_MGMT_CPU=16`，配合 `numactl --membind=0` 确保 UMEM 100% 落于 NUMA0。

---

## 2. 门禁验收终表（`G0 / G1 / G2 / G3`）

| 门禁 | 验收范围 | 最终实测结果 | 结论 |
| :--- | :--- | :--- | :--- |
| **`G0` 64 B 小包开销** | 64 B、16 连接 3 轮交替 | 旧基线 `1,185.155` / 同版 `off` `1,186.315` / `counters` **`1,175.465 Kops/s`**（相对旧基线 **`-0.82%`**、相对 `off` **`-0.91%`**） | **通过（双基准 `< 1%`）** |
| **`G0` 32 B LAT & 画像** | 32 B 6 轮交替 + 无探针标定 | `p99` 增量 **`+0.032 / +0.074 µs`**（`< 0.5 µs`）；`ETRAN_SOFTIRQ_PROBE=0` 下 `light-1g/2g` 与 `multi-q8-q6` 100% 准确；`off` 零引用 | **通过** |
| **`G1-coop-check`** | 正式合作式单 `TID` `4→3→4` | 6/6 按序号确认（`276–441 µs`）；收缩过渡窗 `p99 = 23.32 µs`、最大 RTT 中位数 `76.87 µs`、`> 100 µs = 0`；稳态 3/3 满足 `≤ 25 µs` | **通过** |
| **`G2-cc-jitter`** | 并发注册/建连/迁移/回收 + NUMA | 活跃 CC `p50 = 204–205 µs`、`p99 = 369–371 µs`、**`max = 416–477 µs`**（较 Batch 3 的 `509–568 ms` 压降 **1,200×**，`active` 6/6 端次 `gt500 = 0`）；UMEM 131,072 页 100% NUMA0 | **通过** |
| **`G3-quiesce`** | 迁移中强杀立即同 `QID` 重绑 + 故障注入 | 双端各 **20/20** `CANCELLED_QUIESCENT` 并恢复 `Q0@CPU8`；APP 暂停超时回滚与持续暂停 `QUARANTINED` 双端通过，内核告警 0 | **通过** |

---

## 3. `ETRAN_SCHED_MODE=active` 全场景终表（`S0–S5`）

### 3.1 `S5` 三核同底座横向对照（3 轮交替中位数，NUMA0 核池 `0,6,8`）

| 模式 | TP 有效吞吐 (Gbps) | LAT `p50 / p99 / p99.9` (µs) | 51 `CPU 0` 忙碌度 | 52 RTT 样本数（2 s 窗） | 活跃 CC `gt500` |
| :--- | ---: | ---: | ---: | ---: | :--- |
| 同版 `off` | `18.80` | `833.61 / 2,804.08 / 4,330.19` | `0%` | `2,024` | 1 端次有 1 次 |
| 静态 `oracle`（人工最优） | `19.60` | `21.54 / 28.84 / 40.71` | `100%` | `92,902` | 2 端次各 1 次 |
| **`active`（本方案自动调度）** | **`19.80`** | **`20.55 / 28.91 / 41.42`** | **`100%`** | **`93,588`** | **6/6 端次均为 `0`**（`max 477 µs`） |

- 相比 `off`：`active` 将 **`LAT p99` 压低 `97.0×`**、`p99.9` 压低 `104.5×`，同时 TP 提升 `+1.00 Gbps`；
- 相比静态 `oracle`：`active` 自动收敛至完全相同的甜点布局（`TP APP=0 / IRQ=8`，`LAT APP+IRQ=6`），吞吐中位数与区间完全贴齐（`19.48–19.92` vs `19.48–19.84 Gbps`），`LAT p99` 差距仅 `+0.07 µs`。

### 3.2 `S0–S4` 动态场景汇总

| 场景 | 实测行为与核心指标 |
| :--- | :--- |
| **`S0` 3 核先 `LAT` 后 `TP` 自动折叠** | 双端 `R4` 一步迁 `LAT IRQ → CPU 0`；稳态 `TP = 19.84 Gbps`、`LAT p99 = 29.36 µs` |
| **`S1-merge` 3 核 `AUTO` 容量内合并** | ① `AUTO=LAT` 零迁移自共置（`9.86 Gbps / 27.75 µs`）；② `AUTO=TP` 三轮共 6 端次 **6/6 一次迁移成功零回滚**（5 次驱动 `done_delta=1` 耗时 `7.62–8.77 ms`，1 次命中 `direct_irq=1 / target_irq_delta=5` 耗时 `16.41 ms`），合计吞吐 `18.42–18.61 Gbps` |
| **`S1-cap-degrade` 3 核 `AUTO` 超容保护** | 双端记录 `irq_cap_exceeded` 拒并，守住既有 `TP1 = 18.36 Gbps`，新增 `TP2` 自折叠降级 `2.96 Gbps` |
| **`S2` 4 核单核 `BG` 变载解救与释放展开** | `BG` 凭 `ETRAN_APP_CPU=10 ETRAN_IRQ_CPU=10` 独占 `CPU 10`；升压后 `T_detect = 71.91 ms`、`T_actuate = 12.71 ms`、`p99 ≤ 32 µs` 恢复 `150 ms`；`BG` 退出后双端自动展开至 `CPU 10`，恢复 4 核全分离（`TP = 19.76 Gbps`、`LAT p99 = 20.03 µs`） |
| **`S3` 多 worker 单 `TID` 精准迁核** | `Worker 0` `228 µs` 确认迁至 `CPU 4`，`Worker 1` 核位不变且接收速率中位数 `16.85 → 16.93 Gbps` 零扰动；暂停注入不合作准确回滚 |
| **`S4` 高 QPS 64 B `LATENCY` + `TP`** | 4 核识别 `(LATENCY, HEAVY)` 零误折（`LAT 1.024 Mops/s / TP 17.28 Gbps`）；3 核无解下 `reject` 双端返 `-ENOSPC`（`LAT 1.156 Mops/s`），`degrade` 保 LAT 分核（`1.126 Mops/s`）并将 TP 降级自折叠（`3.16 Gbps`） |
