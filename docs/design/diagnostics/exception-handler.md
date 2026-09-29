# Exception Handler 增强设计

## 概述

`exception_handler()` 是 LaOS 内核的异常统一入口。当 CPU 触发异常(#DE， #GP, #PF， #UD 等)时，
该函数负责 dump 完整的寄存器状态、解析异常原因、进行栈回溯，然后 halt 系统。

本文档记录增强版本的设计思路和输出格式。

## 设计原则

**"一次异常输出足够定位根因"** -- 不需要重新编译、加日志、连调试器。

增强前的异常输出只有 RIP/RSP/RFLAGS/CS/SS + CR2/CR3/CR4 + 裸地址的 RBP 回溯。
这意味着每次遇到新异常，都需要重新编译内核加调试信息才能知道"崩溃时寄存器里有什么"、
"崩溃的是什么指令"、"在哪个函数的哪个偏移"。

增强后，所有这些信息一次性输出。

## 输出解剖

### 1. 寄存器全量 dump

```
[Registers]
  RAX=...  RBX=...  RCX=...  RDX=...
  RSI=...  RDI=...  RBP=...  RSP=...
  R8 =...  R9 =...  R10=...  R11=...
  R12=...  R13=...  R14=...  R15=...
  RIP=...  CS=...  RFLAGS=...  SS=...
  CR0=...  CR2=...  CR3=...  CR4=...
```

设计点：
- `interrupt_frame` 结构体已包含全部 GPR(由 ISR stub 压栈)，之前只是没打印
- R8-R11、R12-R15 分行，保持 4 列对齐便于视觉扫描
- CR0 是新增的 -- FPU 相关的 #GP 需要看 CR0.TS / CR0.EM / CR0.MP

### 2. RFLAGS 位解码

```
[RFLAGS decode] 0x10206 = IF PF IOPL=0 RF
```

将裸 `0x10206` 展开为命名位:IF(中断使能)、TF(单步)、DF(方向)、AC(对齐检查)、
IOPL、条件码(OF/SF/ZF/CF/AF/PF)、RF/VM/NT。

关键场景：异常发生在关中断上下文中时，IF 位为 0，说明不能依赖调度器/timer。

### 3. CR0 位解码

```
[CR0 decode] 0x80010033: PE MP NE WP PG
```

FPU 相关位(MP/EM/TS/NE)单独标注，因为 LaOS 最近遇到过 CR4.OSFXSR 未设置导致
`fxsave64` #GP 的问题。TS=1 时说明 FPU 处于"任务切换"状态，下一次 FPU 指令也会 #NM。

### 4. RIP 符号解析 + 指令字节

```
[RIP symbol] switch_to+0x23
[RIP code] f3 0f ae 0f 48 8b 26 0f ae 0e 5f 5e 5d 5b 58 c3
```

两个信息：
- **符号解析**: 通过 `kallsyms_lookup()` 将 RIP 转为 `函数名+偏移`。不需要 objdump 就能知道崩溃位置
- **指令字节**: RIP 处 16 字节 raw hex。经验丰富的开发者可以直接从指令字节识别：
  - `f3 0f ae 0f` = `fxsave64 [rdi]`(正是之前 #GP 的元凶)
  - `0f 0b` = `ud2`(故意触发的 #UD)
  - `48 f7 f9` = `idiv rcx`(除零的 #DE)

### 5. 异常专项分析

每种异常有独立的解码逻辑：

| 向量 | 异常 | 专项信息 |
|------|------|---------|
| 0 | #DE | 除零或商溢出 |
| 6 | #UD | 指令字节 + CPUID 提示 |
| 8 | #DF | 不可恢复 |
| 13 | #GP | error code 解码； ec=0 时提示可能原因 |
| 14 | #PF | CR2 + error code 五位解码 + 四级页表遍历 |

`#GP ec=0` 的原因提示从单纯的 `"non-segment cause"` 扩展为：
> non-canonical addr, misaligned memory access, privileged instr,
> bad MSR write, or NULL segment load

这对于 `fxsave64` 对齐问题("misaligned memory access")能直接定位方向。

### 6. 栈回溯(带符号解析)

```
[Stack Backtrace]
  #0  RIP=0xffffffff8000aa75  <exception_test_trigger+0x35>
  #1  rbp=0xffff80007ff94fa0  ret=0xffffffff8000aa8d  <exception_test_lv2+0xd>
  #2  rbp=0xffff80007ff94fb0  ret=0xffffffff8000aa9d  <exception_test_lv1+0xd>
  #3  rbp=0xffff80007ff94fc0  ret=0xffffffff8000af3c  <kmain+0x14c>
  #4  rbp=0xffff80007ff94ff0  ret=0x0000000000000000  <?+0x0>
```

每帧从裸 `ret=0xffff...` 变成 `<函数名+偏移>`，直接看到调用链。RIP 本身作为 #0 帧。

帧 #4 的 `ret=0x0000000000000000` 是汇编 bootstrap 的栈底 — 启动代码不维护帧指针链，
`ret=0` 标记链的末端，这是预期行为。

三层 `noinline` 调用链 (`lv1 -> lv2 -> trigger`) 保证测试场景下至少 3 帧命中符号解析。
线程上下文中的异常(如 `switch_to` 内触发)能自然展示 4+ 帧的完整调度链。

帧格式说明：
- `ret` 值是 caller 中 `call` 指令的下一条指令地址。即该帧返回后 CPU 从 `ret` 处继续执行
- 偏移量 (`+0x14c`) = `ret_addr - 函数入口地址`，用于定位崩溃点在函数内的位置

## 全量符号表： 两遍链接

### 动机

最初 `kallsyms_lookup()` 只搜索 `__ksymtab`(由 `EXPORT_SYMBOL` 宏填充)。
未导出的函数(如 `kmain`)在回溯中不可解析，显示为 `<？+0x0>`。

增强后，所有函数(包括未导出的 C 函数)都能在回溯中解析，不需要 `EXPORT_SYMBOL`。

### 构建机制

`kernel.mk` 使用两遍链接：

```
# Pass 1: 链接 kernel(不包含符号表)，生成 kernel.bin
ld $(OBJ) -o bin-x86_64/kernel

# 从 kernel.bin 提取全部函数符号，生成 C 数组
script/gen_kallsyms.sh bin-x86_64/kernel > obj-x86_64/generated/kernel/kallsyms_all.c

# 编译生成的符号表
cc -c obj-x86_64/generated/kernel/kallsyms_all.c \
  -o obj-x86_64/generated/kernel/kallsyms_all.c.o

# Pass 2: 重新链接，包含符号表
ld $(filter-out kallsyms_all.c.o,$(OBJ)) kallsyms_all.c.o -o bin-x86_64/kernel
```

`gen_kallsyms.sh` 用 `nm -n` 提取 `.text` 段所有函数符号(过滤 `.L*` / `_Z*` / `.` 前缀)，
按地址升序生成 `kallsyms_all[]` 数组。当前包含约 234 个符号。

### kallsyms_lookup 算法： 两遍 + 最佳匹配

```
kallsyms_lookup(ret_addr, &offset)
    best_addr = 0

    // Pass 1: __ksymtab (EXPORT_SYMBOL, 快速)
    for sym in __ksymtab:
        if sym->addr <= ret_addr && sym->addr > best_addr:
            best_addr = sym->addr
            best = sym

    // Pass 2: kallsyms_all (全量 nm 表)
    for i in 0..KALLSYMS_COUNT:
        a = kallsyms_all[i].addr
        if a <= ret_addr && a > best_addr:
            best_name = kallsyms_all[i].name
            best_addr = a
            from_kallsyms = 1

    if from_kallsyms:
        *offset = ret_addr - best_addr
        return best_name

    if best && offset:
        *offset = ret_addr - best->addr

    return best ? best->name : NULL
```

关键设计：
- 两层 Pass 都使用 `addr <= ret_addr && addr > best_addr` 条件 — 在所有候选符号中选**地址最大者**
- Pass 2 必须遍历全表后再决定，不能提前返回
- 每层函数都能被解析，不依赖 `EXPORT_SYMBOL`

### Pass 2 的早期 return bug(已修复)

初版 Pass 2 在遇到第一个满足条件的符号时就 `return`:

```c
// 旧代码 — 有 bug
for (int i = 0; i < KALLSYMS_COUNT; i++) {
    uint64_t a = kallsyms_all[i].addr;
    if (a <= ret_addr && a > best_addr) {
        *offset = ret_addr - a;
        return kallsyms_all[i].name;   // 第一个就返回
    }
}
```

由于表按地址排序，ret_addr 之前的第一个符号(地址较低)会立即返回，
而更接近 ret_addr 的后续符号(如 `kmain`)永远不会被考虑。

修复：让 Pass 2 像 Pass 1 一样遍历全表，追踪最佳匹配：

```
| ret_addr        | 旧结果(错误)                 | 新结果(正确)    |
|-----------------|-----------------------------|----------------|
| kmain+0x14c     | secondary_cpu_init+0x49c    | kmain+0x14c    |
```

## current NULL guard

```c
struct thread *cur = cpu_get_ctx()->current;
const char *proc_name = cur ? cur->name : "(null)";
int pid = cur ? cur->id : -1;
```

早期异常(在 `thread_init_main()` 之前)发生时 `current` 为 NULL，直接解引用会导致
在异常处理器内部触发二次异常(双重故障或三重故障)。加 guard 后优雅降级为 `"(null)"` / `-1`。

## 测试机制

参见 `kernel/config.h` 中的 `CONFIG_EXCEPTION_TEST` 宏。

```
CONFIG_EXCEPTION_TEST = 0  ->  #if 0，函数体和调用点被预处理器删除,零开销
CONFIG_EXCEPTION_TEST = 1  ->  在 kmain() 末尾触发除零异常(#DE)
```

测试覆盖项：
- GPR 全量 dump
- RFLAGS 位解码
- CR0 位解码
- RIP 符号解析 + 指令字节
- 栈回溯符号化(四层调用链： `kmain -> lv1 -> lv2 -> trigger`)

使用 `#if` 而非 `#ifdef`:如果误删 define，编译器报错而非静默关闭。

### 使用方式

```bash
# 开启测试开关
sed -i 's/CONFIG_EXCEPTION_TEST 0/CONFIG_EXCEPTION_TEST 1/' kernel/config.h
make -j

# 启动后会在所有子系统就绪后触发 #DE 除零异常
# 输出应包含:
#   [Registers]       -- 全量 GPR (RAX-R15, RIP, RSP, RFLAGS, CR0-4)
#   [RFLAGS decode]   -- IF/ZF/CF/PF/IOPL...
#   [CR0 decode]      -- PE/MP/NE/WP/PG
#   [RIP symbol]      -- exception_test_trigger+偏移
#   [RIP code]        -- 16 字节 hex (含 idiv 指令)
#   [Stack Backtrace] -- trigger -> lv2 -> lv1 -> kmain (带符号名+偏移)

# 验证完恢复
sed -i 's/CONFIG_EXCEPTION_TEST 1/CONFIG_EXCEPTION_TEST 0/' kernel/config.h
```

### 预期输出(截取核心部分)

```
================================================
  KERNEL EXCEPTION  vec=0 (#DE Divide Error) CPU 0 name main pid -1
================================================
[Registers]
  RAX=...  RBX=...  RCX=...  RDX=...
  ...
  CR0=0x0000000080010011  CR2=0x0  CR3=0x0000000000110000  CR4=0x0000000000000620
[RFLAGS decode] 0x286 = IF SF PF IOPL=0
[CR0 decode] 0x80010011: PE WP PG
[RIP symbol] exception_test_trigger+0x35
[RIP code] f7 f9 89 44 24 0c 8b 44 24 0c 31 c0 e8 07 a3 ff 00
[Divide Error] DIV/IDIV with zero divisor or quotient overflow.
[Stack Backtrace]
  #0  RIP=0xffffffff8000aa75  <exception_test_trigger+0x35>
  #1  rbp=0xffff80007ff94fa0  ret=0xffffffff8000aa8d  <exception_test_lv2+0xd>
  #2  rbp=0xffff80007ff94fb0  ret=0xffffffff8000aa9d  <exception_test_lv1+0xd>
  #3  rbp=0xffff80007ff94fc0  ret=0xffffffff8000af3c  <kmain+0x14c>
  #4  rbp=0xffff80007ff94ff0  ret=0x0000000000000000  <?+0x0>
================================================
  System Halted.
================================================
```

## CR0 读取： arch_read_cr0()

在 `kernel/arch/x86_64/arch_cpu.h` 中新增，与已有的 `arch_read_cr2()` /
`arch_read_cr4()` 保持一致的命名和实现风格。

## 文件清单

| 文件 | 改动 |
|------|------|
| `kernel/arch/x86_64/idt.c` | `exception_handler()` 完整重写； 新增 `#include "ksym.h"` |
| `kernel/arch/x86_64/idt.h` | 修正 `interrupt_frame` 字段顺序(匹配 PUSH_ALL) |
| `kernel/arch/x86_64/arch_cpu.h` | 新增 `arch_read_cr0()` |
| `kernel/config.h` | 新增 `CONFIG_EXCEPTION_TEST` 宏 |
| `kernel/arch/x86_64/main.c` | 新增 `exception_test()` + 三层 noinline 调用链； 新增 `#include "config.h"` |
| `kernel/ksym.c` | 新增两遍 `kallsyms_lookup()`(EXPORT_SYMBOL + 全量表)； Pass 2 最佳匹配 |
| `kernel/ksym.h` | `kallsyms_lookup(ret_addr, *offset)` 声明 |
| `obj-<arch>/generated/<output>/kallsyms_all.c` | 架构/输出私有的生成符号表，避免并行构建竞态 |
| `script/gen_kallsyms.sh` | `nm -n` 提取全部函数符号，生成 C 数组 |
| `kernel.mk` | `-fno-omit-frame-pointer` + `-fno-optimize-sibling-calls`； 两遍链接规则 |
| `docs/design/diagnostics/exception-handler.md` | 本文档 |

## 设计决策与坑

### `-fno-omit-frame-pointer` — 让 RBP 回溯生效

内核默认 `-O2` 编译，x86_64 上 GCC 默认开启 `-fomit-frame-pointer`，
RBP 被当通用寄存器用，不存在帧指针链。异常处理器里的 RBP 回溯从头到尾都是
读随机值 — 不管几层调用链，只能拿到调用者残留在 RBP 里的垃圾。

修复：`kernel.mk` 加 `-fno-omit-frame-pointer`，强制每个函数维护 RBP 链。
代价 ~5% 代码体积增加，换回真正可用的栈回溯。

### `-fno-optimize-sibling-calls` — 防止尾调用折叠栈帧

即使有了帧指针，编译器仍会在尾位置将 `call + ret` 优化为 `jmp`（sibling call
optimization）。例如 `lv1` 的最后一个动作是调 `lv2`，编译器生成 `jmp lv2`
而非 `call lv2； ret`，导致 `lv1` 不出现在 RBP 链中。

修复：`kernel.mk` 加 `-fno-optimize-sibling-calls`，确保每层函数都生成
`call` + `push rbp/mov rsp,rbp`。

两个 flag 的位置：`kernel.mk` 第 79-80 行，`-fdata-sections` 之后。

### 为什么测试需要三层 noinline 调用链

v1 版本的 `exception_test()` 是单一 `static` 函数，直接触发除零：

```
kmain()
  -> exception_test()   [static， 没有栈帧链]
    -> idiv  #DE
```

结果： 回溯只有 1 帧 (`#0 RIP`)，且符号解析命中最近的导出符号
`interrupts_enabled+0x4c8`(而非 `exception_test`，因为 `static` 函数不在 `__ksymtab` 中)。

v2 拆成三层 `noinline` + `EXPORT_SYMBOL`:

```
kmain()
  -> exception_test()            [static， 入口包装]
    -> exception_test_lv1()      [EXPORT_SYMBOL， noinline]
      -> exception_test_lv2()    [EXPORT_SYMBOL， noinline]
        -> exception_test_trigger()  [EXPORT_SYMBOL， noinline]
          -> idiv  #DE
```

结果： 至少 4 帧 (`trigger -> lv2 -> lv1 -> kmain`)，每帧 `<函数名+偏移>`
全部可解析。

### 全量符号表 vs EXPORT_SYMBOL

引入两遍链接 + `kallsyms_all` 后，所有 C 函数(包括 `kmain` 等未导出函数)
都能在回溯中解析。不再需要为测试函数单独 `EXPORT_SYMBOL`。

`EXPORT_SYMBOL` 机制仍保留，用于模块加载时的动态符号解析(`ksym_lookup`)。
`kallsyms_lookup` 的 Pass 1 先查 `__ksymtab`，Pass 2 再查全量表 — 两表互补。

### `pid -1` 与 `name (null)`

测试在 `kmain()` 末尾触发，此时 `thread_init_main()` 尚未执行，
`cpu_get_ctx()->current` 为 NULL。加 NULL guard 后优雅降级为
`pid -1` / `name (null)`，避免在异常处理器内部二次崩溃。

线程上下文中的真实异常会正确显示 `pid X` / `name foo`。

### 为什么 kmain 帧之后 RBP 链断开

kmain 由汇编 bootstrap 调用：

```
_start (asm)
  -> setup stack, call kmain
```

汇编入口不维护帧指针(`RBP`)，因此 kmain 的 `RBP` 指向 bootloader
残留值，不在内核虚拟地址范围内，回溯在此停止。`ret=0` 标记链末端，
这是预期行为，不影响 kmain 之下的 C 调用链。

### RFLAGS 位解码为何内联而非查表

RFLAGS 在异常时最重要的问题是"是否在中断上下文中"(IF 位)。
另外条件码(ZF/CF/OF)在除零或断言失败时直接提示分支方向。
IOPL/AC/ID 等用于判断是否用户态触发了特权指令。

位集不固定，内联 `if` 比查表更紧凑，且编译器能将常量折叠优化到最小。
