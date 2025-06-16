#include <stdint.h>
#define LAPIC_REG_ID 0x020
#define LAPIC_REG_EOI 0x0B0
#define LAPIC_REG_SVR 0x0F0
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR 0x390
#define LAPIC_REG_TIMER_DIV 0x3E0
#define LAPIC_ENABLE 0x100

#define LAPIC_REG(offset)                                                      \
    (*(volatile uint32_t *) ((uintptr_t) lapic + (offset)))
#define TIMER_VECTOR 0x20
#define TIMER_MODE_PERIODIC (1 << 17)
extern uint64_t *lapic;
void lapic_init();
#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MASK 0xFFFFF000UL
