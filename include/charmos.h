#include <boot/stage.h>
#include <fs/vfs.h>
#include <stdatomic.h>
#pragma once

struct charmos_globals {
    char *root_partition;
    struct vfs_mount *mount_list_head;
    struct vfs_node *root_node;
    volatile enum bootstage current_bootstage;
    uint64_t core_count;
    struct scheduler **schedulers;
    struct core **cores;
    atomic_uint_fast64_t next_tlb_gen;

    volatile bool panic_in_progress;
};

extern struct charmos_globals global;
