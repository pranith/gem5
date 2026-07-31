/*
 * Compare RC and TSO merge-buffer drain throughput.
 *
 * Each loop iteration writes every eight-byte word in four new cache lines.
 * The unrolling keeps multiple entries ready despite loop-branch recovery.
 * The merge buffer receives full-line entries without partial write RMW
 * overhead. RC may keep multiple independent drains outstanding, while TSO
 * admits only one drain until its response arrives.
 */

    .section .data
    .balign 64
stream:
    .quad 0

    .section .bss
    /* 8192 cache lines total, including the initialized word above. */
    .skip 524280

    .section .text
    .global _start
    .type _start, %function
_start:
    adrp x0, stream
    add  x0, x0, :lo12:stream
    mov  x1, #2048
    mov  x2, #1

1:
    str  x2, [x0, #0]
    str  x2, [x0, #8]
    str  x2, [x0, #16]
    str  x2, [x0, #24]
    str  x2, [x0, #32]
    str  x2, [x0, #40]
    str  x2, [x0, #48]
    str  x2, [x0, #56]
    str  x2, [x0, #64]
    str  x2, [x0, #72]
    str  x2, [x0, #80]
    str  x2, [x0, #88]
    str  x2, [x0, #96]
    str  x2, [x0, #104]
    str  x2, [x0, #112]
    str  x2, [x0, #120]
    str  x2, [x0, #128]
    str  x2, [x0, #136]
    str  x2, [x0, #144]
    str  x2, [x0, #152]
    str  x2, [x0, #160]
    str  x2, [x0, #168]
    str  x2, [x0, #176]
    str  x2, [x0, #184]
    str  x2, [x0, #192]
    str  x2, [x0, #200]
    str  x2, [x0, #208]
    str  x2, [x0, #216]
    str  x2, [x0, #224]
    str  x2, [x0, #232]
    str  x2, [x0, #240]
    str  x2, [x0, #248]
    add  x0, x0, #256
    subs x1, x1, #1
    b.ne 1b

    mov  x0, #0
    mov  x8, #93
    svc  #0

    .size _start, .-_start
