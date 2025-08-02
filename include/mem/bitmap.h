#include <stdbool.h>
#include <stdint.h>

extern uint8_t *bitmap;
extern uint64_t bitmap_size;

void *bitmap_alloc_page(bool add_offset);
void *bitmap_alloc_pages(uint64_t count, bool add_offset);
void bitmap_free_pages(void *addr, uint64_t count, bool has_offset);

static inline void set_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t mask = 1 << (index % 8);
    __atomic_fetch_or(&bitmap[byte], mask, __ATOMIC_SEQ_CST);
}

static inline void clear_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t mask = ~(1 << (index % 8));
    __atomic_fetch_and(&bitmap[byte], mask, __ATOMIC_SEQ_CST);
}

static inline bool test_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t value;
    __atomic_load(&bitmap[byte], &value, __ATOMIC_SEQ_CST);
    return (value & (1 << (index % 8))) != 0;
}
