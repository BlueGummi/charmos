#include <stdint.h>
void k_shutdown();
void k_shutdown_init(uint16_t pm1a_control, uint16_t pm1b_control,
                     uint16_t slp_typ);
