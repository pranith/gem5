/*
 * Freestanding AArch64 microbenchmark for PO3 D-cache bank conflicts.
 *
 * Each element occupies one 64-byte cache line. Every partial-store stream is
 * 64 lines above its corresponding load stream, preserving the lower six
 * bank-index bits. This intentionally aliases each read/write pair in caches
 * with 64 or fewer line-interleaved banks.
 */

#include <stdint.h>

enum
{
    Streams = 8,
    LinesPerStream = 16,
    StreamStrideLines = 128,
    StoreOffsetLines = 64,
    TotalLines = Streams * StreamStrideLines,
    Repetitions = 256,
};

struct BankLine
{
    volatile uint64_t value;
    uint8_t padding[56];
};

static struct BankLine workingSet[TotalLines] __attribute__((aligned(64)));

__attribute__((noreturn)) void
_start(void)
{
    uint64_t accumulator0 = 1;
    uint64_t accumulator1 = 3;
    uint64_t accumulator2 = 5;
    uint64_t accumulator3 = 7;
    uint64_t accumulator4 = 11;
    uint64_t accumulator5 = 13;
    uint64_t accumulator6 = 17;
    uint64_t accumulator7 = 19;

    for (unsigned repetition = 0; repetition < Repetitions; ++repetition) {
        for (unsigned line = 0; line < LinesPerStream; ++line) {
            const uint64_t value0 =
                workingSet[0 * StreamStrideLines + line].value;
            const uint64_t value1 =
                workingSet[1 * StreamStrideLines + line].value;
            const uint64_t value2 =
                workingSet[2 * StreamStrideLines + line].value;
            const uint64_t value3 =
                workingSet[3 * StreamStrideLines + line].value;
            const uint64_t value4 =
                workingSet[4 * StreamStrideLines + line].value;
            const uint64_t value5 =
                workingSet[5 * StreamStrideLines + line].value;
            const uint64_t value6 =
                workingSet[6 * StreamStrideLines + line].value;
            const uint64_t value7 =
                workingSet[7 * StreamStrideLines + line].value;

            workingSet[0 * StreamStrideLines + StoreOffsetLines + line].value =
                value0 + accumulator0;
            workingSet[1 * StreamStrideLines + StoreOffsetLines + line].value =
                value1 + accumulator1;
            workingSet[2 * StreamStrideLines + StoreOffsetLines + line].value =
                value2 + accumulator2;
            workingSet[3 * StreamStrideLines + StoreOffsetLines + line].value =
                value3 + accumulator3;
            workingSet[4 * StreamStrideLines + StoreOffsetLines + line].value =
                value4 + accumulator4;
            workingSet[5 * StreamStrideLines + StoreOffsetLines + line].value =
                value5 + accumulator5;
            workingSet[6 * StreamStrideLines + StoreOffsetLines + line].value =
                value6 + accumulator6;
            workingSet[7 * StreamStrideLines + StoreOffsetLines + line].value =
                value7 + accumulator7;

            accumulator0 += value0 + repetition + 1;
            accumulator1 += value1 + repetition + 3;
            accumulator2 += value2 + repetition + 5;
            accumulator3 += value3 + repetition + 7;
            accumulator4 += value4 + repetition + 11;
            accumulator5 += value5 + repetition + 13;
            accumulator6 += value6 + repetition + 17;
            accumulator7 += value7 + repetition + 19;
        }
    }

    register long status asm("x0") =
        (accumulator0 | accumulator1 | accumulator2 | accumulator3 |
         accumulator4 | accumulator5 | accumulator6 | accumulator7) == 0;
    register long syscallNumber asm("x8") = 93;
    asm volatile("svc #0" : : "r"(status), "r"(syscallNumber) : "memory");
    __builtin_unreachable();
}
