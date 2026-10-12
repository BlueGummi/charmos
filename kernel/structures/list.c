#include <enforce.h>
#include <math/align.h>
#include <mem/address_range.h>
#include <structures/list.h>

#ifdef DEBUG_LIST

static bool list_link_is_plausible(const struct list_head *n) {
    uintptr_t v = (uintptr_t) n;

    return v >= ADDRESS_RANGE_KERNEL_START && IS_ALIGNED(v, sizeof(void *));
}

static bool list_node_looks_linked(const struct list_head *n) {
    if (n->next == NULL || n->prev == NULL)
        return false;

    if (n->next == n || n->prev == n)
        return false;

    if (!list_link_is_plausible(n->next) || !list_link_is_plausible(n->prev))
        return false;

    return n->next->prev == n && n->prev->next == n;
}

void list_add_check(const struct list_head *new, const struct list_head *prev,
                    const struct list_head *next) {
    if (cc_unlikely(new == NULL || prev == NULL || next == NULL))
        panic("list_add: NULL operand (new %p, prev %p, next %p)", new, prev,
              next);

    if (cc_unlikely(prev->next != next))
        panic("list_add corruption: prev %p ->next should be next %p, was %p",
              prev, next, prev->next);

    if (cc_unlikely(next->prev != prev))
        panic("list_add corruption: next %p ->prev should be prev %p, was %p",
              next, prev, next->prev);

    if (cc_unlikely(new == prev || new == next))
        panic("list_add: double add of %p (prev %p, next %p)", new, prev, next);

    if (cc_unlikely(list_node_looks_linked(new)))
        panic("list_add: %p is already linked (next %p, prev %p) -- adding it "
              "to %p would unlink it; swapped arguments?",
              new, new->next, new->prev, next);
}

void list_del_check(const struct list_head *entry) {
    if (cc_unlikely(entry == NULL))
        panic("list_del: NULL entry");

    if (cc_unlikely(entry->next == NULL || entry->prev == NULL))
        panic("list_del: %p already removed (next %p, prev %p)", entry,
              entry->next, entry->prev);

    if (cc_unlikely(entry->prev->next != entry))
        panic("list_del corruption: %p ->prev %p ->next should be %p, was %p",
              entry, entry->prev, entry, entry->prev->next);

    if (cc_unlikely(entry->next->prev != entry))
        panic("list_del corruption: %p ->next %p ->prev should be %p, was %p",
              entry, entry->next, entry, entry->next->prev);
}

void list_splice_check(const struct list_head *list,
                       const struct list_head *prev,
                       const struct list_head *next) {
    if (cc_unlikely(list == NULL || prev == NULL || next == NULL))
        panic("list_splice: NULL operand (list %p, prev %p, next %p)", list,
              prev, next);

    if (cc_unlikely(prev->next != next || next->prev != prev))
        panic("list_splice corruption at destination: prev %p ->next %p, "
              "next %p ->prev %p",
              prev, prev->next, next, next->prev);

    const struct list_head *first = list->next;
    const struct list_head *last = list->prev;

    if (cc_unlikely(first == NULL || last == NULL))
        panic("list_splice: source %p is not initialised (next %p, prev %p)",
              list, first, last);

    if (cc_unlikely(first->prev != list || last->next != list))
        panic("list_splice corruption at source %p: first %p ->prev %p, "
              "last %p ->next %p",
              list, first, first->prev, last, last->next);
}
#endif /* DEBUG_LIST */

void list_sort(struct list_head *head,
               int (*cmp)(struct list_head *, struct list_head *)) {
    if (list_empty(head) || head->next->next == head)
        return;

    struct list_head *list = head->next;
    head->prev->next = NULL;
    list->prev = NULL;

    int insize = 1;

    while (true) {
        struct list_head *p = list;
        list = NULL;
        struct list_head *tail = NULL;
        int nmerges = 0;

        while (p) {
            nmerges++;
            struct list_head *q = p;
            int psize = 0;
            for (int i = 0; i < insize; i++) {
                psize++;
                q = q->next;
                if (!q)
                    break;
            }

            int qsize = insize;

            /* Merge the two lists */
            while (psize > 0 || (qsize > 0 && q)) {
                struct list_head *e;

                if (psize == 0) {
                    e = q;
                    q = q->next;
                    qsize--;
                } else if (qsize == 0 || !q) {
                    e = p;
                    p = p->next;
                    psize--;
                } else if (cmp(p, q) <= 0) {
                    e = p;
                    p = p->next;
                    psize--;
                } else {
                    e = q;
                    q = q->next;
                    qsize--;
                }

                if (tail)
                    tail->next = e;
                else
                    list = e;

                e->prev = tail;
                tail = e;
            }

            p = q;
        }

        tail->next = NULL;

        if (nmerges <= 1)
            break;

        insize *= 2;
    }

    struct list_head *prev = head;
    struct list_head *curr = list;
    while (curr) {
        curr->prev = prev;
        prev->next = curr;
        prev = curr;
        curr = curr->next;
    }

    prev->next = head;
    head->prev = prev;
}

/* Example:
 *
 * int node_cmp(struct list_head *a, struct list_head *b) {
 *     struct node *na = list_entry(a, struct node, list);
 *     struct node *nb = list_entry(b, struct node, list);
 *     return (na->value > nb->value) - (na->value < nb->value);
 * }
 */
