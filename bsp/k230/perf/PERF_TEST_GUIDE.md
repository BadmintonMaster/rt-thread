# C908（K230）内存层级性能测试方案（零基础教学版）

| 项 | 内容 |
|----|------|
| 被测对象 | K230 大核 **XuanTie C908**（RISC-V64, rv64imafdcv），RT-Thread `bsp/k230`（RT-Smart） |
| 对标对象 | **ARM Cortex-A53**（Amlogic S905，1536 MHz，来源 7-cpu.com） |
| 测试重点 | **TLB**（地址翻译缓存）、**TWU**（Translation Walk Unit，硬件页表遍历）、**Cache**（L1/L2/DRAM 层级延迟与带宽） |
| 测试载体 | 内核态 finsh/msh 命令 `perf_*`（S 态运行，位于 `bsp/k230/perf/`） |
| 计量手段 | RISC-V S 态只读计数器 `cycle`/`instret`/`time`（无需 SBI PMU 驱动） |
| 目标平台 | **真机 K230 开发板**（时延/带宽数据才有物理意义） |
| 面向读者 | 第一次做处理器内存子系统性能测试的工程师 |
| 文档性质 | 本文档由 AI 协助生成，**需关注信息准确性及幻觉问题**；数值需在真机实测后回填 |

> 阅读顺序建议：先读第 1、2 章理解“测什么、为什么”，第 3 章看方法论（如何做到公平、消除干扰），第 4 章逐项看代码原理，最后按第 5–7 章操作构建、烧录、读结果、对比 A53。

---

## 1. 背景：我们到底在测什么

CPU 访问一个变量，看起来只是一条 load 指令，但硬件在背后做了两件耗时的事：

1. **地址翻译**：程序用的是虚拟地址（VA），内存芯片认的是物理地址（PA）。CPU 必须把 VA 翻译成 PA。翻译结果缓存在 **TLB**；如果 TLB 没命中，就要由 **TWU** 去内存里逐级读页表（page walk），SV39 走 3 级、SV48 走 4 级，每级都是一次访存。
2. **取数据**：拿到 PA 后，去 **Cache**（L1→L2）找数据，找不到再去 **DRAM**。层级越深，延迟越大。

所以“一次访存有多快”取决于：**TLB 命中吗？Cache 命中吗？** 本方案就是用软件手段，把这两个维度的成本一层层量出来，并折算成与 A53 可比的口径。

### 1.1 C908 硬件特性（测试设计的约束）

| 部件 | C908 参数 | 对测试的含义 |
|------|-----------|--------------|
| L1 I-TLB | 32 项，全相联 | 取指翻译，本方案不重点覆盖（数据侧为主） |
| L1 D-TLB | **17 项**，全相联 | 页步进访问超过 **17×4K = 68KB** 后开始 miss |
| L2 TLB | 4 路组相联，**512 或 1024 项** | 超过 **512×4K=2MB** 或 **1024×4K=4MB** 后每次访问都要走 TWU |
| 页大小 | 4K / 2M / 1G / 512G | 大页可让一个 TLB 项覆盖更大区域，减少 miss |
| 地址模式 | **SV39 / SV48** | 决定页表级数（3 vs 4），即 TWU 遍历深度 |
| Cache line | 64 B | 步进 ≥64B 时每次访问都是新 cache line |
| PMU | 支持（本方案默认不依赖） | 见 §3.5 PMU 说明 |

### 1.2 A53 基准数据长什么样

A53 参考页（`ARM Cortex-A53.html`）的核心是一张 **“延迟 vs 工作集大小”曲线**（4KB 页 / 64-bit Linux）：

```
  Size     Latency (cyc + ns)   说明
  32 K     3                    全部命中 L1（3 周期）
  64 K     10                   +L2 命中 +Micro-TLB miss
 128 K     14
 256 K     16
 512 K     17 + 4ns
   1 M     17 + 70ns            开始落到 DRAM
   2 M     17 + 100ns
   4 M     25 + 114ns           +L2 TLB miss（+22 周期）
   8 M     31 + 121ns
```

它还给了：L1=3cyc、L2=15cyc、RAM=15cyc+128ns；带宽（RAM 读 ~2000MB/s、写 ~3700MB/s）；分支误预测惩罚=7 周期。

**关键洞察**：这条曲线的“台阶（拐点）”位置暴露了 cache 容量和 TLB 容量，“台阶高度”暴露了各级延迟。我们对 C908 测同样形状的曲线，就能逐项对标。

---

## 2. 测试项与 A53 的对齐关系

| 我们的命令 | 测什么 | 对标 A53 的哪项 |
|-----------|--------|----------------|
| `perf_lat` | 指针追逐延迟-容量曲线（步进 64B） | A53 “Latency vs Size” 主表；L1/L2/RAM 延迟拐点 |
| `perf_tlb` (A) | 页步进延迟（每页一次访问），扫页数 | A53 Micro D-TLB(10项)/L2 TLB(512项) 拐点 |
| `perf_tlb` (B) | `sfence.vma` 全刷/单页刷开销 | A53 无直接项（TLB 维护成本，补充） |
| `perf_tlb` (C) | 冷 TLB vs 热 TLB 单页遍历成本 = **TWU 延迟** | A53 “L2 TLB miss +22 周期”“page walk to RAM” |
| `perf_bw` | 顺序读/写带宽（MB/s，周期/行） | A53 “RAM Read/Write B/W” |
| `perf_branch` | 分支误预测惩罚估计 | A53 “Branch misprediction penalty = 7 cyc” |

初步构想中的“页面大小 4K/2M/1G 差异”“SV39/SV48 差异”，本方案的处理见 **§6（进阶实验）**：SV39/SV48 用“重编译对比”实现（代码已支持切换），4K/2M 大页对比作为可选进阶实验说明。

---

## 3. 方法论：如何做到“测得准、可归因”

新手最常见的错误是“写个循环 `clock()` 一减就报数”。那样测出来的数字混入了大量噪声，无法归因。本方案用 5 个手段保证数据可信。

### 3.1 控制变量：同一段机器码，只变工作集

每个数据点跑的是**完全相同的机器指令**（同一个 unrolled 指针追逐循环），唯一变量是 buffer 的大小（range）。因此周期数的差异只能来自“工作集变大导致 cache/TLB 命中率下降”，而不是代码不同。为此测量函数都用 `__attribute__((noinline, optimize("O2")))` 固定编译形态。

### 3.2 依赖链（pointer chasing）：逼出真实单次延迟

如果顺序访问数组，CPU 会**预取**并**并行**多个 load，测到的是吞吐不是延迟。我们把 buffer 组织成一个**随机置换的指针环**：下一个地址来自上一次 load 的结果（`p = *p;`）。这样每次 load 必须等上一次完成，无法并行，测到的就是**单次 load-use 延迟**（含 TLB miss + cache miss）。随机置换还能击穿硬件预取器。

- 代码：`perf_build_chase()`（Fisher–Yates 洗牌成单一哈密顿环）。

### 3.3 缓存/TLB 预热：区分“冷”和“热”

- **热测量**：先 `perf_warm_chase()` 走几圈，把该工作集的数据/翻译都装进 cache/TLB，再测——得到**稳态延迟**（`perf_lat`、`perf_tlb`(A) 用此）。
- **冷测量**：先 `sfence.vma` 刷空 TLB，紧接着计时第一遍遍历——每次访问都 miss，测到的是**冷启动/TWU 成本**（`perf_tlb`(C) 用此）。
- 两者相减（cold − hot）≈ 纯 TWU 页表遍历成本。

### 3.4 干扰隔离与统计

1. **关中断**：短测量窗口内 `rt_hw_interrupt_disable()`，避免 tick 中断混入。**注意**：带宽测试（`perf_bw`）窗口长达毫秒级，故意**不关中断**——否则关太久会丢 tick；带宽是吞吐量，单次 tick 开销占比可忽略。
2. **取最小值**：每个数据点重复 `PERF_REPEAT`(默认 7) 次取 **min**。min 是“最少被打断”的那次，最接近硬件真实成本。
3. **减空窗开销**：启动时标定“空测量窗口”的固定成本（读计数器本身的周期），每次测量都减掉（`perf_calib.empty_cycle`）。
4. **流水线序列化**：计数器读取前后加 `fence rw,rw`（`perf_barrier()`），确保窗口精确括住被测代码。

### 3.5 计数器：三个各司其职（以及 PMU 说明）

RISC-V S 态可直接读三个只读计数器，**无需 SBI/PMU 驱动**（OpenSBI 默认打开 `mcounteren` 允许 S 态访问）：

| 计数器 | 读法 | 用途 |
|--------|------|------|
| `cycle` | `rdcycle` | **真实成本**，主指标，换算成 ns / ns/GHz |
| `instret` | `rdinstret` | **确定性照妖镜**：同项跨数据点若指令数应相同却不同，说明跑了不同代码，数据作废 |
| `time` | `rdtime` | 墙钟（K230 @ 27MHz），用于**自动标定 CPU 主频**（见 `perf_calibrate`） |

> **PMU（本方案的取舍）**：C908 支持 PMU，可直接数“TLB miss 次数”“cache miss 次数”。但要读这些硬件事件计数器，需要 OpenSBI 提供 SBI PMU 扩展或 M 态编程，当前 `bsp/k230` + libcpu 中**没有现成 PMU 驱动**。因此本方案采用“**周期计数为主 + 时延曲线推断层级**”的稳健路线（这也是 lmbench、7-cpu.com 的经典做法）：miss 率不直接数，而是从“延迟随容量的台阶”推断。若后续接入 SBI PMU，可在此框架上加一层事件计数，属增强项。

### 3.6 主频归一化：ns/GHz 是跨架构对比的通用尺

C908（~1.6GHz）与 A53（1.536GHz）主频不同，直接比 ns 不公平。团队报告的通用口径是 **ns/GHz**：

> ns/GHz = 周期数（因为 1 个周期 @ 1GHz 恰好 = 1 ns）。

所以我们输出的 `ns_per_ghz` 列**在数值上等于 cycles/access**，可直接与 A53 表里的“cycle”列横向对比，消除主频差异，比较的是**微架构效率**。同时我们也输出绝对 ns 供参考。

---

## 4. 代码解析（`bsp/k230/perf/`）

| 文件 | 作用 |
|------|------|
| `perf_common.h/.c` | 计数器读取、主频标定、统计、指针环构建、浮点打印、横幅 |
| `perf_mem.c` | `perf_lat`（延迟曲线）、`perf_bw`（带宽） |
| `perf_tlb.c` | `perf_tlb`（页步进 / sfence / 冷热 walk） |
| `perf_branch.c` | `perf_branch`（分支误预测） |
| `perf_main.c` | `perf_all`（一键全跑 + A53 参考表） |
| `SConscript` | 由 `BSP_USING_PERF_TEST` 开关控制编译 |

### 4.1 主频自动标定 `perf_calibrate()`

原理：`cycle` 和 `time` 在任意时间段内都同步前进（即使被中断也一起走）。所以：

```
cpu_hz = (cycle_end - cycle_start) × timebase_hz / (time_end - time_start)
```

在约 20ms 墙钟窗口内测 cycle 增量即可，**无需关中断**（比值天然抵消中断时间）。K230 的 `timebase_hz` 取 27MHz（`board.h` 的 `TIMER_CLK_FREQ`，也可用 Kconfig/`-D` 覆盖）。启动横幅会打印“derived CPU freq”，**请以此校验是否与预期主频吻合**。

### 4.2 指针环 `perf_build_chase()` / 依赖链

在 `[buf, buf+range)` 内每隔 `stride` 放一个指针，用 Fisher–Yates 洗牌把它们串成一个覆盖全部 link 的单环。追逐时 `p = *p;` 形成依赖链。`stride=64B` 时测 cache 层级；`stride=4096B`（一页）时测 TLB。

### 4.3 `perf_lat`：延迟-容量曲线（核心）

- 工作集从 4KB 按 lmbench 的几何级数增长到上限（默认 8MB）。
- 每个大小：建环 → 预热 → `lat_window()` 关中断跑 4096 次依赖 load → 取 min → 减空窗 → 除以 load 数 = **cycles/access**。
- 输出 CSV：`size, cyc_per_access, ns_per_access, ns_per_ghz`。
- **怎么看**：曲线应出现台阶——落在 L1（32K 内）时最低（几周期），超过 L1 升到 L2，超过 L2/TLB reach 再升到 DRAM。台阶位置=容量，台阶高度=延迟。

### 4.4 `perf_tlb`：三个子测

- **(A) 页步进**：`stride=4096`，每页只碰一个 cache line（数据基本命中 cache），扫“页数”从 4 到 2048。延迟在 ≤17 页（68KB）平；超过后进 L2 TLB；超过 512/1024 页（2M/4M）后每次都要 TWU。**拐点即 TLB 容量**。
- **(B) sfence 开销**：循环 4096 次 `sfence.vma`（全刷）与 `sfence.vma addr`（单页刷），测每条均摊周期。
- **(C) 冷热 walk**：对 64 页，先 `sfence.vma` 刷空再计时第一遍（cold，全 miss+全遍历），对比预热后的热遍历（hot）。`cold − hot` = **TWU 单页遍历成本**。SV39/SV48 下重跑此项对比页表深度影响。

### 4.5 `perf_bw`：带宽

顺序求和（读）/顺序写，扫大小。输出周期/cache-line 和 MB/s。**不关中断**（见 §3.4）。对标 A53 的 RAM 读/写带宽。

### 4.6 `perf_branch`：分支误预测惩罚

同一段 `if(arr[i])` 循环（编译为 **O0** 保证是真分支而非条件传送），分别喂**可预测数据**（全 1）和**随机数据**（50% 1）。两者每分支周期差 = 误预测导致；`惩罚 ≈ 差值 / 误预测率(≈0.5) = 2×差值`。对标 A53 的 7 周期。

---

## 5. 构建、打包、烧录与启动（Env / scons → K230）

> 完整环境搭建见 `bsp/k230/README.md`。以下是**实测验证过**的增量步骤（已在 WSL Ubuntu-22.04 上成功编译出含本套件的 `rtthread.bin`）。

### 5.0 前置：工具链与依赖（关键，先读）

- **工具链必须支持 RVV 1.0**。本 BSP 默认开启向量（`BSP_RISCV_FPU_D` → `ARCH_RISCV_VECTOR`），内核 `components/lwp/arch/risc-v/rv64/lwp_gcc.S` 使用 RVV 1.0 指令/CSR（`vsetvli e8,m8`、`vle8.v`、`vcsr` 等）。
  - README 推荐的 `riscv64-unknown-linux-musl-gcc 10.1.0` 只支持早期草案向量，会报 `unknown CSR vcsr` / `unrecognized opcode vsetvli` 而**编译失败**。
  - 请改用较新的 **Xuantie musl 工具链（GCC 14.1.1）**：`~/tools/Xuantie-900-gcc-linux-6.6.0-musl64-x86_64-V3.0.2/bin`，前缀同为 `riscv64-unknown-linux-musl-`。（若坚持用 10.1.0，需在 menuconfig 选 `Software floating-point` 关闭向量。）
- **Env 依赖 tqdm**：`pkgs --update` 需要 python 模块 `tqdm`，缺失时报 `ModuleNotFoundError: No module named 'tqdm'`，先 `pip3 install tqdm`。
- **在线包 zlib**：本 BSP 默认开启 `RT_USING_DFS_CROMFS`，依赖 `zlib` 在线包。首次构建前须在 `bsp/k230` 下 `pkgs --update` 拉取，否则报 `zlib.h: No such file`。
- **仓库脚本是 CRLF**：`build.sh`/`utils.sh`/`flashsd.sh` 在 Windows 检出后为 CRLF，WSL 直接 `./build.sh` 会报 `No such file or directory`（shebang 带 `\r`）。运行前先转 LF：`sed -i 's/\r$//' build.sh utils.sh flashsd.sh`（临时本地修改，勿提交）。
- **`source ~/.env/env.sh` 报 `.venv/bin/activate: No such file or directory` 是无害的**（实测确认）。原因：系统缺 `python3.10-venv`（`python3 -m ensurepip` 报 `No module named ensurepip`），导致 `~/.env/.venv` 只建了目录骨架而没有 `bin/activate`；env.sh 因 `.venv` 目录存在而跳过重建，直接 source 不存在的 activate。bash 在 `source` 中遇错会**继续执行后续行**，所以 `PATH` 仍然正确加入了 `~/.env/tools/scripts`（`pkgs` 可用），构建不受影响。
  - 想消除报错：`source ~/.env/env.sh 2>/dev/null`；或彻底修好（需 sudo）：`sudo apt install -y python3.10-venv && rm -rf ~/.env/.venv && source ~/.env/env.sh`（会重建 venv；注意重建后 python 切到 venv 内，若缺 `kconfiglib`/`tqdm` 需在 venv 里重装）。
- **⚠️ env.sh 会把 `RTT_EXEC_PATH` 覆盖成 `/usr/bin`**（env.sh 最后一行）。所以**必须先 `source ~/.env/env.sh`，再 `export RTT_EXEC_PATH=<你的工具链>`**；顺序写反会导致找不到 `riscv64-unknown-linux-musl-gcc`。§5.2 的命令已按正确顺序给出。

### 5.1 打开测试套件开关

```bash
cd rt-thread/bsp/k230
source ~/.env/env.sh 2>/dev/null   # 忽略 .venv/activate 报错（无害，见 §5.0）
scons --menuconfig
```

在菜单里进入并勾选：

```
[*] Enable C908 memory-hierarchy performance test suite
      (8)  Max working-set size for the sweeps (MB)   # 默认 8，可调
      (7)  Repetitions per data point                 # 默认 7
```

对应 Kconfig 符号 `BSP_USING_PERF_TEST`（见 `bsp/k230/Kconfig`）。保存退出。

### 5.2 编译

设置工具链环境变量后编译（把 `RTT_EXEC_PATH` 也加进 `PATH`，因为 lwp/vdso 子构建通过 PATH 找编译器）：

```bash
cd rt-thread/bsp/k230
source ~/.env/env.sh
export RTT_CC=gcc
export RTT_CC_PREFIX=riscv64-unknown-linux-musl-
export RTT_EXEC_PATH=$HOME/tools/Xuantie-900-gcc-linux-6.6.0-musl64-x86_64-V3.0.2/bin
export PATH=$RTT_EXEC_PATH:$PATH

pkgs --update          # 首次：拉取 zlib 等在线包（需 tqdm）
scons -j$(nproc)
```

成功后生成 `rtthread.elf`（含调试信息）与 `rtthread.bin`（约 620KB），日志末尾出现 `LINK rtthread.elf`。`bsp/k230/perf/*.c` 仅在开关打开时参与编译（`SConscript` 用 `depend=['BSP_USING_PERF_TEST']` 控制）。可用 `grep __fsym_perf rtthread.map` 确认 5 条命令（`perf_all/lat/bw/tlb/branch`）已链接进固件。

### 5.3 关键概念：`.elf`/`.bin` 不能直接启动

`scons` 产出的 `rtthread.elf`（仅供 gdb/objdump）和 `rtthread.bin`（纯二进制）**都不能直接放进 SD 卡启动**。K230 启动链要求内核由 **OpenSBI 承载**并封装成开发板能识别的镜像格式（带 K230 magic 头、压缩、哈希）。因此必须再做一次“二次打包”，得到 `opensbi_rtt_system.bin` 才能烧录。

启动链：`U-Boot SPL → U-Boot → OpenSBI（M 态）→ RT-Smart（S 态）`。`rttpkgtool` 负责编译 OpenSBI（`platform/kendryte/fpgac908`）、把 `rtthread.bin` 作为 payload 打进去、再套 K230 镜像头。

### 5.4 打包（生成 opensbi_rtt_system.bin）

打包会**另外编译 OpenSBI**，它与编内核用的工具链不同，且有一个坑：K230 的 OpenSBI（`platform/kendryte/fpgac908`）Makefile 强制 `-march=rv64gcxthead`，**只有较老的 T-Head GCC 10.1.0 接受该 march**；用 GCC 14.1.1（V3.0.2）或 elf-newlib（V3.4.0）会报 `extension 'xthead' ... unsupported`，导致 OpenSBI 编译失败（**且 mkpkg.sh 仍会误报 success，生成的镜像不可启动**）。因此两半各用一套：

- 编内核（rtthread.bin）：用 **GCC 14.1.1**（见 §5.2）。
- 编 OpenSBI（打包）：用 **GCC 10.1.0** `riscv64-linux-musleabi_for_x86_64-pc-linux-gnu`（README 的下载工具链），或 README 默认的 V2.10.1 glibc 工具链。

打包依赖 `mkimage`(u-boot-tools)、`pycryptodome`、`gmssl`：

```bash
pip3 install pycryptodome gmssl
# 有 sudo：sudo apt install -y u-boot-tools patch
# 无 sudo 时免 root 装 mkimage：
cd ~ && apt-get download u-boot-tools && dpkg-deb -x u-boot-tools_*.deb ubt && cp ubt/usr/bin/mkimage ~/.local/bin/
```

因为仓库脚本是 CRLF，直接跑 `./build.sh` 会失败；这里绕过它，直接驱动 rttpkgtool（其脚本为 LF）：

```bash
cd rt-thread/bsp/k230
git clone -b for-k230 https://gitee.com/unicornx/rttpkgtool.git rttpkgtool   # 首次
cd rttpkgtool
DPT_PATH_KERNEL=$(realpath ..) \
DPT_CROSS_COMPILE=$HOME/tools/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl- \
bash script/mkpkg.sh -f
```

`mkpkg.sh` 会从 gitee 克隆 opensbi 源码（需网络）、用上面的 GCC 10.1.0 编 OpenSBI、把 `rtthread.bin` 作为 payload 打进去、再套 K230 头。**务必验证真成功**（失败时下列文件不存在，但脚本仍打印 success）：

```
output/k230_rtos_01studio_defconfig/opensbi/opensbi.bin                      # 存在且非空
output/k230_rtos_01studio_defconfig/images/opensbi/opensbi_rtt_system.bin    # 最终镜像，约 400KB
```

### 5.5 烧录到 SD 卡

**第一次**要先做一张“基础 SD 卡”（含分区表、U-Boot、rootfs），用 K230 RTOS-Only SDK 生成整卡镜像 `RtSmart-K230_..._rtsmart_..img`，用 balenaEtcher（Windows 方便）烧到卡上。这步只做一次（README §5.1）。

**以后每次**只需把内核写到**裸盘偏移 10 MiB** 处（= 官方 `sdcard.sh` 的 `dd seek=20480`，20480×512B）。该位置在第一个分区之前的空白区（实测 01Studio 基础卡：p1@110MiB、p2@140MiB），**不动分区表和文件系统**，只覆盖旧内核。

#### 方案 1：原生 Linux / 能直通 USB 的虚拟机

```bash
lsblk                  # 先确认设备节点（按容量比对）
cd rt-thread/bsp/k230/rttpkgtool
SRC=output/k230_rtos_01studio_defconfig/images/opensbi/opensbi_rtt_system.bin DEST=/dev/sdb bash script/sdcard.sh
```

#### 方案 2：受策略限制的 Windows 上的可行路径（usbipd-win 直通到 WSL）

先记住三个**已实测行不通**的方法，别浪费时间：

| 方法 | 结果 | 原因 |
|------|------|------|
| `wsl --mount \\.\PHYSICALDRIVE<N> --bare` | ❌ `0x8007000f` | Hyper-V 磁盘直通不支持 `usbstor` 类读卡器；且会把磁盘置 Offline（`Set-Disk -Number N -IsOffline $false` 恢复）|
| Git Bash 的 `dd` | ❌ 读 0 字节且**不报错** | MSYS2 移除了 Cygwin 的裸设备支持，`//./PhysicalDriveN` 被当普通文件（连 `skip=0` 读 MBR 也是 0 字节）|
| PowerShell 调 .NET 裸写 | ❌ 策略禁止 | 公司 WDAC/AppLocker 使 PowerShell 处于 `ConstrainedLanguage`，不能实例化 `System.IO.FileStream`（`$ExecutionContext.SessionState.LanguageMode` 可查）|

**可行方案：用 [usbipd-win](https://github.com/dorssel/usbipd-win) 把读卡器以 USB 设备形式直通进 WSL**，然后在 WSL 里用正常的 `dd`。

前提检查（WSL 内核需带 USB 存储驱动）：

```bash
zcat /proc/config.gz | grep USB_STORAGE      # 应见 CONFIG_USB_STORAGE=m
find /lib/modules -name "usb-storage*"       # 应能找到 usb-storage.ko
```

步骤：

```powershell
# Windows 管理员 PowerShell
winget install --id dorssel.usbipd-win
# 重开管理员 PowerShell（刷新 PATH）
usbipd list                       # 找到读卡器的 BUSID，如 1-6
usbipd bind   --busid 1-6         # 首次需要，每个设备一次
usbipd attach --wsl --busid 1-6   # 附加后该设备从 Windows 消失（E:/F: 不见）
```

```bash
# WSL 里
lsblk                             # 按容量认出 SD 卡（如 29.7G），假设 /dev/sdd
sudo modprobe usb-storage         # 若 lsblk 看不到再手动加载
dmesg | tail -20                  # 确认 usb-storage 识别过程

IMG=/mnt/d/Code/rt-thread_fork_cgh/rt-thread/bsp/k230/rttpkgtool/output/k230_rtos_01studio_defconfig/images/opensbi/opensbi_rtt_system.bin

# 1) 只读探测：确认 10MiB 处是旧内核（应见 K 2 3 0）
sudo dd if=/dev/sdd bs=512 skip=20480 count=1 2>/dev/null | od -A d -c | head -3

# 2) 写入
sudo dd if=$IMG of=/dev/sdd bs=512 seek=20480 conv=fsync status=progress
sync

# 3) 回读校验（必做）
sudo dd if=/dev/sdd bs=512 skip=20480 count=804 of=/tmp/rb.bin 2>/dev/null
cmp -n 411580 /tmp/rb.bin $IMG && echo "VERIFY OK"
```

完成后把设备归还 Windows：`usbipd detach --busid 1-6`（或直接拔读卡器）。

> ⚠️ 在 WSL 里 `/dev/sda` 通常是 WSL 自己的根磁盘（稀疏，显示约 1TB）。**唯一判据是 SIZE 与 SD 卡容量吻合**，写错目标会不可恢复地毁盘。

#### 方案 3：Cygwin（与 MSYS2 不同，**有**真正的裸设备支持）

如不想装 usbipd，可装 Cygwin（默认含 `dd`），以管理员打开 Cygwin 终端：

```bash
cat /proc/partitions        # Cygwin 会列出 sdX 与 PhysicalDriveN 的对应
dd if=/cygdrive/d/Code/rt-thread_fork_cgh/rt-thread/bsp/k230/rttpkgtool/output/k230_rtos_01studio_defconfig/images/opensbi/opensbi_rtt_system.bin \
   of=/dev/sdb bs=512 seek=20480 conv=fsync
sync
```

> balenaEtcher / Win32DiskImager / Rufus **都不行** —— 它们只能烧整卡镜像，不支持指定偏移写入。

### 5.6 上电、进入 msh、跑测试

SD 卡插入 01Studio K230，USB-UART 接 “CPU0 调试口”，波特率 115200，上电。看到 `msh />` 后：

```
msh /> perf_all              # 一键全跑（推荐），末尾附 A53 参考表
msh /> perf_lat 512 128      # 只跑延迟曲线：上限 512KB、步进 128B
msh /> perf_tlb              # 只跑 TLB/TWU
msh /> perf_bw               # 只跑带宽
msh /> perf_branch           # 只跑分支
```

用串口工具全程记录日志，再 `grep '^CSV,' run.log > result.csv` 提取数据。

---

## 6. 进阶实验：页大小与 SV 模式

### 6.1 SV39 vs SV48（TWU 遍历深度）

C908 的 MMU 代码已支持切换（`libcpu/risc-v/t-head/c908/riscv_mmu.h`，`SATP_MODE` 默认 `SATP_MODE_SV39`）。要对比 SV48：

1. 在构建中定义 `SATP_MODE=SATP_MODE_SV48`（例如临时在编译宏 `KCFLAGS` 或该头文件里改），重新 `scons`。
2. 分别用 SV39、SV48 固件跑 `perf_tlb`，比较 (C) 子测的 `twu_delta`：SV48 走 4 级页表、SV39 走 3 级，冷 miss 成本应更高。
3. 启动横幅的 “SATP mode” 行会显示当前实际模式（运行时读 `satp`），**务必确认与预期一致**再采数。

> 属于改平台构建配置，建议单独分支进行并实测交叉验证（避免污染默认配置）。

### 6.2 4K vs 2M 大页（可选，需谨慎）

“同一块内存用 4K 页 vs 2M 大页映射”的对比，需要自建页表并改写映射，属高风险改动，**本方案不默认提供代码**。稳健的替代做法：`perf_lat`/`perf_tlb`(A) 曲线的 TLB 拐点位置本身就反映了当前页粒度下的 TLB reach——若将测试 buffer 改为大页映射，拐点会显著右移。真要做，请在有真机、可单步验证 MMU 的条件下单独实现，并先用 (A) 子测验证拐点变化。

---

## 7. 结果解读与 A53 横向对比

### 7.1 先看 instret（可选自检）

若你扩展了 instret 输出：同一测试项跨数据点的 instret 应一致（延迟型差异不改变指令数）。不一致说明对比失效，先排查。

### 7.2 逐项对比（把 `ns_per_ghz` 列对到 A53 的 cycle 列）

**延迟曲线（`perf_lat`）**：把每个 size 的 `ns_per_ghz` 与 A53 表同 size 的 cycle 比。示例（真机数据需回填）：

| size | A53 (cyc) | C908 ns/GHz（实测回填） | 解读 |
|------|-----------|------------------------|------|
| 32K  | 3         | ?                      | 都应落在各自 L1 延迟 |
| 256K | 16        | ?                      | A53 已进 L2；看 C908 L2 拐点位置 |
| 1M   | 17 (+70ns)| ?                      | 进 DRAM |
| 4M   | 25 (+114ns)| ?                     | A53 此处 +22 周期=L2 TLB miss；看 C908 是否同样出现台阶 |

**TLB（`perf_tlb` A）**：找 `cyc_per_access` 出现第一个明显台阶的页数：
- 约 17 页（68KB）附近抬升 → L1 D-TLB 容量（对标 A53 的 10 项 Micro-TLB）。
- 约 512/1024 页（2M/4M）附近再抬升 → L2 TLB 容量（对标 A53 512 项）。

**TWU（`perf_tlb` C）**：`twu_delta` = 一次完整页表遍历的周期。对标 A53 “L2 TLB miss +22 cyc / page walk to RAM”。

**带宽（`perf_bw`）**：`rd_MBps`/`wr_MBps` 对标 A53 RAM 读 ~2000MB/s、写 ~3700MB/s；小 size 时反映 L1/L2 带宽。

**分支（`perf_branch`）**：`est_mispredict_penalty_cyc` 对标 A53 的 7 周期。

### 7.3 结论怎么写（参考团队报告结构）

建议按团队报告模板组织（版本历史 / 术语 / 概述 / 测试环境 / 对比数据总表 / 主要结论 / 逐项分析 / 附录）。每个结论都应给出：**指标形态**（周期涨还是指令涨）、**归因**（TLB？cache？分支？）、**证据**（哪条 CSV 数据）。示例结论句式：

> “在 4M 工作集，C908 的 ns/GHz 相比 256K 抬升 N（周期），与 `perf_tlb`(A) 在 ~512 页出现的台阶一致，归因为超出 L2 TLB reach、每次访问触发 TWU 遍历——与 A53 在 4M 出现的 +22 周期 L2 TLB miss 同源。”

---

## 8. 风险与注意事项（务必先读）

1. **计数器可读性**：若运行 `perf_*` 立即触发 illegal instruction，说明 OpenSBI 未对 S 态开放 `cycle/instret/time`（`mcounteren`）。K230 OpenSBI 默认开放；若不行需在 OpenSBI 侧打开。
2. **QEMU 不可信**：K230/玄铁 QEMU 通常不建模 Cache/TLB 时序，`cycle ≈ instret`，延迟曲线会是“平的”。**延迟/带宽/TWU 数据只能在真机采**；QEMU 仅用于验证代码能跑、CSV 格式正确。
3. **主频核对**：以横幅打印的 derived CPU freq 为准换算 ns；若与预期主频偏差大，先查 `PERF_TIMEBASE_HZ` 是否等于该板 `time` CSR 频率。
4. **堆内存**：`Max working-set` 不要超过 RT-Smart 堆（默认 32MB）的一半；默认 8MB 安全。
5. **关中断时长**：延迟类窗口已控制在亚毫秒；不要盲目调大 `LAT_INNER`/`TLBA_INNER`，否则关中断过久可能丢 tick。
6. **A53 数据来源差异**：A53 参考取自 Amlogic S905 + DDR3，与 K230 的 DDR/频率不同；跨平台绝对值本就有差异，**重点比“形状/拐点/微架构效率（ns/GHz）”，而非绝对 ns**。

---

## 9. 附：CSV 字段速查

```
CSV,lat,<size_bytes>,<size>,<cyc_per_access>,<ns_per_access>,<ns_per_ghz>
CSV,bw,<size_bytes>,<size>,<rd_cyc_per_line>,<rd_MBps>,<wr_MBps>
CSV,tlb_stride,<pages>,<reach_bytes>,<cyc_per_access>,<ns_per_access>,<ns_per_ghz>
CSV,sfence,<all|page>,<cyc_per_op>,<ns_per_op>
CSV,walk,<hot|cold|twu_delta>,<cyc_per_page>,<ns_per_page>
CSV,branch,<metric>,<cycles>
```

采集：`grep '^CSV,' run.log > result.csv`，导入表格/报告模板即可。

---

## 10. 用 7-Benchmark 官方工具做权威对标（强烈推荐）

### 10.1 这套工具为何重要

`D:\Code\7bench1400` 是 **Igor Pavlov（7-Zip 作者 / 7-cpu.com 站长）的 7-Benchmark 14.00**，公有领域发布。**第 2 章里 A53 那张“延迟-容量”表就是用它测出来的**。用同一个工具测 C908，就从根上消除了“方法论不同”这个最大的对比风险。

包含：

| 文件 | 作用 |
|------|------|
| `MemLat64.exe` / `MemLat.txt` | Cache/TLB/RAM 的延迟与带宽（**核心**）|
| `PipeLen64.exe` / `PipeLen.txt` | 流水线长度 = 分支误预测惩罚 |
| `timer64.exe` | 通用计时器 |
| `7zr64.exe` | 7-Zip 压缩基准（综合整数性能）|
| `src/` | 全部源码，**带 `makefile.gcc` 且支持 `CROSS_COMPILE`** |
| `test.bat` | 一键跑全量（约 30–60 分钟）|

> 关键事实：包里只有 `Walk.x86.asm` / `pl.x86.asm` 两个 x86 汇编，**ARM 和 RISC-V 走的都是纯 C 路径**（`Walk.c` / `pl2.c`）。所以 C908（C）vs A53（C）vs U74（C）**三者同口径，可直接比**。

### 10.2 命令 → 7-cpu 表格列的映射（最重要）

`p{NN}` 测试的输出列含义（来自 `MemLat.txt`）：

```
列 0 = 块大小 (KB)
列 1 = 单流随机访存延迟     ← 就是 7-cpu “Latency vs Size” 主表
列 2 = 2 路并行随机读平均
列 3 = 3 路并行随机读平均     ← “Parallel Random Read” 带宽数据
...
```

| 7-cpu 页上的数据项 | 产生它的命令 |
|------------------|--------------|
| “Latency vs Size”主表（4KB 页） | `memlat <Size> p6` 的**列 1**（p6 = 2^6 = 64B 行）|
| 同表（2MB 大页） | `memlat <Size> l p6`（`l` = large pages，仅 Windows）|
| `L1 latency = 3 cyc (simple access via pointer)` | `p6` 小 size 处的列 1（**默认直接指针链**）|
| `L1 latency = 5 cyc (complex, n = p[n])` | `p6 k`（`k` = 用整数索引链代替指针）|
| `L2 latency` / `RAM latency` | `p6` 在 L2 尺寸 / 远大于 LLC 尺寸处的列 1 |
| TLB 项数、TLB miss penalty | `memlat <Size> h40h s2`（h + 64B=0x40 行）与 `p12`（4KB 步进）|
| `L1 B/W (Parallel Random Read)` | `p6` 的列 2/3/4...（多流并行）|
| `L2->L1 B/W (Read, 64 bytes step)` | `memlat <Size> b` |
| `... (Read, 64B step - pointer chasing)` | `memlat <Size> c` |
| `Write B/W (skip, 64 Bytes step)` | `memlat <Size> b w` |
| 非对齐 / range cross penalty | `memlat <Size> a` |
| `Branch misprediction penalty` | `pipelen r` |
| 7-Zip Benchmark 表 | `7zr64 b` |

常用选项速查（`memlat <Size>[K|M|G] <cmd>...`）：

```
p{N}   步进 = 2^N 字节：p6=64B(cache 行) p12=4KB(TLB) p21=2MB
p      自动扇扫多种步进        k   整数索引链（complex n=p[n]）
b      带宽                    w   写模式（配合 b）
c      顺序链的延迟+带宽        a   非对齐读
h{N}h  用顺序链测 TLB，{N}=行大小(16进制)
f{N}   指定主频 MHz            n   结果用 ns    nt  结果用计时器 ticks
i{N}   迭代次数(默认5)         e{N} 测试个数   s{N} 尺寸子级别[0,5]
l      2MB 大页（仅 Windows）  u   非 cached 模式  t{N} 绑定 CPU
```

### 10.3 第一步：先在 Windows x86 上跑一遍（熟悉输出格式）

不用上板，先在你自己的 PC 上看懂输出长什么样：

```powershell
cd D:\Code\7bench1400
.\MemLat64.exe 1024 p6 > p6.txt      # 延迟-容量主表
.\MemLat64.exe 1024 p12 > p12.txt    # 4KB 步进 → TLB
.\MemLat64.exe 64 b > b.txt          # 读带宽
.\PipeLen64.exe r > plen.txt         # 分支误预测
```

把 `p6.txt` 的列 1 跟你本机 CPU 在 7-cpu.com 上的页面对一下，能对上就说明你已经会读这个输出了。

> 跑全量：以**管理员**运行 `test.bat`（大页测试需要），结果输出到 `res\`。跑前关闭其他程序、BIOS 里关 Turbo。

### 10.4 第二步：使用已抽离的 K230 独立版（已实测编译）

已经在仓库外层建立独立目录 `d:\Code\rt-thread_fork_cgh\memlat-k230`，其中只保留 MemLat 必需的 5 个源码文件：

```text
memlat-k230/
├── Makefile
├── README.md
├── bin/memlat.elf
└── src/
    ├── Benchmark.h
    ├── MemLat.cpp
    ├── MyVersion.h
    ├── Walk.c
    └── Walk.h
```

原版 7-Benchmark 14.00 的 `Benchmark.h` 尚未识别 RISC-V，会退化为微秒级 `gettimeofday()`，不足以测量 Cache/TLB。本独立版只增加了一个 RISC-V 适配：用 `fence` + `rdcycle` 读取硬件周期；测试算法和输出格式保持原样。

重新构建：

```bash
export PATH=$HOME/tools/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin:$PATH
cd /mnt/d/Code/rt-thread_fork_cgh/memlat-k230
make clean all
file bin/memlat.elf
```

已验证生成：

```text
ELF 64-bit LSB executable, UCB RISC-V, RVC, soft-float ABI, statically linked
Type: EXEC, Entry point address: 0x200000000 (与官方 smart_ipc.elf 同口径)
SHA-256 = 42424d00a57bdc5b90bb381af2001c9d9217e42789b587d29345c4126f24dbfe  (memlat.elf)
SHA-256 = a120fb1b47e7d131a8d2702ac4030b0ca123472754a3b2ae04418b4142f6c50a  (memlat-rdcycle.elf)
```

这里使用 K230 README 的 GCC 10.1 musl、`-march=rv64gc -mabi=lp64 -mcmodel=medany -O2 -static` + SDK userapps 标准 `link.lds`（基址 0x200000000）。静态链接避免依赖板端 `/lib/ld-musl-riscv64*.so.1`；基础测试代码里已反汇编确认存在 `rdcycle`。完整构建与部署说明见 `memlat-k230/README.md`。

> **重要**：K230 LWP 加载器校验 EXEC 的 `e_entry` 必须严格等于 `USER_LOAD_VADDR(0x200000000)`。普通 musl `-static` 产物有两道门：（1）玄铁 `crt1.o` 的 `_start` 在无 `SHF_ALLOC` 的 `.start` 段 → `entry=0x0` 报 `ELF loading failed with result -1`；（2）默认 medlow 代码模型与 0x10000 基址 → 报 `elf entry is invalidate, entry:0x...!`（随后的 `command not found` 都是误导，实为装载校验失败）。memlat-k230 已用 `-mcmodel=medany` + 官方 `link.lds` + `-nostartfiles` 显式 CRT 解决。**任何新引入的用户态 ELF 拷卡前都必须先 `readelf -h` 确认 Entry point == 0x200000000**。

### 10.5 第三步：复制到当前官方 SDK 基础卡

当前官方 SDK 固件已经正常启动，不需要替换内核。只需把用户态 ELF 放入 SD 卡的数据分区：

1. K230 完全断电，取出 SD 卡，插到 Windows 读卡器。
2. 若读卡器仍经 usbipd 连接 WSL，先执行 `usbipd detach --busid <实际BUSID>`。
3. Windows 会出现两个 FAT 分区。打开约 1 GB、根目录中含 `app` 文件夹的分区（通常是 `F:`，以实际盘符为准）。
4. PowerShell 执行：

```powershell
New-Item -ItemType Directory -Force F:\bench
Copy-Item D:\Code\rt-thread_fork_cgh\memlat-k230\bin\memlat.elf F:\bench\memlat.elf
Get-Item F:\bench\memlat.elf
```

5. 为避免默认 SmartIPC 占用 CPU、内存、DDR 和 Cache，建议暂时禁用其自动启动文件（测试结束再恢复）：

```powershell
Rename-Item F:\app\examples\integrated_poc\smart_ipc.elf smart_ipc.elf.disabled
# 测试完成后恢复：
# Rename-Item F:\app\examples\integrated_poc\smart_ipc.elf.disabled smart_ipc.elf
```

若路径不同，在两个分区中查找 `app\examples\integrated_poc\smart_ipc.elf`。安全弹出 SD 卡，插回 K230 并启动。

### 10.6 第四步：板端兼容性门禁

进入 `msh />` 后先确认路径：

```text
ls /sdcard
ls /sdcard/bench
/sdcard/bench/memlat.elf
```

不带参数运行应打印 `MemLat64 14.00` 和帮助信息后返回 msh。只有这一步成功，才继续性能测试。

- `not found`：路径/分区不对，执行 `mount`、`ls /` 查实际挂载点。
- `Permission denied`：尝试 `chmod 755 /sdcard/bench/memlat.elf`；FAT 通常不需要权限位。
- `illegal instruction`：当前固件没有向 U 态开放 `cycle` CSR（`scounteren.CY`），需要换用内核态 `perf_*` 或修改固件；不要把失败结果当性能数据。
- 异常退出/缺 syscall：说明官方 SDK 用户态 ABI 与该静态 musl 程序不兼容，请保留完整异常日志再适配。

若此前没有在 SD 卡上重命名 SmartIPC，先停止后台负载：

```text
list_process
killall smart_ipc.elf
list_process
```

如进程名不同，以 `list_process` 输出为准，也可 `kill <PID>`。等待后台输出停止、静置 5 秒再测。

### 10.7 第五步：按从小到大的顺序运行

先做 4 MB 冒烟测试，避免一上来申请大量内存：

```text
/sdcard/bench/memlat.elf 4m p6 f1600 i1 s0 n
```

成功返回 msh 后，再执行正式测试（**注意尺寸与主频参数**）：

```sh
/sdcard/bench/memlat.elf 64m p6 f1600 i5 s2       # 延迟-容量主表（对标用）
/sdcard/bench/memlat.elf 64m p6 k f1600 i5 s2     # complex (n = p[n]) 形式
/sdcard/bench/memlat.elf 64m p12 f1600 i5 s2      # 4KB 步进 → TLB
/sdcard/bench/memlat.elf 64m h40h s2 f1600 i5     # TLB，64B 行
/sdcard/bench/memlat.elf 64m b f1600 i5            # 读带宽
/sdcard/bench/memlat.elf 64m b w f1600 i5          # 写带宽
/sdcard/bench/memlat.elf 64m c f1600 i5            # 指针追逐带宽
/sdcard/bench/memlat.elf 64m a f1600 i5            # 非对齐
```

**必须注意的四个点（移植风险）**：

1. **尺寸别用 1024**。`test.bat` 里的 `1024`（MB）适用于 PC。K230 只有 512MB（RT-Smart 区 256MB、MMZ 256MB），**用 `64` 或 `128`**。要看 L2 TLB 拐点，64MB 已远超 C908 的 4MB TLB reach，够用。
2. **用 `f{N}` 钉住主频**。启动时 `GetStartFreqs()` 会自旋测频，而 RT-Smart 上 `clock()` 的分辨率可能很粗（tick 级）导致测频失真。直接 `f1600` 指定，并用我们的 `perf_all` 横幅里的 *derived CPU freq* 校对。
3. **交叉验证**。同时跑 `nt`（原始 ticks）与 `n`（ns），并拿 `memlat ... p6` 的列 1 与本套件 `perf_lat` 的 `ns_per_ghz` 对比。**两者趋势与拐点应一致**；差异大就说明计时源有问题，以内核态 `rdcycle` 的结果为准。
4. **`l`（2MB 大页）在 RT-Smart 上用不了** —— 它走 Windows 的 large-page API（`ITS_WINDOWS` + `LARGE_PAGES`）。想对比大页，用第 6 章的方法。

另：`t{N}`（绑核）在单核配置下无意义；`u`（uncached）依赖平台特性，谨慎使用。

### 10.8 三方结构对比（预测拐点位置）

U74 是 RISC-V + SV39 + 4K/2M/1G 页，比 A53 更贴近 C908，**建议作为第二参照同时列入报告**（数据源：`D:\Ref\Doc\SiFive U74.html`）：

| 项目 | **C908 (K230)** | SiFive U74 | ARM A53 |
|------|-----------------|-----------|---------|
| 主频 | ~1.6 GHz | 1.2 GHz | 1.536 GHz |
| L1D | 32KB（待确认） | 32KB / 8-way | 32KB / 4-way |
| L2 | 待确认 | 2MB / 16-way | 512KB / 16-way |
| Cache line | 64B | 64B | 64B |
| **L1 D-TLB (4K)** | **17，全相联** | 40，全相联 | 10 |
| **L2 TLB** | **512 或 1024，4 路** | 512，direct-map | 512，4 路 |
| L1 延迟（指针） | 待测 | 3 cyc | 3 cyc |
| L1 延迟（n=p[n]） | 待测 | 5 cyc | 3 cyc |
| L2 延迟 | 待测 | 26 cyc | 15 cyc |
| RAM 延迟 | 待测 | 26 cyc + 145 ns | 15 cyc + 128 ns |
| L1 TLB miss penalty | 待测 | 11 cyc | 2 cyc |
| L2 TLB miss penalty | 待测 | 24 cyc | 11–22 cyc |
| 分支误预测惩罚 | 待测 | 4 cyc | 7 cyc |
| RAM 读带宽 | 待测 | 430–464 MB/s | ~2000 MB/s |

**由 TLB 项数推算出的拐点位置（以 4K 页计）**——这是你看曲线时的预期：

| | L1 D-TLB reach | L2 TLB reach |
|---|---------------|--------------|
| **C908** | 17 x 4K = **68 KB** | 512 x 4K = **2 MB**（或1024 → 4MB）|
| U74 | 40 x 4K = 160 KB | 512 x 4K = 2 MB |
| A53 | 10 x 4K = 40 KB | 512 x 4K = 2 MB |

所以 **C908 的第一个 TLB 拐点应出现在 ~68KB**（比 U74 早、比 A53 晚），第二个在 2–4MB。`perf_tlb`(A) 的页数扇扫与 `memlat p12` 应该都能看到这两个台阶——**两个工具互相印证，才能定论**。

### 10.9 与本套件（`perf_*`）的分工

两者不是重复，而是互补，建议**都跑**：

| | 7-Benchmark（用户态） | 本套件 `perf_*`（内核态）|
|---|---------------------|---------------------------|
| 优势 | **与 7-cpu.com 口径完全一致**，可直接对表；多流并行、非对齐等测项齐全 | 能用 `sfence.vma` 做**冷/热 TLB 分离**、直接读 `rdcycle/instret`、关中断隔离噪声 |
| 局限 | 计时靠 `clock()`，无法刷 TLB，无法读特权计数器 | 自研代码，与 7-cpu 口径靠“方法论对齐”而非同一份代码 |
| 用途 | 对外报告的权威对比数据 | TWU/sfence/页表深度等**内核级机理分析** |

推荐流程：先 `perf_all` 拿到内核态全景与 TWU 细节 → 再跑 `memlat p6/p12` 拿到与 A53/U74 同口径的对比数据 → 两者拐点一致则结论可信。
