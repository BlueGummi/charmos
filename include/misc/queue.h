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

#define queue_remove(q, t, val)                                                \
    typeof(q->head) prev = NULL;                                               \
    typeof(q->head) curr = q->head;                                            \
                                                                               \
    while (curr) {                                                             \
        if (curr == t) {                                                       \
            if (prev) {                                                        \
                prev->next = curr->next;                                       \
            } else {                                                           \
                q->head = curr->next;                                          \
            }                                                                  \
                                                                               \
            if (q->tail == curr) {                                             \
                q->tail = prev;                                                \
            }                                                                  \
            val = true;                                                        \
            break;                                                             \
        }                                                                      \
                                                                               \
        prev = curr;                                                           \
        curr = curr->next;                                                     \
    }
