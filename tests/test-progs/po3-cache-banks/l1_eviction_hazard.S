/*
 * Exercise TSO load/load recovery when an executed younger load's cache line
 * is evicted before an older load completes.
 *
 * The older load address depends on a long integer chain. Younger loads use
 * independent addresses separated by 8 KiB, which maps them to the same set
 * in the 32 KiB, four-way L1D used by po3_cache_banks.py.
 */

    .section .data
    .balign 8192
data:
    .quad 0

    .section .bss
    .skip 57336

    .section .text
    .global _start
    .type _start, %function
_start:
    adrp x20, data
    add  x20, x20, :lo12:data
    mov  x21, #8192

    add  x1, x20, x21
    add  x2, x1, x21
    add  x3, x2, x21
    add  x4, x3, x21
    add  x5, x4, x21
    add  x6, x5, x21

    mov  x0, x20
    .rept 48
    add  x0, x0, xzr
    .endr

    /* Program-order older load, delayed by the x0 dependency chain. */
    ldr  x10, [x0]

    /* These execute early and collectively exceed the L1D associativity. */
    ldr  x11, [x1]
    ldr  x12, [x2]
    ldr  x13, [x3]
    ldr  x14, [x4]
    ldr  x15, [x5]
    ldr  x16, [x6]

    orr  x10, x10, x11
    orr  x10, x10, x12
    orr  x10, x10, x13
    orr  x10, x10, x14
    orr  x10, x10, x15
    orr  x0, x10, x16

    mov  x8, #93
    svc  #0

    .size _start, .-_start
