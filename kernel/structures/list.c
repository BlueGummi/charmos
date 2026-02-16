#include <structures/list.h>

void list_sort(struct list_head *head,
               int (*cmp)(struct list_head *, struct list_head *)) {
    if (list_empty(head) || head->next->next == head)
        return;

    struct list_head *list = head->next;
    head->prev->next = NULL;
    list->prev = NULL;

    int insize = 1;

    while (1) {
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
