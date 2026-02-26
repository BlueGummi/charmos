#include <console/panic.h>
#include <console/printf.h>
#include <global.h>
#include <kassert.h>
#include <math/sort.h>
#include <mem/alloc.h>
#include <mem/numa.h>
#include <string.h>

void numa_dump(void) {
    size_t n = global.numa_node_count;
    if (!n) {
        log_msg(LOG_WARN, "No NUMA nodes detected");
        return;
    }

    log_msg(LOG_INFO, "NUMA distance matrix (%zu nodes):", n);
    log_msg(LOG_INFO, "Displayed as distance (relative distance):", n);

    printf("     ");
    for (size_t j = 0; j < n; j++)
        printf("%11zu", j);
    printf("\n");

    for (size_t i = 0; i < n; i++) {
        printf("%4zu: ", i);
        for (size_t j = 0; j < n; j++) {
            printf("%4u (%4u)", global.numa_nodes[i].distance[j],
                     global.numa_nodes[i].rel_dists[j]);
        }
        printf("\n");
    }
}

static int cmp(const void *a, const void *b) {
    return (*(uint8_t *) a) - (*(uint8_t *) b);
}

static uint8_t idx_of_val(uint8_t *buf, size_t len, size_t search_for) {
    for (size_t i = 0; i < len; i++)
        if (buf[i] == search_for)
            return i;

    kassert_unreachable();
    return 0;
}

void numa_construct_relative_distances(struct numa_node *node) {
    node->rel_dists = kzalloc(node->distances_cnt, ALLOC_PARAMS_DEFAULT);
    uint8_t *tmp = kzalloc(node->distances_cnt, ALLOC_PARAMS_DEFAULT);
    if (!node->rel_dists || !tmp)
        panic("could not allocate numa relative distances\n");

    /* we copy the distance array into the temporary array and
     * sort it. then, for each distance in the distance array,
     * we iterate through the temporary array and find its
     * position. this position is then set as the distance's rel_dist */
    memcpy(tmp, node->distance, node->distances_cnt);
    qsort(tmp, node->distances_cnt, sizeof(uint8_t), cmp);

    for (size_t i = 0; i < node->distances_cnt; i++) {
        node->rel_dists[i] =
            idx_of_val(tmp, node->distances_cnt, node->distance[i]);
    }

    kfree(tmp, FREE_PARAMS_DEFAULT);
}
