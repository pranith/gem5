/* Exercise merge-buffer dependencies from AArch64 release stores. */

#include <stdint.h>

struct BankLine
{
    volatile uint64_t value;
    uint8_t padding[56];
};

static struct BankLine lines[64] __attribute__((aligned(64)));
static uint64_t ready __attribute__((aligned(64)));

__attribute__((noreturn)) void
_start(void)
{
    for (uint64_t i = 0; i < 256; ++i) {
        lines[i % 64].value = i;
        __atomic_store_n(&ready, i, __ATOMIC_RELEASE);
    }

    register long status asm("x0") = lines[63].value != 255;
    register long syscall_number asm("x8") = 93;
    asm volatile("svc #0" : : "r"(status), "r"(syscall_number) : "memory");
    __builtin_unreachable();
}
