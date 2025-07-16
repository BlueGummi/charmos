#define dll_add(q, thing)                                                      \
    thing->next = NULL;                                                        \
    thing->prev = NULL;                                                        \
    if (!q->head) {                                                            \
        q->head = thing;                                                       \
        q->tail = thing;                                                       \
        thing->next = thing;                                                   \
        thing->prev = thing;                                                   \
    } else {                                                                   \
        thing->prev = q->tail;                                                 \
        thing->next = q->head;                                                 \
        q->tail->next = thing;                                                 \
        q->head->prev = thing;                                                 \
        q->tail = thing;                                                       \
    }

#define dll_remove(q, thing)                                                   \
    if (q->head == q->tail && q->head == thing) {                              \
        q->head = NULL;                                                        \
        q->tail = NULL;                                                        \
    } else if (q->head == thing) {                                             \
        q->head = q->head->next;                                               \
        q->head->prev = q->tail;                                               \
        q->tail->next = q->head;                                               \
    } else if (q->tail == thing) {                                             \
        q->tail = q->tail->prev;                                               \
        q->tail->next = q->head;                                               \
        q->head->prev = q->tail;                                               \
    } else {                                                                   \
        typeof(thing) current = q->head->next;                                 \
        while (current != q->head && current != thing)                         \
            current = current->next;                                           \
        if (current == thing) {                                                \
            current->prev->next = current->next;                               \
            current->next->prev = current->prev;                               \
        }                                                                      \
    }                                                                          \
    thing->next = thing->prev = NULL;

#define dll_clear(q)                                                           \
    typeof(q->head) start = q->head;                                           \
    typeof(start) iter = start;                                                \
    do {                                                                       \
        typeof(iter->next) next = iter->next;                                  \
        iter->next = iter->prev = NULL;                                        \
        iter = next;                                                           \
    } while (iter != start);                                                   \
    q->head = NULL;
