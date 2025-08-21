#include <boot/stage.h>
#include <fs/vfs.h>
#include <mem/numa.h>
#include <misc/list.h>
#include <mp/topology.h>
#include <stdatomic.h>
#pragma once

struct charmos_globals {
    volatile bool panic_in_progress;
    volatile enum bootstage current_bootstage;

    char *root_partition;

    /* TODO: no more list */
    struct vfs_mount *mount_list_head;
    struct vfs_node *root_node;
    struct generic_disk *root_node_disk;

    struct topology topology;
    size_t numa_node_count;
    struct numa_node *numa_nodes;
    uint64_t core_count;
    struct scheduler **schedulers;
    struct core **cores;
    uint64_t hhdm_offset;

    /* TODO: no more of this */
    atomic_uint_fast64_t next_tlb_gen;

    /* Conditional compilation globals go down here */
#ifdef PROFILING_ENABLED
    struct list_head profiling_list_head;
#endif
};

extern struct charmos_globals global;
