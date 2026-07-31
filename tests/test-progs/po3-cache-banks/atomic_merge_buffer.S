/*
 * Check that an LSE atomic retains its SQ state while it drains through the
 * PO3 merge buffer and observes an older buffered store to the same line.
 */

    .arch armv8.1-a+lse

    .section .data
    .balign 64
value:
    .quad 0
    .skip 56
other:
    .quad 0

    .section .text
    .global _start
    .type _start, %function
_start:
    adrp x0, value
    add  x0, x0, :lo12:value

    /* Populate two merge-buffer entries before issuing the atomic. */
    mov  x1, #41
    str  x1, [x0]
    mov  x1, #7
    str  x1, [x0, #64]

    /* The atomic must read 41 and leave 42 in memory. */
    mov  x2, #1
    ldaddal x2, x3, [x0]
    cmp  x3, #41
    b.ne fail
    ldr  x4, [x0]
    cmp  x4, #42
    b.ne fail

    mov  x0, #0
    mov  x8, #93
    svc  #0

fail:
    b    fail

    .size _start, .-_start
