#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
