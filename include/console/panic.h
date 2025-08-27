#pragma once
#include <asm.h>
#include <charmos.h>
#include <limine.h>
#include <misc/colors.h>
#include <misc/logo.h>
#include <time.h>

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)
#define LINE_STRING STRINGIZE(__LINE__)

void debug_print_stack();
extern void panic_entry();
void k_printf(const char *format, ...);

static inline void qemu_exit(int code) {
    outb(0xf4, ((code << 1) | 1) & 0xFF);
}

struct panic_regs {
    uint64_t rsp;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
};

#define ELEVEN_LINES "==========="
#define TWENTY_TWO_LINES ELEVEN_LINES ELEVEN_LINES
#define FORTY_FOUR_LINES TWENTY_TWO_LINES TWENTY_TWO_LINES
#define EIGHTY_EIGHT_LINES FORTY_FOUR_LINES FORTY_FOUR_LINES
extern struct spinlock panic_lock;

#define k_panic(fmt, ...)                                                      \
    do {                                                                       \
        global.panic_in_progress = true;                                       \
        disable_interrupts();                                                  \
        spin_lock(&panic_lock);                                                \
        k_printf("\n" EIGHTY_EIGHT_LINES "\n");                                \
        k_printf("\n                                    [" ANSI_BG_RED         \
                 "KERNEL PANIC" ANSI_RESET "]\n\n");                           \
        k_printf(ANSI_RED "%s\n\n" ANSI_RESET, OS_LOGO_PANIC_CENTERED);        \
        panic_entry();                                                         \
        k_printf("\n    [" ANSI_BRIGHT_BLUE "AT" ANSI_RESET " ");              \
        time_print_current();                                                  \
        k_printf("]\n");                                                       \
        k_printf("    [" ANSI_BRIGHT_GREEN "FROM" ANSI_RESET                   \
                 "] " ANSI_GREEN __FILE__ ANSI_RESET                           \
                 ":" ANSI_GREEN LINE_STRING ANSI_RESET ":" ANSI_CYAN           \
                 "%s()" ANSI_RESET "\n"                                        \
                 "    [" ANSI_YELLOW "MESSAGE" ANSI_RESET "] ",                \
                 __func__);                                                    \
        k_printf(fmt, ##__VA_ARGS__);                                          \
        debug_print_stack();                                                   \
        k_printf("\n" EIGHTY_EIGHT_LINES "\n");                                \
        spin_unlock(&panic_lock, false);                                       \
        qemu_exit(1);                                                          \
        while (1)                                                              \
            wait_for_interrupt();                                              \
    } while (0)
