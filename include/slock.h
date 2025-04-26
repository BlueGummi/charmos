#include <stdint.h>

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT {0}
void spinlock_lock(spinlock_t *lock);
void spinlock_unlock(spinlock_t *lock); 

