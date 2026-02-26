#include <console/panic.h>
#include <console/printf.h>

void __assert_fail(const char *assertion, const char *file, unsigned int line,
                   const char *function) {
    panic("Assertion %s failed -> %s:%u:%s()\n", assertion, file, line,
            function);
}
