#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#define k_panic(fmt, ...)                                                                                                                                                                                        \
    do {                                                                                                                                                                                                         \
        k_printf("\n+-\033[31m!!!\033[0m[\033[91mPANIC\033[0m]\033[31m!!!\033[0m\n|\n+-> [\033[92mFROM\033[0m] \033[32m" __FILE__ "\033[0m:\033[32m%d\033[0m\n+-> [\033[33mMESSAGE\033[0m] \033[31m", __LINE__); \
        panic(fmt, ##__VA_ARGS__);                                                                                                                                                                               \
        k_printf("\033[0m");                                                                                                                                                                                     \
    } while (0)
void k_printf(const char *format, ...);
void panic(const char *format, ...);
void k_printf_init(struct flanterm_context *f);
