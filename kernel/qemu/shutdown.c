#include <printf.h>
#include <stdint.h>
#include <io.h>

void k_shutdown(void) {
    outw(0x604, 0x2000);
}
