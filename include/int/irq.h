#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void (*irq_handler_t)(void *ctx);

struct irq_entry {
    bool installed;
    irq_handler_t handler;
    void *ctx;
};

#define IRQ_MAX 256

void irq_init(void);
int irq_install(uint8_t irq, irq_handler_t handler, void *ctx);
int irq_free(uint8_t irq);
void irq_dispatch(uint8_t irq);
int irq_alloc_free_vector(void);

bool irq_is_installed(uint8_t irq);
void irq_set_installed(uint8_t irq, bool state);
