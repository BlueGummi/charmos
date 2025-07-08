#include <block/sched.h>

bool noop_should_coalesce(struct generic_disk *disk,
                          const struct bio_request *a,
                          const struct bio_request *b) {
    (void) disk, (void) a, (void) b;
    return false;
}

void noop_do_coalesce(struct generic_disk *disk, struct bio_request *into,
                      struct bio_request *from) {
    (void) disk, (void) into, (void) from;
}

void noop_reorder(struct generic_disk *disk) {
    (void) disk;
}
