#include <irq/irq.h>
extern enum irq_result page_fault_handler(void *context, uint8_t vector,
                                          struct irq_context *rsp);
