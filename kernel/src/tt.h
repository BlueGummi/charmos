/*
#define make_task(id, sauce, terminate)                                        \
    void task##id() {                                                          \
        while (1) {                                                            \
            k_printf("task %d says %s\n", id, sauce);                          \
            for (int i = 0; i < 50; i++)                                       \
                asm("hlt");                                                    \
            if (terminate)                                                     \
                scheduler_rm_id(&global_sched, t3_id);                         \
        }                                                                      \
    }
make_task(1, "MAYOOOO", true);
make_task(2, "MUSTAAARD", false);
make_task(3, "KETCHUUUP", false);
make_task(4, "RAAAANCH", false);
make_task(5, "SAUERKRAAAUUUT", false);

#define make_mp_task(id, os)                                                   \
    void task_mp##id() {                                                       \
        k_printf("multiprocessor task %d says %s\n", id, os);                  \
        while (1)                                                              \
            asm("cli;hlt");                                                    \
    }

make_mp_task(1, "linux");
make_mp_task(2, "macos");
make_mp_task(3, "freebsd");
make_mp_task(4, "openbsd");
make_mp_task(5, "solaris");
*/
