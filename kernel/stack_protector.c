#include <enforce.h>
#include <stdint.h>

uintptr_t __stack_chk_guard = 0x595e9fbd94fda766ULL;

cc_noreturn void __stack_chk_fail(void) {
    panic("Kernel stack smashing detected!");
}
