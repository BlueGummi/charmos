#include <bootstage_condition.h>
#include <console/printf.h>
#include <dbg.h>
#include <linker/symbol_table.h>
#include <linker/symbols.h>
#include <log.h>
#include <mem/alloc_or_die.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <structures/locked_list.h>
#include <thread/thread.h>
#include <time/time.h>

#define LOG_IMPORTANT_RETRY 32
LOG_SITE_DECLARE(global, .flags = LOG_SITE_DEFAULT,
                 .capacity = LOG_SITE_CAPACITY_DEFAULT,
                 .dump_opts = LOG_DUMP_CONSOLE, .enabled_mask = LOG_SITE_ALL);

LOG_HANDLE_DECLARE(global, .flags = LOG_PRINT);

struct log_globals {
    struct locked_list list;
};

struct log_globals log_global = {0};

/* NULL until the blob has been stamped, which only an unpatched image should
 * ever hit. The table cannot go stale any more: it is written into the linked
 * kernel after the fact, so it always describes this exact image */
static const struct kernel_syms_hdr *syms_header(void) {
    const struct kernel_syms_hdr *hdr = (const void *) kernel_syms_blob;

    if (hdr->magic != KERNEL_SYMS_MAGIC || !hdr->count)
        return NULL;

    return hdr;
}

static void syms_warn_if_missing(void) {
    if (syms_header())
        return;

    printf("  <no symbol table: .kernel_syms was never stamped, "
           "rebuild to symbolize>\n");
}

static const char *find_symbol(uint64_t addr, uint64_t *out_sym_addr) {
    const struct kernel_syms_hdr *hdr = syms_header();

    if (out_sym_addr)
        *out_sym_addr = 0;

    if (!hdr)
        return NULL;

    const struct kernel_sym *tab = (const void *) (hdr + 1);
    const char *strtab = kernel_syms_blob + hdr->strtab_off;

    /* Sorted by address, so bisect for the last entry at or below addr. Worth
     * it here: this runs from panic paths where a scan of every symbol is the
     * last thing we want */
    uint32_t lo = 0, hi = hdr->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (tab[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (!lo)
        return NULL;

    const struct kernel_sym *best = &tab[lo - 1];

    if (out_sym_addr)
        *out_sym_addr = best->addr;

    return strtab + best->name_off;
}

static void k_printf_from_log(const char *fmt, const uint64_t *args,
                              uint8_t nargs, void (*print)(const char *, ...)) {
    switch (nargs) {
    case 0: print(fmt); break;
    case 1: print(fmt, args[0]); break;
    case 2: print(fmt, args[0], args[1]); break;
    case 3: print(fmt, args[0], args[1], args[2]); break;
    case 4: print(fmt, args[0], args[1], args[2], args[3]); break;
    case 5: print(fmt, args[0], args[1], args[2], args[3], args[4]); break;
    case 6:
        print(fmt, args[0], args[1], args[2], args[3], args[4], args[5]);
        break;
    case 7:
        print(fmt, args[0], args[1], args[2], args[3], args[4], args[5],
              args[6]);
        break;
    case 8:
        print(fmt, args[0], args[1], args[2], args[3], args[4], args[5],
              args[6], args[7]);
        break;
    default: print("<invalid nargs>");
    }
}

static void log_dump_record(const struct log_site *site,
                            const struct log_record *rec,
                            const struct log_dump_options opts,
                            void (*print)(const char *f, ...)) {
    size_t sec = MS_TO_SECONDS(rec->timestamp);
    size_t msec = rec->timestamp % 1000;
    if (sec == 0 && msec == 0) {
        print("[X.XXX] %s%s%s: ", log_level_color(rec->level), site->name,
              ANSI_RESET);
    } else {
        if (!rec->handle->print) {
            print("[%llu.%03llu] %s%s%s: ", sec, msec,
                  log_level_color(rec->level), site->name, ANSI_RESET);
        } else {
            rec->handle->print(site, rec, print);
        }
    }

    if (opts.show_cpu)
        print("cpu=%u ", rec->cpu);

    if (opts.show_tid)
        print("tid=%u ", rec->tid);

    if (opts.show_irql)
        print("irql=%d ", rec->logged_at_irql);

    /* message */
    if (opts.show_args && rec->fmt) {
        k_printf_from_log(rec->fmt, rec->args, rec->nargs, print);
    } else if (rec->handle && rec->handle->msg) {
        print("%s", rec->handle->msg);
    }

    if (opts.show_caller) {
        print(" <+ at %s()", rec->caller_fn);
    }

    if (!(rec->handle->flags & LOG_NO_NEWLINE))
        print("\n");
}

static void log_dump_record_locked(const struct log_site *site,
                                   const struct log_record *rec,
                                   const struct log_dump_options opts) {
    enum irql irql = printf_lock();
    log_dump_record(site, rec, opts, printf_unlocked);
    printf_unlock(irql);
}

static inline bool log_ringbuf_try_enqueue(struct log_site *site,
                                           struct log_ringbuf *rb,
                                           const struct log_record *rec) {
    uint64_t pos;
    struct log_ring_slot *slot;

    while (true) {
        pos = atomic_load_explicit(&rb->head, memory_order_relaxed);
        slot = &rb->slots[pos % site->capacity];

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

static inline bool log_ringbuf_try_dequeue(struct log_site *site,
                                           struct log_ringbuf *rb,
                                           struct log_record *out) {
    uint64_t pos;
    struct log_ring_slot *slot;

    while (true) {
        pos = atomic_load_explicit(&rb->tail, memory_order_relaxed);
        slot = &rb->slots[pos % site->capacity];

        uint64_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) (pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&rb->tail, &pos, pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {

                *out = slot->rec;

                atomic_store_explicit(&slot->seq, pos + site->capacity,
                                      memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;
        }
    }
}

static bool log_ringbuf_force_enqueue(struct log_site *site,
                                      struct log_ringbuf *rb,
                                      const struct log_record *rec) {
    struct log_record dummy;
    log_ringbuf_try_dequeue(site, rb, &dummy);
    return log_ringbuf_try_enqueue(site, rb, rec);
}

void log_dump_site_with_opts(struct log_site *site,
                             struct log_dump_options opts) {
    struct log_record rec;
    if (!site || !log_site_get(site))
        return;

    enum irql irql = printf_lock();
    while (log_ringbuf_try_dequeue(site, &site->rb, &rec)) {
        if (rec.level < opts.min_level)
            continue;

        if (site->dropped) {
            printf_unlocked("!! dropped %u log records !!\n", site->dropped);
        }

        log_dump_record(site, &rec, opts, printf_unlocked);
    }
    printf_unlock(irql);

    log_site_put(site);
}

void log_dump_site_default(struct log_site *site) {
    if (!site || !log_site_get(site))
        return;

    log_dump_site_with_opts(site, LOG_DUMP_DEFAULT);
    log_site_put(site);
}

void log_dump_site(struct log_site *site) {
    log_dump_site_with_opts(site, site->dump_opts);
}

void log_emit_internal(struct log_site *site, struct log_handle *handle,
                       enum log_level ll, const char *func, const char *file,
                       int32_t line, uintptr_t ip, uint8_t narg, char *fmt,
                       ...) {
    if (!site || !log_site_get(site))
        return;

    enum log_level level = ll;
    struct log_record rec = {0};
    rec.handle = handle;
    rec.level = level;
    rec.fmt = fmt;
    rec.caller_pc = ip;
    rec.caller_fn = (char *) func;
    rec.caller_file = (char *) file;
    rec.caller_line = line;

    /* pack args */
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; i < narg && i < 8; i++) {
        rec.args[i] = va_arg(ap, uint64_t);
        rec.nargs++;
    }
    va_end(ap);

    struct log_dump_options dopts = site->dump_opts;

    BOOTSTAGE_IF_LT(BOOTSTAGE_LATE) {
        if (log_handle_should_print(handle, site, level))
            return log_dump_record(site, &rec, dopts, printf);
    }

    if (!log_site_accepts(site) ||
        (site->flags & LOG_SITE_NO_IRQ && irq_in_interrupt()))
        return;

    if (!log_site_enabled(site, level))
        return;

    rec.timestamp = time_get_ms();
    rec.cpu = smp_core_id();
    rec.tid = thread_get_current()->id;
    rec.logged_at_irql = irql_get();

    if (irq_in_interrupt())
        rec.flags |= LOG_REC_FROM_IRQ;

    if (handle->flags & LOG_ONCE) {
        if (atomic_fetch_add(&handle->seen_internal, 1) != 0)
            return;
    }

    if (handle->flags & LOG_RATELIMIT) {
        uint64_t now = rec.timestamp;
        uint64_t last = atomic_load(&handle->last_ts_internal);

        if (now - last < 100)
            return;

        atomic_store(&handle->last_ts_internal, now);
    }

    bool queued = log_ringbuf_try_enqueue(site, &site->rb, &rec);

    if (!queued) {
        if (handle->flags & LOG_IMPORTANT) {
            if (site->flags & LOG_SITE_DROP_OLD) {
                queued = log_ringbuf_force_enqueue(site, &site->rb, &rec);
            } else if (!irq_in_interrupt()) {
                for (int i = 0; i < LOG_IMPORTANT_RETRY; i++) {
                    if (log_ringbuf_try_enqueue(site, &site->rb, &rec)) {
                        queued = true;
                        break;
                    }
                    cpu_relax();
                }
            }

            if (!queued) {
                /* last-resort visibility */
                log_dump_record_locked(site, &rec, dopts);
            }
        } else {
            site->dropped++;
        }
    }

    if (log_handle_should_print(handle, site, level)) {
        log_dump_record_locked(site, &rec, dopts);
    }

    if ((ll & LOG_PANIC) && level >= LOG_ERROR) {
        log_dump_all();
        debug_print_stack();
        panic("fatal log event");
    }

    log_site_put(site);
}

void log_sites_init(void) {
    locked_list_init(&log_global.list, LOCKED_LIST_INIT_NORMAL);

    for (struct log_site *s = __skernel_log_sites; s < __ekernel_log_sites;
         s++) {
        INIT_LIST_HEAD(&s->list);
        s->enabled = true;
        refcount_init(&s->refcount, 1);
        struct log_ringbuf *lrb = &s->rb;
        kassert(s->capacity);
        lrb->slots = kmalloc_or_die(sizeof(struct log_ring_slot) * s->capacity,
                                    ALLOC_FLAGS_ZERO);

        for (size_t i = 0; i < s->capacity; i++) {
            atomic_store_explicit(&lrb->slots[i].seq, i, memory_order_release);
        }

        locked_list_add(&log_global.list, &s->list);
    }
}

void log_dump_all(void) {
    enum irql irql = spin_lock_irq_disable(&log_global.list.lock);

    struct log_site *site;
    list_for_each_entry(site, &log_global.list.list, list) {
        log_dump_site_default(site);
    }

    spin_unlock(&log_global.list.lock, irql);
}

void log_site_free(struct log_site *site) {
    locked_list_del(&log_global.list, &site->list);
    kfree(site->rb.slots);
    kfree(site->name);
    kfree_aligned(site, _Alignof(struct log_site));
}

struct log_site *log_site_create(struct log_site_options opts) {
    struct log_site *ret = kmalloc_aligned(
        sizeof(struct log_site), _Alignof(struct log_site), ALLOC_FLAGS_ZERO);
    if (!ret)
        return NULL;

    ret->name = strdup(opts.name);
    if (!ret->name)
        goto err;

    struct log_ring_slot *slots =
        kmalloc(sizeof(struct log_ring_slot) * opts.capacity, ALLOC_FLAGS_ZERO);
    if (!slots)
        goto err;

    ret->dump_opts = opts.dump_opts;
    ret->enabled_mask = opts.enabled_mask;
    ret->capacity = opts.capacity;
    ret->rb.slots = slots;
    refcount_init(&ret->refcount, 1);
    ret->dropped = 0;
    ret->flags = opts.flags;
    INIT_LIST_HEAD(&ret->list);
    ret->enabled = true;
    for (size_t i = 0; i < opts.capacity; i++) {
        atomic_store_explicit(&slots[i].seq, i, memory_order_release);
    }

    locked_list_add(&log_global.list, &ret->list);

    return ret;

err:

    if (ret) {
        kfree(ret->rb.slots);
        kfree(ret->name);
    }

    kfree_aligned(ret, _Alignof(struct log_site));
    return NULL;
}

static bool stack_addr_readable(uint64_t addr) {
    return vmm_get_phys(PAGE_ALIGN_DOWN(addr), VMM_FLAG_NONE) != (uintptr_t) -1;
}

/* Frame holds caller's saved rbp at [0] and ret addr at [1], so both
 * qwords have to be there */
static bool stack_frame_readable(uint64_t frame) {
    if (!frame || (frame & (sizeof(uint64_t) - 1)))
        return false;

    return stack_addr_readable(frame) &&
           stack_addr_readable(frame + sizeof(uint64_t));
}

static bool stack_addr_is_text(uint64_t addr) {
    return addr >= (uint64_t) &__stext && addr < (uint64_t) &__etext;
}

/* Use the rbp chain that -fno-omit-frame-pointer builds, and collect
 * them all into `entries`, returning the number of entries found */
size_t stack_unwind(uint64_t frame, uint64_t *entries, size_t max) {
    size_t nr = 0;

    if (max > STACK_TRACE_MAX_DEPTH)
        max = STACK_TRACE_MAX_DEPTH;

    while (nr < max) {
        if (!stack_frame_readable(frame))
            break;

        uint64_t next = ((const uint64_t *) frame)[0];
        uint64_t ret = ((const uint64_t *) frame)[1];

        if (!stack_addr_is_text(ret))
            break;

        if (entries)
            entries[nr] = ret;

        nr++;
        if (next <= frame)
            break;

        frame = next;
    }

    return nr;
}

void debug_print_stack_trace(const uint64_t *entries, size_t nr) {
    if (!nr) {
        printf("  <no kernel frames found>\n");
        return;
    }

    syms_warn_if_missing();

    for (size_t i = 0; i < nr; i++) {
        uint64_t sym_addr;
        const char *sym = find_symbol(entries[i], &sym_addr);

        if (sym) {
            printf("    #%-2zu [0x%016lx] %s+0x%lx\n", i, entries[i], sym,
                   entries[i] - sym_addr);
        } else {
            printf("    #%-2zu [0x%016lx] <unknown>\n", i, entries[i]);
        }
    }
}

void debug_print_stack(void) {
    uint64_t entries[STACK_TRACE_MAX_DEPTH];

    size_t nr = stack_unwind((uint64_t) __builtin_frame_address(0), entries,
                             sizeof(entries) / sizeof(*entries));

    debug_print_stack_trace(entries, nr);
}

void debug_print_memory(void *addr, uint64_t size) {
    uint8_t *ptr = (uint8_t *) addr;
    printf("Memory at %p:\n", (uint64_t) addr);
    for (uint64_t i = 0; i < size; i++) {
        if (i % 16 == 0) {
            if (i != 0)
                printf("\n");
            printf("%p: ", (uint64_t) (ptr + i));
        }
        printf("%02x ", ptr[i]);
    }
    printf("\n");
}

void debug_print_stack_from(uint64_t *start, size_t max_scan) {
    int hits = 0;
    uint8_t *last_checked_page = NULL;

    if (!max_scan)
        max_scan = 64 * 1024;

    syms_warn_if_missing();

    printf("Stack unwind from %p:\n", (uint64_t) start);

    for (size_t offset = 0; offset < max_scan; offset += sizeof(uint64_t)) {
        uint8_t *addr = (uint8_t *) start + offset;
        uint8_t *page_base = (uint8_t *) PAGE_ALIGN_DOWN(addr);

        if (page_base != last_checked_page) {
            if (vmm_get_phys((vaddr_t) page_base, VMM_FLAG_NONE) ==
                (uintptr_t) -1)
                break;
            last_checked_page = page_base;
        }

        uint64_t val = *(uint64_t *) addr;

        if (val >= 0xffffffff80000000ULL && val <= 0xffffffffffffffffULL) {
            uint64_t sym_addr;
            const char *sym = find_symbol(val, &sym_addr);
            if (sym) {
                printf("    [0x%016lx] %s+0x%lx (sp=0x%016lx)\n", val, sym,
                       val - sym_addr, (uint64_t) addr);
                hits++;
            }
        }
    }

    if (hits == 0)
        printf("  <no kernel symbols found>\n");
}
