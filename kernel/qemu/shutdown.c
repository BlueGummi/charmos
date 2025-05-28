#include <io.h>
#include <printf.h>
#include <stdint.h>

void k_shutdown(void) {
    outw(0x604, 0x2000);
}
