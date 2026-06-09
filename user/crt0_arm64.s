// crt0_arm64.s — ARM64 用户态入口 (GAS 语法)
//
// 内核通过 arch_enter_usermode ERET 进入 EL0，
// x0=argc, sp 指向 argv 数组（当前 stub 均为 0）。
// AAPCS64: 进入函数时 sp 必须 16 字节对齐。

.section .text
.global _start
.type _start, @function

_start:
    // x0=argc, x1=argv (由内核 EL0 入口设置，stub 时均为 0)
    mov  x1, sp          // argv = sp (栈顶指针数组)

    // 调用 main(argc, argv)
    bl   main

    // main 返回后：调用 SYS_EXIT (svc #1)
    mov  x0, x0          // 返回值已在 x0
    svc  #1

    // 理论上不会到这里，兜底死循环
1:  wfe
    b    1b
    .size _start, . - _start
