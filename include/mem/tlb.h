#include <acpi/lapic.h>
#include <mem/page.h>
#include <stdatomic.h>
#include <stdint.h>

/* per-cpu */
#define TLB_QUEUE_SIZE 64

struct tlb_shootdown_cpu {
    atomic_uintptr_t queue[TLB_QUEUE_SIZE];
    atomic_uint_fast32_t head;
    atomic_uint_fast32_t tail;
    atomic_uint_fast64_t ack_gen;
    atomic_uint_fast8_t flush_all;
    atomic_uint_fast8_t dpc_queued;
};

void tlb_init(void);
void tlb_shootdown_isr(void *ctx, uint8_t irq, void *rsp);
void tlb_shootdown(uintptr_t addr, bool synchronous);
