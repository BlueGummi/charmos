/* @title: Global Variables */
#pragma once
#include <bootstage.h>
#include <fs/vfs.h>
#include <mem/buddy.h>
#include <mem/movealloc.h>
#include <mem/numa.h>
#include <smp/topology.h>
#include <stdatomic.h>
#include <structures/list.h>

/* TODO: almost everything here is RO after init.
 *
 * I can clone these per-NUMA node and make backpointers
 * via the per-core 'struct core' in the G segment register */
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

    atomic_size_t idle_core_count;
    size_t core_count;
    struct core **cores;
    struct tlb_shootdown_cpu *shootdown_data;
    struct scheduler **schedulers;
    struct dpc_cpu *dpc_data;

    size_t domain_count;
    struct domain **domains;
    struct slab_domain **slab_domains;
    struct domain_buddy *domain_buddies;
    struct page *page_array;
    struct free_area buddy_free_area[MAX_ORDER];

    bool scheduler_domains_ready;
    struct scheduler_domain *scheduler_domains[TOPOLOGY_LEVEL_MAX];

    vaddr_t hhdm_offset;

    uint64_t total_pages;
    paddr_t last_pfn;

    struct movealloc_callback_chain movealloc_chain;

    /* TODO: no more of this */
    atomic_uint_fast64_t next_tlb_gen;
    atomic_uint_fast64_t rcu_gen;

    /* Per core workqueues */
    struct workqueue **workqueues;

    /* Big ol' global one */
    struct workqueue *system_workqueue;

    struct turnstile_hash_table *turnstiles;

    /* Conditional compilation globals go down here */
#ifdef PROFILING_ENABLED
    struct list_head profiling_list_head;
#endif
};

extern struct charmos_globals global;
