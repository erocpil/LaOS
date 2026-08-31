# LaOS Codex 全库审查报告

审查范围：`kernel/`, `kernel/arch/x86_64/`, `module/`, `user/`, 构建脚本。
方式：只读审计，未修改任何文件。

---

## P0 — 会导致内核崩溃、内存损坏或远程利用

### P0-1：GDT 栈上分配（kernel/arch/x86_64/gdt.c:461-505）

**代码：**
```c
void gdt_init_cpu(void) {
    uint64_t gdt[GDT_ENTRIES * 2];      // 栈上分配！
    struct gdt_ptr per_cpu_gdt_ptr;       // 也是栈上！

    // ... 填充 gdt 和 per_cpu_gdt_ptr ...

    asm volatile("lgdt (%0)" :: "r"(&per_cpu_gdt_ptr));
    // 函数返回后，GDTR 指向已失效的栈内存
}
```

**问题：** `lgdt` 加载的 GDTR 指向栈局部变量。函数返回后，该栈空间被复用，此后任何段寄存器加载或中断都会读取垃圾数据作为 GDT 描述符，触发 #GP 或 triple-fault。

**现状：** 目前 BSP 在启动早期调用一次后栈未被复用，侥幸没炸。但加 SMP（多个 AP 各自调 `gdt_init_cpu`）或叠加 IST 中断使用后必然崩溃。

**修复方向：** GDT 必须持久分配（静态数组或 `pmm_alloc`），在整个 CPU 生命周期内保持有效。

---

### P0-2：CPU 索引不安全（main.c:290, cpu.c:1076, gdt.c TSS/IST）

**代码：**
```c
// main.c
struct limine_smp_response *smp_response = limine_smp_request.response;
if (smp_response == NULL) {
    L("WARNING: SMP not supported by bootloader");
    // 警告后继续使用！
}
// ...
cpu_count = smp_response->cpu_count;           // 无上限
for (int i = 0; i < cpu_count; i++) {
    int apic_id = smp_response->cpus[i]->lapic_id;  // 直接当数组下标
    g_cpu_contexts[apic_id] = ...;                   // MAX_CPUS=16
}
```

**问题：**
1. `smp_response` 为 NULL 后仍然解引用
2. CPU 数量无上限检查，超过 16 越界写 `g_cpu_contexts`
3. APIC ID 不一定是 0..N-1（可能是 0,2,4,6... 或 0,1,16,17...），稀疏 ID 直接当数组下标导致写越界

**现状：** 单 CPU 不触发。但只要加 SMP 或固件返回非连续 APIC ID 就炸。

**修复方向：** 验证响应非 NULL、限制 CPU 数、建立 APIC ID → 0..N-1 的映射表。

---

### P0-3：AP 的 TSS/IST 完全空白（main.c + gdt.c:563）

**代码：**
```c
// BSP 路径：调用完整初始化
gdt_init_cpu();  // 设置 TSS、IST

// AP 路径：
gdt_init_dynamic();  // 仅设置 CS/DS 等基础段，TSS 描述符指向全零内存
```

**问题：** 每个 AP CPU 有自己的 `per_cpu_tss`，但 `gdt_init_dynamic()` 只填充了 GDT 描述符的基址和 limit，**没有初始化 TSS 内容**。rsp0=0（Ring 3 → Ring 0 切换栈指针为零）、IST 条目全空。

**后果：** 在 AP 上运行用户态代码时，中断/Syscall 切换会用 rsp0=0 作为内核栈 → 必崩。#DF/NMI 也无 IST 保护。

**修复方向：** AP 启动路径中调用完整的 TSS 初始化（设置 rsp0、IST1-IST7、IOPB）。

---

### P0-4：SYS_WRITE 无边界检查（kernel/syscall.c:648-687）

**代码：**
```c
case SYS_WRITE: {
    char *buf = (char *)arg1;          // 用户传入的指针
    int len = (int)arg2;
    int off = (int)arg3;
    if ((uint64_t)buf < KERNEL_BASE) { // 只检查了"不在内核范围"
        for (int i = 0; i < len; i++) {
            putchar(buf[off + i]);     // 直接解引用！
        }
    }
}
```

**问题：**
1. 只检查 `buf >= KERNEL_BASE`，但不验证 `buf` 是否真的映射了
2. 不检查 `off + len` 是否越界（跨页、跨段、溢出回绕）
3. 不检查 `buf` 是否属于当前进程的用户地址空间
4. 用户传一个未映射的地址 → 内核 page fault → 崩溃

**修复方向：** 实现 `copy_from_user()`，逐页验证用户映射存在，用安全拷贝代替直接解引用。

---

### P0-5：ELF loader 无验证（kernel/elf.c:16）

**代码：**
```c
int elf_check(void *elf_data) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != 0x7f ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F')
        return -1;
    return 0;  // 只检查了魔数！
}
```

**问题：** 通过魔数检查后，下列字段**完全未验证**就被使用：
- `e_type`（是否真的是 ET_EXEC/ET_REL）
- `e_machine`（是否是 x86_64）
- `e_phoff`/`e_shoff`（段表/节表偏移是否在文件范围内）
- `e_phnum`/`e_shnum`（数量是否合理，是否溢出）
- `e_phentsize`/`e_shentsize`（条目大小是否正确）
- `p_offset`、`p_filesz`、`p_memsz`（段偏移和大小是否越界）
- 重定位表的索引和范围

**后果：** 一个精心构造的假 ELF 文件可以让 loader 越界读、写任意内核内存、创建任意页表映射。

**现状：** Boot module 来自受信任的磁盘镜像。但安全模型为零。

**修复方向：** 完整的 ELF 头校验（类型、架构、所有偏移在文件内、段大小关系正确、段虚拟地址在用户空间）。

---

### P0-6：e1000 ICMP 栈溢出（module/protocol.c:20-55）⭐ 远程可触发

**代码：**
```c
void send_icmp_reply(e1000_driver_t *nic, struct eth_header *req_eth,
                     struct ip_header *req_ip, struct icmp_header *req_icmp,
                     int payload_len) {
    uint8_t response[512];              // 栈上 512 字节
    // ...
    if (payload_len > 0) {
        memcpy((uint8_t*)icmp + sizeof(struct icmp_header),
               (uint8_t*)req_icmp + sizeof(struct icmp_header),
               payload_len);            // 无上限拷贝！
    }
}
```

**调用链：**
```
e1000_handle_packet()
  → 解包时 trust IP total_length（来自网络，攻击者控制）
  → payload_len = ip_total_len - sizeof(ip_header) - sizeof(icmp_header)
  → send_icmp_reply(..., payload_len)
  → memcpy(response + offset, packet_data, payload_len)
```

**问题：**
1. `payload_len` 来自网络包 IP 头的 `total_length` 字段，攻击者可控
2. `response[512]`，但 `memcpy` 长度无限制
3. 攻击者发送一个声明 `total_length=2000` 的 ICMP 包 → 栈溢出 1500 字节 → 覆盖返回地址

**严重性：** 远程可达（只需网络连通），一行 fix。

**修复方向：**
```c
int max_payload = sizeof(response) - sizeof(struct eth_header)
                  - sizeof(struct ip_header) - sizeof(struct icmp_header);
if (payload_len > max_payload) payload_len = max_payload;
```

---

### P0-7：分配失败后使用物理页 0（多处）

**代码示例（module/e1000.c:86-100）：**
```c
nic->rx_ring = (struct e1000_rx_desc *)phys_to_virt((uint64_t)pmm_alloc());
// pmm_alloc() 返回 NULL → phys_to_virt(0) = HHDM_BASE
// DMA 描述符环指向物理地址 0

for (int i = 0; i < NUM_RX_DESC; i++) {
    void* buf_phys = pmm_alloc();
    nic->rx_buffers[i] = phys_to_virt((uint64_t)buf_phys);
    nic->rx_ring[i].buffer_addr = (uint64_t)buf_phys;
    // pmm_alloc 返回 NULL → buffer_addr = 0 → 网卡 DMA 写到物理地址 0
}
```

**同样模式出现在：**
- `elf.c:71,161`（ELF 加载）
- `vmm.c:1283,1319,1546`（页表创建）
- `heap.c:787`（堆扩展）
- `cpu.c:997,1026`（CPU 上下文）
- `module/e1000.c:347-357,740`（e1000 TX/缓冲区池）

**问题：** `pmm_alloc()` 在 OOM 时返回 NULL（即 0），但所有调用者都不检查返回值，直接将 0 当作有效物理地址使用。物理页 0 通常包含实模式中断向量表、BIOS 数据区等关键结构，被覆盖后系统行为不可预测。

**修复方向：** 所有分配调用点检查 NULL 返回值，失败时回滚已分配资源并报错。

---

### P0-8：用户栈单页无容量检查（kernel/elf.c:152-221）

**代码：**
```c
void setup_user_stack(...) {
    uint64_t *stack = phys_to_virt((uint64_t)pmm_alloc());  // 仅 4KB
    uint64_t *sp = (uint64_t *)((uint8_t*)stack + 4096);

    // 复制参数字符串到栈顶，sp 递减
    for (int i = argc - 1; i >= 0; i--) {
        int len = strlen(argv[i]) + 1;
        sp = (uint64_t *)((uint8_t*)sp - len);   // 不检查是否超过 stack 底部
        memcpy(sp, argv[i], len);
    }
    // 继续压 argv 指针数组...
    sp = (uint64_t *)((uint64_t)sp & ~0xF);      // 对齐
    sp -= argc;                                   // 不检查
    // ...
}
```

**问题：**
1. `sp` 递减无下界检查，参数过长时 `sp < stack`，后续 `memcpy` 写到栈页之外
2. 写入的目标地址是 HHDM 映射的相邻物理页，会静默破坏内核数据
3. `argc * sizeof(uint64_t)` 可能溢出
4. `kmalloc`、`pmm_alloc`、`vmm_map` 全部不检查返回值

**修复方向：** 在每次 `sp` 递减前检查 `(uint8_t*)sp - size >= (uint8_t*)stack`。

---

### P0-9：Kernel heap 算术溢出（kernel/heap.c:787-794, 820）

**代码：**
```c
// kheap_expand_to_addr
uint64_t target = (uint64_t)addr;  // 用户可控的地址？
uint64_t aligned = (target + 0xFFF) & ~0xFFF;  // target 接近 0xFFFFFFFF 时溢出回绕
// ...
vmm_map(..., new_page, aligned, ...);  // 即使 vmm_map 失败
// 继续使用 new_page 对应的物理页（已从 pmm_alloc 消耗但未映射）

// kmalloc
uint64_t total_needed = sizeof(struct heap_header) + size;  // size 接近 UINT64_MAX 时溢出
// ...
```

**问题：**
1. `target + 0xFFF` 可溢出回绕到小地址
2. `sizeof(struct heap_header) + size` 可溢出导致分配不足
3. `vmm_map` 失败后不回滚已分配的物理页
4. 导致堆元数据写入未映射内存 → page fault

**修复方向：** 所有加法前检查溢出，`vmm_map` 失败时调用 `pmm_free` 回滚。

---

## P1 — 逻辑错误（通常不立即崩溃但结果错误）

### P1-1：模块参数无限制（kernel/elf_loader.c:683, module/module_foo.c:33,65）

- `module_apply_kv_params()` 将 task.conf 的任意值直接写入模块变量
- `PARAM_STRING` 无目的缓冲区大小限制，可溢出模块的全局变量
- `module_foo` 中 `n % (1000 / count)` 当 `count=0` 时除以零 → #DE 异常

### P1-2：thread_destroy 混用分配域（kernel/thread.c:27-29）

`thread_destroy()` 对所有线程都调用 `kfree(t->elf_load_addr)`，但模块线程使用 `module_alloc()`（独立 bump 分配器，不在 kheap 中）。`kfree` 会把模块地址当作堆块头解析 → 损坏内存或 page fault。

### P1-3：task 参数解析顺序错误（kernel/task.c:154-187, 218-221）

`task_run_cpu` 先读 `t->argc/t->argv`（用于提取用户态 id/code），**然后才**调用 `task_build_argv(t)` 去构建 argv。结果是 argv 还是空的，用户态参数被忽略，总是使用默认值。

### P1-4：vmm_map_region 静默忽略错误（kernel/vmm.c:1517-1541）

内部 page table 创建失败时返回 -1，但函数返回 `void`。调用者（如 PCI MMIO 映射）不知道映射不完整，继续访问未映射物理地址。

### P1-5：页表销毁不支持 huge pages 无 TLB shootdown

销毁路径假设所有映射都是 4KB 页。遇到 2MB/1GB huge page 时仅释放一个物理页（漏了剩余的）。没有 TLB invalidate 或 IPI shootdown，其他 CPU 可能继续使用旧映射。

### P1-6：task_parser 无边界检查（kernel/task_parser.c:34-151）

`parse_dec`/`parse_hex`/`parse_string` 递增 char 指针时从不检查是否超过 `task.conf` Limine module 的末尾。恶意/损坏的配置文件可越界读。

### P1-7：SMP 初始化竞争

- AP 和 BSP 同时调用 `gdt_init_dynamic()` 操作未同步的全局数据
- `mutex_test_init()` 使用非原子 `static int mutex_inited` 标志
- 两个 CPU 可同时看到 `mutex_inited == 0`，同时初始化 → 破坏 mutex 状态

### P1-8：RCU/stats 无 CPU 拓扑保护（rcu.c:1199-1202）

RCU grace period 等待遍历 `g_cpu_contexts[0..g_cpu_count-1]`，但如果部分 CPU 启动失败导致对应 slot 为 NULL，直接空指针解引用。

### P1-9：e1000 TX 无锁无长度限制（module/e1000.c:301）

`memcpy(nic->tx_buffers[i], data, len)` 不检查 `len <= 4096`。如果 `send_icmp_reply` 构造的帧超过 TX buffer 大小，静默越界写。多协议并发时可能覆盖描述符。

### P1-10：pmm_free 接受未对齐地址（kernel/pmm.c:438）

只要 bitmap 位为 1（已分配），任何地址——包括未对齐地址、低 1MB 保留区、从未分配的页——都可以被 "free"，导致保留页被标记为可用。

---

## P2 — 代码质量/清理

1. **死代码**：`load_elf()`、`relocate_module()`/`apply_relocation()`/`get_sym()` 旧版、多个 e1000 测试循环、PIT stub、`jump_to_user`、禁用的 TTY 实现
2. **魔数遍布**：段选择子、用户/内核边界、MMIO 基址、4K/1G 常量。应集中到架构头文件
3. **printf 缓冲区硬编码 1024**，宽度/精度累加可溢出 int
4. **PCI ECAM 硬映射 256MB**，不读 MCFG 实际 bus range。应在 MCFG 中解析实际大小
5. **错误路径资源泄漏**：`kthread_load_elf_exec()` 中 `kmalloc(elf)` 失败时中断已关且不恢复
6. **TTY 过滤器竞争**：`kprintf_color` 在 TTY 过滤拒绝输出时 `panic`，把 UI 竞争变成系统停机

---

## 推荐修复顺序

| 优先级 | 范围 | 理由 |
|--------|------|------|
| **1** | P0-6 e1000 ICMP 栈溢出 | 远程可达，一行 fix，立即见效 |
| **2** | P0-1 GDT 栈分配 + P0-3 AP TSS | 加 SMP 前必修 |
| **3** | P0-2 CPU 索引 | 加 SMP 前必修 |
| **4** | P0-5 ELF loader | 安全基础 |
| **5** | P0-4 SYS_WRITE | 安全基础 |
| **6** | P0-7 分配失败检查 | 健壮性 |
| **7** | P0-8 用户栈 + P0-9 heap 溢出 | 边缘情况 |
| **8** | P1 各项 | 逻辑正确性 |
| **9** | P2 清理 | 可维护性 |
