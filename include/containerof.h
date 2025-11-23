/* @title: container_of macro */
#define container_of(ptr, type, member)                                        \
    ((type *) ((char *) (ptr) - offsetof(type, member)))
