/* @title: Alignment Macros */
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1ULL))
#define ALIGN_UP(x, align) (((x) + ((align) - 1ULL)) & ~((align) - 1ULL))
