#define queue_pop_front(q, var)                                                \
    typeof(q->head) var = q->head;                                             \
    if (var) {                                                                 \
        q->head = t->next;                                                     \
        if (q->head == NULL) {                                                 \
            q->tail = NULL;                                                    \
        }                                                                      \
        var->next = NULL;                                                      \
    }

#define queue_push_back(q, thing)                                              \
    thing->next = NULL;                                                        \
    if (q->tail) {                                                             \
        q->tail->next = thing;                                                 \
    } else {                                                                   \
        q->head = thing;                                                       \
    }                                                                          \
    q->tail = thing;
