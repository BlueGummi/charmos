#include <console/printf.h>
#include <linker/symbol_table.h>
#include <log.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <structures/locked_list.h>
#include <thread/thread.h>
#include <time.h>

#define LOG_IMPORTANT_RETRY 32

struct log_dump_opts log_dump_default = {
    .min_level = LOG_TRACE,
    .show_args = true,
    .show_cpu = true,
    .show_tid = true,
    .show_irql = true,
    .show_caller = true,
    .resolve_symbols = true,
    .clear_after_dump = false,
};

LOG_SITE_DECLARE(global, LOG_SITE_DEFAULT, LOG_SITE_CAPACITY_DEFAULT,
                 LOG_SITE_ALL);
LOG_HANDLE_DECLARE(global, LOG_DEFAULT);

struct log_globals {
    struct locked_list list;
};

static void log_site_free(struct log_site *site);
static const char *log_level_str[] = {"TRACE", "DEBUG", "INFO", "WARN",
                                      "ERROR"};

struct log_globals log_global = {0};

static struct log_dump_opts log_dump_console = {
    .min_level = LOG_TRACE,
    .show_args = true,
    .show_cpu = false,
    .show_tid = false,
    .show_irql = false,
    .show_caller = false,
    .resolve_symbols = false,
    .clear_after_dump = false,
};

static bool log_site_get(struct log_site *site) {
    return refcount_inc(&site->refcount);
}

static void log_site_put(struct log_site *site) {
    if (refcount_dec_and_test(&site->refcount))
        log_site_free(site);
}

void log_site_destroy(struct log_site *site) {
    log_site_put(site);
}

static const char *find_symbol(uint64_t addr, uint64_t *out_sym_addr) {
    const char *result = NULL;
    uint64_t best = 0;

    for (uint64_t i = 0; i < syms_len; i++) {
        if (syms[i].addr <= addr && syms[i].addr > best) {
            best = syms[i].addr;
            result = syms[i].name;
        }
    }

    if (out_sym_addr)
        *out_sym_addr = best;

    return result;
}

static void k_printf_from_log(const char *fmt, const uint64_t *args,
                              uint8_t nargs) {
    switch (nargs) {
    case 0: k_printf(fmt); break;
    case 1: k_printf(fmt, args[0]); break;
    case 2: k_printf(fmt, args[0], args[1]); break;
    case 3: k_printf(fmt, args[0], args[1], args[2]); break;
    case 4: k_printf(fmt, args[0], args[1], args[2], args[3]); break;
    case 5: k_printf(fmt, args[0], args[1], args[2], args[3], args[4]); break;
    case 6:
        k_printf(fmt, args[0], args[1], args[2], args[3], args[4], args[5]);
        break;
    case 7:
        k_printf(fmt, args[0], args[1], args[2], args[3], args[4], args[5],
                 args[6]);
        break;
    case 8:
        k_printf(fmt, args[0], args[1], args[2], args[3], args[4], args[5],
                 args[6], args[7]);
        break;
    default: k_printf("<invalid nargs>");
    }
}

static void log_dump_record(const struct log_record *rec,
                            const struct log_dump_opts *opts) {
    size_t sec = MS_TO_SECONDS(rec->timestamp);
    size_t msec = rec->timestamp % 1000;
    k_printf("[%llu.%03llu] %s%-5s%s ", sec, msec, log_level_color(rec->level),
             log_level_str[rec->level], ANSI_RESET);

    if (opts->show_cpu)
        k_printf("cpu=%u ", rec->cpu);

    if (opts->show_tid)
        k_printf("tid=%u ", rec->tid);

    if (opts->show_irql)
        k_printf("irql=%d ", rec->logged_at_irql);

    /* message */
    if (opts->show_args && rec->fmt) {
        k_printf_from_log(rec->fmt, rec->args, rec->nargs);
    } else if (rec->handle && rec->handle->msg) {
        k_printf("%s", rec->handle->msg);
    }

    k_printf("\n");

    if (opts->show_caller) {
        uint64_t sym_addr;
        const char *sym = find_symbol(rec->caller_ip, &sym_addr);
        if (sym) {
            k_printf("    at %s+0x%lx\n", sym, rec->caller_ip - sym_addr);
        } else {
            k_printf("    at 0x%lx\n", rec->caller_ip);
        }
    }
}

static inline bool log_ringbuf_try_enqueue(struct log_ringbuf *rb,
                                           const struct log_record *rec) {
    uint64_t pos;
    struct log_ring_slot *slot;

    while (true) {
        pos = atomic_load_explicit(&rb->head, memory_order_relaxed);
        slot = &rb->slots[pos % rb->capacity];

        uint64_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&rb->head, &pos, pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {

                slot->rec = *rec;

                atomic_store_explicit(&slot->seq, pos + 1,
                                      memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;
        }
    }
}

static inline bool log_ringbuf_try_dequeue(struct log_ringbuf *rb,
                                           struct log_record *out) {
    uint64_t pos;
    struct log_ring_slot *slot;

    while (true) {
        pos = atomic_load_explicit(&rb->tail, memory_order_relaxed);
        slot = &rb->slots[pos % rb->capacity];

        uint64_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) (pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&rb->tail, &pos, pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {

                *out = slot->rec;

                atomic_store_explicit(&slot->seq, pos + rb->capacity,
                                      memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;
        }
    }
}

static bool log_ringbuf_force_enqueue(struct log_ringbuf *rb,
                                      const struct log_record *rec) {
    struct log_record dummy;
    log_ringbuf_try_dequeue(rb, &dummy);
    return log_ringbuf_try_enqueue(rb, rec);
}

void log_dump_site(struct log_site *site, struct log_dump_opts *opts) {
    struct log_record rec;
    if (!site || !log_site_get(site))
        return;

    while (log_ringbuf_try_dequeue(&site->rb, &rec)) {
        if (rec.level < opts->min_level)
            continue;

        if (site->dropped) {
            k_printf("!! dropped %u log records !!\n", site->dropped);
        }

        log_dump_record(&rec, opts);
    }

    log_site_put(site);
}

void log_dump_site_default(struct log_site *site) {
    if (!site || !log_site_get(site))
        return;

    log_dump_site(site, &log_dump_default);
    log_site_put(site);
}

void log_console_emit(const struct log_record *rec) {
    log_dump_record(rec, &log_dump_console);
}

void log_emit_internal(struct log_site *site, struct log_handle *handle,
                       enum log_level ll, uintptr_t ip, uint8_t narg, char *fmt,
                       ...) {
    if (!site || !log_site_get(site))
        return;

    enum log_level level = ll;
    struct log_record rec = {0};
    rec.handle = handle;
    rec.level = level;
    rec.fmt = fmt;
    rec.caller_ip = ip;

    /* pack args */
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; i < narg && i < 8; i++) {
        rec.args[i] = va_arg(ap, uint64_t);
        rec.nargs++;
    }
    va_end(ap);

    if (global.current_bootstage < BOOTSTAGE_LATE)
        if (log_handle_should_print(handle, site, level))
            return log_console_emit(&rec);

    if (!log_site_accepts(site) ||
        (site->flags & LOG_SITE_NO_IRQ && irq_in_interrupt()))
        return;

    if (!log_site_enabled(site, level))
        return;

    rec.timestamp = time_get_ms();
    rec.cpu = smp_core_id();
    rec.tid = scheduler_get_current_thread()->id;
    rec.logged_at_irql = irql_get();

    if (irq_in_interrupt())
        rec.flags |= LOG_REC_FROM_IRQ;

    if (handle->flags & LOG_ONCE) {
        if (atomic_fetch_add(&handle->seen, 1) != 0)
            return;
    }

    if (handle->flags & LOG_RATELIMIT) {
        uint64_t now = rec.timestamp;
        uint64_t last = atomic_load(&handle->last_ts);

        if (now - last < 100)
            return;

        atomic_store(&handle->last_ts, now);
    }

    bool queued = log_ringbuf_try_enqueue(&site->rb, &rec);

    if (!queued) {
        if (handle->flags & LOG_IMPORTANT) {

            if (site->flags & LOG_SITE_DROP_OLD) {
                queued = log_ringbuf_force_enqueue(&site->rb, &rec);
            } else if (!irq_in_interrupt()) {
                for (int i = 0; i < LOG_IMPORTANT_RETRY; i++) {
                    if (log_ringbuf_try_enqueue(&site->rb, &rec)) {
                        queued = true;
                        break;
                    }
                    cpu_relax();
                }
            }

            if (!queued) {
                /* last-resort visibility */
                log_console_emit(&rec);
            }
        } else {
            site->dropped++;
            return;
        }
    }

    if (log_handle_should_print(handle, site, level)) {
        log_console_emit(&rec);
    }

    if ((ll & LOG_PANIC) && level >= LOG_ERROR) {
        log_dump_all();
        debug_print_stack();
        k_panic("fatal log event");
    }

    log_site_put(site);
}

void log_sites_init(void) {
    locked_list_init(&log_global.list);

    for (struct log_site *s = __skernel_log_sites; s < __ekernel_log_sites;
         s++) {
        INIT_LIST_HEAD(&s->list);
        s->enabled = true;
        refcount_init(&s->refcount, 1);
        struct log_ringbuf *lrb = &s->rb;
        lrb->slots = kzalloc(sizeof(struct log_ring_slot) * lrb->capacity,
                             ALLOC_PARAMS_DEFAULT);
        if (!lrb->slots)
            k_panic("OOM\n");

        for (size_t i = 0; i < lrb->capacity; i++) {
            atomic_store_explicit(&lrb->slots[i].seq, i, memory_order_release);
        }

        locked_list_add(&log_global.list, &s->list);
    }
}

void log_dump_all(void) {
    enum irql irql = spin_lock_irq_disable(&log_global.list.lock);

    struct log_site *site;
    list_for_each_entry(site, &log_global.list.list, list) {
        log_dump_site(site, &log_dump_default);
    }

    spin_unlock(&log_global.list.lock, irql);
}

static void log_site_free(struct log_site *site) {
    locked_list_del(&log_global.list, &site->list);
    kfree(site->rb.slots, FREE_PARAMS_DEFAULT);
    kfree(site->name, FREE_PARAMS_DEFAULT);
    kfree(site, FREE_PARAMS_DEFAULT);
}

struct log_site *log_site_create(char *name, enum log_site_flags flags,
                                 size_t capacity) {
    struct log_site *ret =
        kzalloc(sizeof(struct log_site), ALLOC_PARAMS_DEFAULT);
    if (!ret)
        return NULL;

    ret->name = strdup(name);
    if (!ret->name)
        goto err;

    struct log_ring_slot *slots =
        kzalloc(sizeof(struct log_ring_slot) * capacity, ALLOC_PARAMS_DEFAULT);
    if (!slots)
        goto err;

    ret->rb.capacity = capacity;
    ret->rb.slots = slots;
    refcount_init(&ret->refcount, 1);
    ret->dropped = 0;
    ret->flags = flags;
    INIT_LIST_HEAD(&ret->list);
    ret->enabled = true;
    for (size_t i = 0; i < capacity; i++) {
        atomic_store_explicit(&slots[i].seq, i, memory_order_release);
    }

    locked_list_add(&log_global.list, &ret->list);

    return ret;

err:

    if (ret) {
        kfree(ret->rb.slots, FREE_PARAMS_DEFAULT);
        kfree(ret->name, FREE_PARAMS_DEFAULT);
    }

    kfree(ret, FREE_PARAMS_DEFAULT);
    return NULL;
}

void debug_print_stack(void) {
    uint64_t *rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));

    uint64_t *p = rsp;
    int hits = 0;

    const size_t MAX_SCAN = 64 * 1024;

    uint8_t *last_checked_page = NULL;

    for (size_t offset = 0; offset < MAX_SCAN; offset += sizeof(uint64_t)) {
        uint8_t *addr = (uint8_t *) p + offset;

        uint8_t *page_base = (uint8_t *) PAGE_ALIGN_DOWN(addr);
        if (page_base != last_checked_page) {
            if (vmm_get_phys_unsafe((vaddr_t) page_base) == (uintptr_t) -1) {
                break;
            }
            last_checked_page = page_base;
        }

        uint64_t val = *(uint64_t *) addr;
        if (val >= 0xffffffff80000000ULL && val <= 0xffffffffffffffffULL) {
            uint64_t sym_addr;
            const char *sym = find_symbol(val, &sym_addr);
            if (sym) {
                k_printf("    [0x%016lx] %s+0x%lx (sp=0x%016lx)\n", val, sym,
                         val - sym_addr, (uint64_t) addr);
                hits++;
            }
        }
    }

    if (hits == 0)
        k_printf("  <no kernel symbols found>\n");
}

void debug_print_memory(void *addr, uint64_t size) {
    uint8_t *ptr = (uint8_t *) addr;
    k_printf("Memory at 0x%lx:\n", (uint64_t) addr);
    for (uint64_t i = 0; i < size; i++) {
        if (i % 16 == 0) {
            if (i != 0)
                k_printf("\n");
            k_printf("0x%lx: ", (uint64_t) (ptr + i));
        }
        k_printf("%02x ", ptr[i]);
    }
    k_printf("\n");
}
