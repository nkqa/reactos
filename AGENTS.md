# AGENTS.md — ReactOS AMD64 高内存与多核（SMP）支持

本文件是本仓库当前工作目标的唯一权威说明：**让 ReactOS AMD64 真正支持大容量物理内存（已验收到 64 GiB）与多核对称多处理（SMP）**。
所有后续 Agent 开工前必须先读本文，并按第 2 节的规则执行。

---

## 1. 项目定位与目标

| 项 | 内容 |
| --- | --- |
| 主仓库 | `D:\repos\reactos`（远程 `origin` = `https://github.com/nkqa/reactos`） |
| 官方上游副本 | `D:\repos\reactos\upstream-reactos`（独立克隆，只读同步用） |
| AMD64 构建目录 | `D:\reactos-build-amd64-344ebfe594` |
| 测试工具目录（工作区外） | `C:\Users\abc\AppData\Local\Temp\reactos-amd64-tools-20261118` |
| 目标架构 | `ARCH=amd64`，MSVC + Ninja + ml64 |

### 1.1 能力目标（允许并明确要求的能力）

1. **高内存**：AMD64 客户机在真实 65536 MiB RAM 下完成启动、桌面、内存 API 对账、高 PFN 写读、受控压力与正常关机。
2. **多核**：AMD64 在 `-smp N`（先 2 核，再 4/8 核）下启动 AP，每核拥有独立 PCR/PRCB/GDT/IDT/TSS/内核栈与 idle 线程，并具备跨核 IPI 与 TLB shootdown。
3. **组合**：先「单核 + 高内存」，再「多核 + 常规内存」，最后才允许「多核 + 高内存」组合回归。不得把某一项通过冒充全部通过。

> 注意：`MM_MAX_PAGE` 提高的是**物理地址窗口**，不是宣称支持该容量的 RAM 总量。文档与提交信息都必须区分这两者。

---

## 2. 硬性规则（必须遵守）

### 2.1 上游隔离同步
- 只允许在 `upstream-reactos/` 内 `git pull --ff-only`；**禁止**在主仓库执行上游 fetch/pull/merge/rebase/reset/切分支/改远程。
- 上游副本必须保持干净；发现本地改动或额外提交立即停止并报告。
- 详见 [.trae/rules/REACTOS_SYNC.md](file:///d:/repos/reactos/.trae/rules/REACTOS_SYNC.md)。

### 2.2 构建
- 必须先进入 VS 开发环境，再用 CMake 构建；`.ps1` 受执行策略限制，统一用 `Launch-VsDevShell.ps1` + `cmake.exe`。
- 修改 `CMakeLists.txt` 后必须重新 configure，不能只 build。
- 判定「构建成功」的标准是目标产物存在且时间/大小更新（如 `bootcd.iso`、`ntkrnlmp.exe`、`halmp.dll`），而不是「最后一条命令返回 0」。

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
& "C:\Program Files\CMake\bin\cmake.exe" --build "D:\reactos-build-amd64-344ebfe594" --target bootcd --parallel 4
```

### 2.3 Git 提交
- 每个**可独立验收的源码修复单元**完成后立即创建独立本地提交。
- **不 push、不 amend、不 force、不 `git add .` / `git add -A`**；只按文件名精确暂存本次相关文件。
- 工作区存在历史遗留修改（`hal/halx86/generic/dma.c`、`ntoskrnl/mm/amd64/init.c`、`modules/CMakeLists.txt`、`.gitignore` 等），不得混入 SMP 提交，也不得回退。
- 没有源码变化的重复测试不制造空提交。

### 2.4 测试环境与实时可视化
- QEMU 可执行文件：`D:\Program Files\qemu\qemu-system-x86_64.exe`。
- **用户要求能实时看到 QEMU 画面：启动时必须开窗口**，用 `-display gtk`（不可用时退回 `-display sdl`），禁止 `-display none`。
- 自动化操作仍通过 QMP 进行：`-qmp tcp:127.0.0.1:<PORT>,server=on,wait=off`。
- 端口避开 80/443/8080/8443/8888，使用高位端口（建议 4460 起）。
- 日志、工具、临时镜像一律放工作区外，不污染仓库。
- 需要人工确认的高风险操作（改分页文件、重启宿主机、终止用户其它 VM）必须先询问。

### 2.5 证据与措辞
- 结论必须基于实测证据（符号化栈、寄存器、序列化 QMP 截图、内核打印、API 返回值）。
- 不下「已经支持 SMP」这类未验证结论；未跑通的项标注为未完成。

---

## 3. 当前进度

### 3.1 已完成并通过验收

| 阶段 | 内容 | 结果 | 提交 |
| --- | --- | --- | --- |
| 高内存 1 | Freeldr `MM_MAX_PAGE` 64 GiB 支持 | 64 GiB 单核进入桌面 | `b722cdc5209` |
| 高内存 2 | ATAPI 恢复路径未初始化变量 | 正常关机 `SHUTDOWN guest=true` | `827859e01e1` |
| SMP 1 | AP 启动链三文件从空实现补到可编译 | `bootcd.iso` 构建成功 | `54c288190a8` |

**64 GiB 单核核心证据**
- `MmNumberOfPhysicalPages = 16777074`（约 63.96 GiB，含 PCI 孔洞）
- `MmHighestPhysicalPage = 17039359`（> 65 GiB，故需要 `MM_MAX_PAGE = 0x2000000`）
- 64 GiB 高 PFN 写读通过；`BugCheck = 0`；`KdEnteredDebugger = 0`

### 3.2 已完成但**未验证运行时**

`54c288190a8` 只证明**编译与链接**通过，AP 能否真正上线尚未验证。第 5 节列出的阻塞项未修完之前，不得声称 SMP 可用。

### 3.2.1 2026-11-19 调试进展（重要）

`-smp 2` 的第一个真正里程碑，已从「一启动就 STOP 0x50 蓝屏」推进到「系统正常启动、AP 执行到开启保护模式」。

已修复的真实缺陷（见 6.7）：

| # | 缺陷 | 后果 |
| --- | --- | --- |
| 1 | `HalpLowStub` 是 `0xFFFFFFFFFFD05000` 垃圾值 | HAL 写坏地址，STOP 0x50 |
| 2 | 汇编硬编码 `KPROCESSOR_STATE` 的 ContextFrame 偏移 | AP 的 RSP 变成垃圾，握手永不完成 |
| 3 | 16 位实模式阶段写 CR3/CR4/EFER | 实模式 #GP，AP 立刻 triple fault |
| 4 | `PtrOffset` 传 `PVOID` 变量而未取地址 | patch 偏移为垃圾，写坏内存 |
| 5 | `HalpAPEntry16End` 放在 32/64 位段之前 | 32/64 位代码根本没被拷进低地址桩 |

**当前卡点**：AP 串口进度为 `01y`（已执行到开保护模式成功），随后的 16 位远跳 `EA`/`66 EA` 到 32 位段会触发 `#GP → 双重故障 → triple fault`。远跳立即数（`0x6B0E8:0x0008`）与 minimal GDT 内容均已逐字节核对无误，怀疑是 SIPI 后 CS 的默认操作数大小与预期不同，需要下一步用 KD 或指令级追踪确认。

**诊断手段（已内置，验证后应清理）**：

- `spinup.c` 的 `HalpSerialString`/`HalpSerialHex` 把关键指针打印到 COM1（QEMU `-serial file:` 可读）。
- `apentry.S` 在关键点 `out 0x3F8` 输出单字符进度：`0`(首条指令) `1`(lgdt 后) `y`(开 PE 成功) `w`(进 32 位段) `a`..`d`(CR4/CR3/EFER/开分页) `2`(32 位完成) `3`..`5`(64 位各阶段)。
- QEMU 侧用 `-d int,guest_errors -D qemu.log` 抓 `check_exception` 与 vCPU 号，用 QMP `human-monitor-command` 执行 `info registers` / `xp /Nxb <phys>` 读取内存与寄存器。
- 注意：`-no-shutdown` 会让蓝屏/停机状态保留，便于事后取证；`0x60000` 是 Freeldr 数据区，不能当调试标记用。

### 3.3 未开始
- IPI 通用调用（`KeIpiGenericCall`）
- 跨核 TLB shootdown
- `-smp 2` LiveCD 端到端验收
- 4/8 核扩展与高内存组合回归

---

## 4. 架构与关键常量

### 4.1 SMP 启动链三文件职责

| 文件 | 职责 |
| --- | --- |
| [mproc.c](file:///d:/repos/reactos/ntoskrnl/ke/amd64/mproc.c) | `KeStartAllProcessors`：分配每核 `APINFO`（GDT/IDT/NMI 栈/PCR/ETHREAD/TSS×3）、克隆 BSP 的 GDT/IDT、patch TSS/PCR 描述符、填 `KPROCESSOR_STATE`、写 `KeLoaderBlock`、调 HAL、轮询握手 |
| [spinup.c](file:///d:/repos/reactos/hal/halx86/smp/amd64/spinup.c) | `HalStartNextProcessor`：构造临时 PAE 页表、把 AP entry 拷到 `HalpLowStub`（<1 MiB）、运行时 patch 跳转偏移与页表根、发 INIT+双 SIPI |
| [apentry.S](file:///d:/repos/reactos/hal/halx86/smp/amd64/apentry.S) | AP 侧 16 位实模式 → 32 位保护模式 → 64 位长模式（手写机器码 + 原生 MASM64） |

### 4.2 低地址桩内存布局（`HalpLowStub`）

`HALP_LOW_STUB_SIZE_IN_PAGES = 5`（仅 AMD64，共 **20 KiB**），见 [halp.h](file:///d:/repos/reactos/hal/halx86/include/halp.h#L64-L68)。

| 偏移 | 用途 |
| --- | --- |
| `+0x000` | AP entry code（16/32/64 位三段） |
| 其后 | 临时 PAE 页表（PDPT + PD） |
| 其余 | 保留 |

> **必须保证页表总占用不超过剩余 4 页**，否则会踩坏 `HalpLowStub` 之后的内存。

### 4.3 apentry.S 内部布局（偏移自 `HalpAPEntry16`）

| 偏移 | 内容 |
| --- | --- |
| `+0x00` | 16 位实模式代码（cli / 平铺段 / 算 flat base / 装 CR3,CR4,EFER / lgdt / 开 PE / 远跳） |
| `+0x30` | `HalpAPEntryData`：`PageTableRoot32`(8) / `PaeCr4`(4) / `EferFlags`(4) / `PageTableRoot64`(8) / `ProcessorState`(8) / `FlatBase`(8) |
| `+0x58` | 16 位远跳 offset（32 位，HAL 运行时写入） |
| `+0x5C` | 16 位远跳 selector（HAL 写 `0x08`） |
| `+0x60` | `MinimalGdtDesc`：`.short limit=0x2F` + `.long base`（HAL 写低 32 位） |
| `+0x68` | `MinimalGdtEntries`：null / 32 位代码 / 32 位数据 / 64 位代码 / 64 位数据 / 保留 |
| `+0x98` | `HalpAPEntry32`（32 位保护模式段） |
| 其后 | `HalpAPEntry64`（64 位长模式段） |

### 4.4 `KPROCESSOR_STATE` 关键偏移（AMD64）

```
SpecialRegisters（起点 +0x000）
  Cr0  +0x00   Cr2 +0x08   Cr3 +0x10   Cr4 +0x18
  Gdtr +0x50  → Limit +0x56, Base +0x58
  Idtr +0x60  → Limit +0x66, Base +0x68
  Tr   +0x70（USHORT）
ContextFrame（AMD64_CONTEXT，起点 +0x0E0）
  Rsp    +0x178
  Rip    +0x1D8
  EFlags +0x1F8
```

### 4.5 GDT 选择子（AMD64）

```
KGDT64_R0_CODE = 0x10    KGDT64_R0_DATA = 0x18
KGDT64_R3_DATA = 0x28    KGDT_R0_PCR    = 0x30（AMD64 ketypes.h 未定义，需自行补）
KGDT64_SYS_TSS = 0x40    KGDT64_R3_CMTEB = 0x50
```

`KIPCR.Prcb` 位于偏移 **0x180** 且是**值成员**（`KPRCB Prcb`），不是指针。

### 4.6 内核与 HAL 产物

- `ntkrnlmp.exe`（SMP 内核，`CONFIG_SMP`）与 `halmp.dll`（SMP HAL）均已 `add_cd_file` 到 `reactos/system32`。
- Freeldr **默认加载 UP 版** `ntoskrnl.exe` + `hal.dll`；要跑 SMP 必须在 `freeldr.ini` 指定 `KERNEL=ntkrnlmp.exe HAL=halmp.dll`。
- 默认选择逻辑在 [winldr.c](file:///d:/repos/reactos/boot/freeldr/freeldr/ntldr/winldr.c#L837-L855)。

---

## 5. 待办工作清单（按优先级）

### P0 — 让 AP 真正能上线（阻塞项）

1. **设置 AP 的 `MSR_GS_BASE` / `MSR_GS_SWAP`**
   - [KiSystemStartupBootStack](file:///d:/repos/reactos/ntoskrnl/ke/amd64/krnlinit.c#L138-L145) 第一句就是 `KeGetCurrentPrcb()`（走 `gs:`），GS 基址不对立刻崩。
   - 现在 `apentry.S` 只做了 `mov gs, 0x2B`，未写 MSR。
   - 方案：`AP_ENTRY_DATA_AMD64` 增加 `Pcr`（VA）字段；64 位段用 `wrmsr` 写 `0xC0000101`/`0xC0000102`；HAL 填值。

2. **修正 `KeLoaderBlock->Thread` 与 idle 线程的 `ApcState.Process`**
   - `KiSystemStartupBootStack` 会读 `Thread->ApcState.Process` 并传给 `KiInitializeHandBuiltThread`，后者执行 `Process->ActiveProcessors |= ...`。
   - 当前 `mproc.c` 传 `&APInfo->Thread.Tcb`（零初始化，`ApcState.Process == NULL`）→ 必然空指针崩。
   - 方案：传 `&APInfo->Pcr.Prcb->IdleThread` 或显式把 `ApcState.Process` 设为系统进程。

3. **修正临时 PAE 页表占用溢出**
   - 现在用了 `HalpLowStub + 4K*1..4K*5`（第 5 页越界）。低地址桩只有 5 页。
   - 方案：只建 1 个 PDPT + 1~2 个 PD（覆盖 0–1 GiB 或 0–2 GiB 足够），保证 `code + PDPT + PD ≤ 5 页`。

4. **把硬编码 patch 偏移改为编译期推导**
   - `spinup.c` 里 `0x58/0x5C/0x62` 是硬编码，一旦 `apentry.S` 前段字节数变化就静默错位。
   - 方案：用 `PtrOffset(HalpAPEntry16, HalpAPEntryData)` 推导，并在 `static_assert` 或运行时 `ASSERT` 校验。

### P1 — SMP 功能补全

5. `ntoskrnl/ke/amd64/ipi.c`：去掉 `KeIpiGenericCall` 里的 `debugbreak`，实现跨核调用与完成屏障。
6. `ntoskrnl/ke/amd64/trap.S`：IPI 汇编入口（`int3` 占位）改为调用处理函数并回 EOI。
7. `ntoskrnl/ke/amd64/cpu.c`：`KeFlushAllProcessorsTb` 补 `AllProcessors` 分支，走 IPI shootdown + 屏障。

### P2 — 验收

8. `-smp 2` + 4 GiB 单核基线镜像，`freeldr.ini` 指定 SMP 内核/HAL，开窗口跑通桌面。
9. 用 KD 证明 CPU1 进入独立 PCR/PRCB 与 idle 循环；两条线程分别绑核持续执行。
10. 双向 IPI、跨核唤醒/抢占、共享映射变更、并发文件读写校验。
11. 4/8 核逐级扩展；最后做「多核 + 64 GiB」组合回归。

---

## 6. 踩过的坑（务必先看，避免重复）

### 6.1 高内存

| 坑 | 现象 | 结论 |
| --- | --- | --- |
| `MM_MAX_PAGE` 语义被误解 | 16/64 GiB 下高段内存被裁掉 | Freeldr 里它是**物理地址窗口上限**，不是 RAM 容量上限；64 GiB 需要 `0x2000000`（128 GiB 窗口） |
| 带孔洞的物理布局 | 64 GiB RAM 顶址超过 65 GiB | 必须按 `MmHighestPhysicalPage` 而非容量算窗口 |
| ATAPI 未初始化变量 | 关机时 `_RTC_UninitUse` 断点挂死 | `dev_error.c` 的 `NTSTATUS Status` 必须初始化为 `STATUS_SUCCESS` |
| 宿主机内存不足 | 65536 MiB 客户机无法启动 | 宿主机已配置 96 GiB 分页文件；客户机仍是真实 64 GiB，不用客户机 swap 凑 |

### 6.2 工具链（ml64 / MASM64）

ReactOS 的 CMake 对 `.S` 统一使用 **ml64.exe**，因此不能用 i386 那套写法：

| 坑 | 报错 | 正确做法 |
| --- | --- | --- |
| 使用 `asm.inc` 的 `.code16` / `.code32` 宏 | `A2008: syntax error : ASSUME / use16 / .586P`、`A2034: must be in segment block` | ml64 不支持段模式切换，16/32 位代码**全部手写机器码字节** |
| 使用 GAS 的 `.byte/.short/.long/.quad` | `A2008: syntax error : byte` | 用 MASM 的 `db/dw/dd/dq` |
| 定义以点开头的宏（如 `.byte MACRO`） | `A2008` / `A1008: unmatched macro nesting` | 不要用点前缀定义宏，直接用原生指令 |
| 同时出现 `.code` 和 `.code16` | 段嵌套错误 | 不要混用 |
| `.code64` 伪指令 | 不被识别 | MASM64 默认就是 64 位，省略即可 |
| `.586P` | 不被识别 | 删除，ml64 默认 64 位 + SSE |
| 16 位里取自身物理基址 | 无 | `movzx ebp, cs` + `shl ebp, 4`，之后用 `[ebp + 绝对偏移]` |
| 嵌套段里用 `OFFSET` | 展开异常 | 改为相对 `label - HalpAPEntryData` 或运行时 patch |

### 6.3 结构体与符号

| 坑 | 现象 | 正确做法 |
| --- | --- | --- |
| `KIPCR.Prcb` 当成指针 | `C2232: 左操作数有 struct 类型，使用 "."` | `&Pcr.Prcb.ProcessorState`（值成员，偏移 0x180） |
| `KDESCRIPTOR.Base` 当成 `ULONG64` | `C4047: PVOID 与 ULONG64 间接级别不同` | 是 `PVOID`，赋值要强转 |
| AMD64 缺 GDT 选择子 | `C2065: KGDT_R0_PCR / KGDT64_DF_TSS / KGDT64_NMI_TSS 未声明` | AMD64 `ketypes.h` 只定义了 0x10/0x18/0x28/0x40，其余需 `#ifndef` 自行补充 |
| 找 `KeInitialProcess` | 不存在 | AMD64 是 `KiInitialProcess` / `PsInitialSystemProcess` |
| `KiGetGdtEntry64` / `KiSetGdtEntry64Base` | 不存在 | 用 [intrin_i.h](file:///d:/repos/reactos/ntoskrnl/include/internal/amd64/intrin_i.h) 的 `KiGetGdtEntry`（返回 `PKGDTENTRY64`）与 `KiSetGdtDescriptorBase` |
| `KiInitializeProcessorBootStructures` 未声明 | 隐式声明/链接失败 | 参数顺序 `(ProcessorNumber, Pcr, GdtBase, IdtBase, TssBase, IdleThread, KernelStack, DpcStack, DoubleFaultStack, NmiStack)`，需自行前置声明 |
| `KiSystemStartupBootStack` 只在 i386 头声明 | 找不到 | AMD64 需自行 `CODE_SEG("INIT") DECLSPEC_NORETURN VOID NTAPI` 前置声明 |
| `LOADER_PARAMETER_BLOCK` 字段类型 | `C4047` | `KernelStack/Prcb/Thread/Process` 都是 `ULONG_PTR` |

### 6.4 链接

| 坑 | 报错 | 结论 |
| --- | --- | --- |
| 汇编里 `dd HalpAPEntry64` 做 32 位绝对重定位 | `LNK2017: 没有 /LARGEADDRESSAWARE:NO，"ADDR32" 到 "HalpAPEntry64" 的重定位无效` | x64 `/DLL` 目标不能有 ADDR32 重定位 |
| 试图加 `/LARGEADDRESSAWARE:NO` | `LNK4075: 忽略 /LARGEADDRESSAWARE:NO（由于 /DLL 规范）` | 该开关对 DLL 无效，白费 |
| 试图用 MASM64 的 `ADDR32` 修饰符 | `A2206: missing operator in expression` | MASM64 无此关键字 |
| **最终方案** | — | 汇编里跳转目标先写 `dd 0` 占位，由 HAL 在 `HalpPatchApEntryAmd64` 运行时按 `HalpLowStub` 物理地址 patch |

### 6.5 运行时设计缺陷（当前已知未修，见第 5 节）

1. **AP 的 GS 基址未设置** → `KeGetCurrentPrcb()` 读到错误 PRCB。
2. **`LoaderBlock->Thread` 指向未初始化 ETHREAD** → `ApcState.Process == NULL` 崩溃。
3. **临时页表越界 1 页**（低地址桩只有 5 页）。
4. **patch 偏移硬编码**，与汇编布局耦合，易静默错位。
5. `mproc.c` 的失败清理路径会释放「最后一个失败 AP」的结构，但循环变量与 `APInfo` 可能已被后续迭代覆盖，需复核。

### 6.7 2026-11-19 新确认的坑（都已踩过）

| 坑 | 现象 | 结论 |
| --- | --- | --- |
| `HalpMapPhysicalMemory64` 在 AMD64 早期不可用 | `HalpLowStub` 得到 `0xFFFFFFFFFFD05000`（= `MM_HAL_VA_START`+1MB+0x5000），写它直接 STOP 0x50 | 该函数从 `HalpHeapStart` 找空闲 PTE，AMD64 的 `MM_HAL_VA_START` 区域并未真正映射；本阶段不要依赖它 |
| `HALP_LOW_STUB_SIZE_IN_PAGES` 调大并不能解决 | 改成 16 页仍返回同一垃圾值 | 与页数无关，是映射机制问题；保持 5 页并把临时页表压到 4 页内 |
| `KPROCESSOR_STATE` 的 `ContextFrame` 偏移不能硬编码 | AP 的 RSP 变成 `0xE478`，握手永不完成 | `KSPECIAL_REGISTERS` 还含 `DebugControl`/`MsrGsBase`/`MsrStar` 等字段，不是 0xE0；改用「HAL 用 `FIELD_OFFSET` 取值填入 `AP_ENTRY_DATA_AMD64`，汇编只读值」 |
| 实模式不能访问 CR3/CR4/EFER | AP 输出 '0'、'1' 后 triple fault | CR 寄存器只能在保护模式写；把 CR4→CR3→EFER→CR0(PG) 全部移到 32 位阶段 |
| `PtrOffset(HalpAPEntry16, HalpAPEntryData)` | patch 偏移为垃圾，写坏内存 | 这些符号是代码标签，必须写成 `PtrOffset(&HalpAPEntry16, &HalpAPEntryData)`；`extern PVOID` 的变量值是代码字节 |
| `HalpAPEntry16End` 位置 | 32/64 位代码没被拷贝 | 该标签必须放在整段最后（含 `HalpFullGdtDesc`/`HalpFullIdtDesc`） |
| `EferFlags` 值 | 曾误写成 MSR 编号 `0xC0000080` | 它是 EFER 的值：`LME(0x400) | SCE(0x800) = 0xC00` |
| `-D qemu.log` 的 `check_exception` 不含 vCPU 号 | 难判断是谁 fault | 结合 `0x8 → 0xd`（双重故障升级 #GP）判定为 triple fault；逐字符串口进度更可靠 |
| **未解决**：16 位远跳到 32 位段 | `01y` 之后 triple fault | 立即数与 GDT 已逐字节核对正确，怀疑 SIPI 后 CS 的默认操作数大小（D 位）与假设不符；`EA` / `66 EA` 两种编码都失败，需指令级追踪 |

### 6.6 其它

- `upstream-reactos` 的 AMD64 SMP 同样是**空实现**，不能指望「同步上游就修好了」。
- 修改 `hal/halx86/CMakeLists.txt` 会触发全量 configure，耗时较长，属正常。
- Git 提交需要仓库内 `user.name`/`user.email`（本仓库已设为 `nkqa`），否则 `Committer identity unknown`。

---

## 7. 验收方法

### 7.1 启动（开窗口，用户可实时观看）

```powershell
& "D:\Program Files\qemu\qemu-system-x86_64.exe" `
  -machine pc-i440fx-9.2 -accel tcg,thread=multi -cpu qemu64 `
  -m 4096 -smp 2 `
  -cdrom "D:\reactos-build-amd64-344ebfe594\bootcd.iso" -boot order=d `
  -display gtk `
  -qmp tcp:127.0.0.1:4460,server=on,wait=off `
  -serial "file:C:\Users\abc\AppData\Local\Temp\reactos-amd64-tools-20261118\smp2-serial.log" `
  -nic none -no-reboot -no-shutdown
```

高内存组合把 `-m` 换成 `65536`；核数验证把 `-smp` 换成 `4` / `8`。

### 7.2 SMP 内核选择
在 Freeldr 启动项里追加或修改：
```
KERNEL=ntkrnlmp.exe HAL=halmp.dll
```
若 ISO 内 `freeldr.ini` 未暴露该字段，需先确认 ISO 中 `reactos/system32/ntkrnlmp.exe` 与 `halmp.dll` 存在，再决定改 `boot.ini`/`freeldr.ini` 或临时替换。

### 7.3 验收项（逐项独立判定）

| 项 | 判据 |
| --- | --- |
| AP 上线 | KD 中 CPU1 的 `Prcb->Number == 1`，进入 `KiIdleLoop` |
| 独立 PCR | CPU1 的 `gs:[0]`/`Self` 指向自己的 PCR，GDT/IDT/TSS 与 CPU0 不同 |
| 绑核执行 | 两条线程分别 `KeSetAffinityThread` 到 0/1 持续累加校验 |
| IPI | 双向 `KeIpiGenericCall` 返回值正确、无超时 |
| TLB | 共享映射修改后另一核读到新值 |
| 并发 I/O | 双核并发读写文件校验一致 |
| 稳定性 | `BugCheck = 0`、`KdEnteredDebugger = 0`、可正常关机 |

---

## 8. 快速命令参考

```powershell
# 构建
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
& "C:\Program Files\CMake\bin\cmake.exe" --build "D:\reactos-build-amd64-344ebfe594" --target bootcd --parallel 4

# 只重配（改了 CMakeLists 后）
& "C:\Program Files\CMake\bin\cmake.exe" -S "D:\repos\reactos" -B "D:\reactos-build-amd64-344ebfe594" -G Ninja -DARCH=amd64

# 产物核对
Get-Item "D:\reactos-build-amd64-344ebfe594\bootcd.iso","D:\reactos-build-amd64-344ebfe594\ntoskrnl\ntkrnlmp\ntkrnlmp.exe","D:\reactos-build-amd64-344ebfe594\hal\halx86\halmp.dll" | Select-Object Name,Length,LastWriteTime

# 精确暂存并提交
git add hal/halx86/smp/amd64/apentry.S hal/halx86/smp/amd64/spinup.c ntoskrnl/ke/amd64/mproc.c
git commit -m "[SMP][AMD64] ..."
```

---

## 9. 交接提示

接手的 Agent 请按以下顺序推进，不要跳步：

1. 读本文第 5 节 P0 四项，先修「GS 基址」与「idle 线程 Process」，这是 AP 能否活下来的前提。
2. 修「页表越界」，否则会随机踩内存，掩盖真实问题。
3. 打开窗口跑 `-smp 2`，用 KD 观察是否真的进入两条执行流。
4. AP 能稳定上线后，再做 IPI 与 TLB shootdown。
5. 每一单元验收通过后立刻独立提交，不在未验证状态下批量堆改动。
