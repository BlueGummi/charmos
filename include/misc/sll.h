#define sll_add(q, thing)                                                      \
    if (!q->head) {                                                            \
        q->head = thing;                                                       \
        q->tail = thing;                                                       \
    } else {                                                                   \
        thing->next = NULL;                                                    \
        q->tail->next = thing;                                                 \
        q->tail = thing;                                                       \
    }
