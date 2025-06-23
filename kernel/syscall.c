#include <console/printf.h>
#include <stdint.h>
#include <mem/vmm.h>


void syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5) {
    switch (num) {
    case 1:
        k_printf("userspace says: %s\n", (char*)arg1);
        break;
    default: k_printf("Unknown syscall: %lu\n", num); break;
    }
}
