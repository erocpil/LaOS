[bits 64]
global switch_to
global ret_from_fork

%include "asm_offsets_nasm.inc"

;
; switch_to(struct thread *old_thread, struct thread *new_thread)
;
; rdi = old_thread（将被切走的线程）
; rsi = new_thread（将被切到的线程）
;
; 保存 old_thread 的当前执行上下文（GPR + RFLAGS + FPU）到其内核栈，
; 然后从 new_thread 的内核栈恢复其上下文，最后 ret 到 new_thread
; 上次被切走的位置。
;
; 已知风险：old_thread == NULL 时 mov [rdi], rsp 会零地址写。
; 当前所有调用路径（__schedule, thread_exit）保证 prev != NULL，
; 但未添加运行时 guard。若未来引入新的 switch_to 调用点，需注意。
;
; 空指针 guard 模板（如需启用，取消注释）：
;       test rdi, rdi
;       jz   .skip_save
;       ; ... 原有的 save 逻辑 ...
; .skip_save:
;       mov rsp, [rsi]
;       jmp .restore
;
switch_to:
        ; ── 1. 保存通用寄存器（callee-saved + RFLAGS）──
        pushfq                          ; RFLAGS
        push rbp
        push rbx
        push r12
        push r13
        push r14
        push r15
        push rdi                        ; old_thread 指针本身（也是新线程的 data 参数）

        mov [rdi], rsp                  ; old_thread->rsp = rsp（栈顶快照）

        ; ── 2. 保存 FPU/SSE 状态到 old_thread->fpu_state ──
        ;
        ; fxsave64 将当前硬件的完整 FPU 上下文写入 old_thread 的 512 字节
        ; 保存区。ST(0-7)、MMX(0-7)、XMM(0-15)、FCW、FSW、FTW、MXCSR 等
        ; 全部寄存器状态被捕获。fxsave64 不修改任何 FP 寄存器，old_thread
        ; 的 FPU 状态此刻冻结，下次切回时由 fxrstor64 精准还原。
        ;
        ; 对齐：[rdi + THREAD_FPU_STATE] 保证 16 字节对齐——
        ; struct thread 由 kmalloc（16 字节对齐）分配，
        ; fpu_state 字段通过 __attribute__((aligned(16))) 保证
        ; struct 内偏移为 16 的倍数。
        fxsave64 [rdi + THREAD_FPU_STATE]

        ; ── 3. 换栈到 new_thread ──
        mov rsp, [rsi]                  ; rsp = new_thread->rsp

        ; ── 4. 恢复 FPU/SSE 状态 ──
        ;
        ; 此时 rsp 已指向 new_thread 的栈，FPU 硬件仍是 old_thread 的状态。
        ; fxrstor64 从 new_thread->fpu_state 加载其上次被保存的完整 FPU
        ; 上下文。对于首次运行的线程，fpu_state 包含 cpu_enable_fpu() 生成的
        ; 标准 FXSAVE 模板（fninit → FCW=0x037F, ldmxcsr 0x1F80 → MXCSR），
        ; 即 x86 定义的 FPU reset state：所有数据寄存器 = +0.0，
        ; tag word = 0xFFFF（empty），所有异常已屏蔽。
        ;
        ; 注意：全零 buffer 不是合法 FXSAVE 镜像（FCW=0x0000, MXCSR=0x00000000），
        ;
        ; 顺序：先 fxrstor64 再 pop GPR。如果反过来（先 pop 再 fxrstor），
        ; 从 pop 到 fxrstor 之间，new_thread 的 FPU 上下文尚未就位——
        ; 若编译器在 pop 路径中使用了 XMM（当前 -mno-sse 不会，但防御），
        ; 会读到 old_thread 的残留 FPU 状态，产生隐蔽的错误。
        fxrstor64 [rsi + THREAD_FPU_STATE]

        ; ── 5. 恢复通用寄存器 ──
        ;
        ; pop 顺序必须与 push 顺序严格对称。
        ; 对于初生线程：rdi = data（由 thread_create_common 填入栈帧），
        ; r15 = entry（线程入口函数地址，通过 R15 走私传递给
        ; thread_entry_point 中的 inline asm）。
        pop rdi                         ; data 参数（初生线程）或 old_thread 指针
        pop r15                         ; entry 函数地址（初生线程走 R15 走私）
        pop r14
        pop r13
        pop r12
        pop rbx
        pop rbp
        popfq                           ; 恢复 RFLAGS（初生线程: 0x202, IF=1）

        ret                             ; → schedule() 的调用点，或 → ret_from_fork


extern thread_entry_point

;
; ret_from_fork — 新线程的首次执行入口
;
; 新线程不是从 schedule() 切换过来的——它没有"上次被切走的位置"。
; thread_create_common 在内核栈上伪造了一份 switch_to 的上下文帧，
; 栈顶的 ret 地址被设为 ret_from_fork（见 thread.c:104）。
;
; switch_to 执行到 ret 时：
;   - rdi = data（线程创建时传入的参数）
;   - r15 = entry（线程入口函数地址）
;   - RFLAGS = 0x202（IF=1，中断已开）
;   - FPU = reset state
;
; ret_from_fork 将控制权交给 thread_entry_point trampoline，
; 后者通过 inline asm 读出 R15 得到真正的入口函数，sti 后调用之。
; 如果线程函数返回，thread_entry_point 调用 thread_exit(code)
; 进入 schedule()，永不返回此处。
;
ret_from_fork:
        call thread_entry_point

        ; thread_entry_point 是 noreturn（末尾有 __builtin_unreachable），
        ; 不会执行到这里。


section .note.GNU-stack noalloc noexec nowrite progbits
