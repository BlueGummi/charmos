/* Space for the symbol table
 *
 * scripts/syms.awk fills this in after the kernel is linked
 *
 * Leading bytes are a placeholder because an image no one
 * stamped then reports no symbols instead of reading a header full of zeroes
 * as a valid empty table
 *
 * The initialiser also keeps the section PROGBITS, an all zero array would be
 * moved to .bss, and there would be nothing on disk for objcopy to replace */
#include <linker/symbol_table.h>

__attribute__((section(".kernel_syms"), used, aligned(16)))
const char kernel_syms_blob[KERNEL_SYMS_RESERVE] = "NOSYMS";
