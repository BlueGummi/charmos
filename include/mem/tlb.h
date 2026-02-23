/* @title: TLB */
#include <acpi/lapic.h>
#include <mem/page.h>
#include <stdatomic.h>
#include <stdint.h>

/* per-cpu */
#define TLB_QUEUE_SIZE 64

struct tlb_shootdown_cpu {
    _Atomic uintptr_t queue[TLB_QUEUE_SIZE];
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    atomic_bool in_tlb_shootdown;
    _Atomic uint64_t ack_gen;
    _Atomic uint64_t target_gen;
    _Atomic uint8_t flush_all;
    _Atomic uint8_t dpc_queued;
};

void tlb_init(void);
enum irq_result tlb_shootdown_isr(void *ctx, uint8_t irq,
                                  struct irq_context *rsp);
void tlb_shootdown(uintptr_t addr, bool synchronous);
