#include <asm.h>
#include <block/sched.h>
#include <drivers/ahci.h>
#include <mem/alloc.h>
#include <mem/vmm.h>

void ahci_do_coalesce(struct generic_disk *disk, struct bio_request *into,
                      struct bio_request *from);

bool ahci_should_coalesce(struct generic_disk *disk,
                          const struct bio_request *a,
                          const struct bio_request *b);

void ahci_reorder(struct generic_disk *disk);
