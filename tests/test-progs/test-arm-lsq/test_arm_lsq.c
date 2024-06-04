#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void store_then_load(uint8_t* addr, uint8_t* addr2) {
    asm volatile(
        "stp x0, x1, [%0] \n\t"
        "ldp x0, x1, [%1] \n\t"
        :
        : "r"(addr), "r"(addr2)
        : "v0", "v1", "memory"
    );
}


int main() {
    void *ptr = NULL;
    size_t alignment = 4 * 1024;
    size_t size = 1024 * 1024;

    int result = posix_memalign(&ptr, alignment, size);
    if (result) {
        fprintf(stderr, "posix_memalign failed: %d\n", result);
        return EXIT_FAILURE;
    }

    store_then_load(ptr+4080, ptr+4088);

    free(ptr);
    return 0;
}
