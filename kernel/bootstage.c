#include <bootstage.h>
#include <bootstage_condition.h>
#include <console/printf.h>
#include <global.h>
#include <log.h>
#include <rw_once.h>
#include <string.h>

/* The standard bootstage.h API and the fancy condition stuff are both here */

static LOG_HANDLE_DECLARE_PRINT(bootstage);
const char *bootstage_str[BOOTSTAGE_COUNT] = {
    [BOOTSTAGE_NONE] = "None",
    [BOOTSTAGE_EARLY_FB] = "Early - Framebuffer",
    [BOOTSTAGE_EARLY_ALLOCATORS] = "Early - Allocators",
    [BOOTSTAGE_EARLY_DEVICES] = "Early - Devices",
    [BOOTSTAGE_MID_MP] = "Mid - SMP",
    [BOOTSTAGE_MID_TOPOLOGY] = "Mid - Topology",
    [BOOTSTAGE_MID_ALLOCATORS] = "Mid - Allocators",
    [BOOTSTAGE_LATE] = "Late",
    [BOOTSTAGE_COMPLETE] = "Complete",
};

enum bootstage bootstage_get() {
    return global.current_bootstage;
}

static void bootstage_write_jump(struct bootstage_condition_entry *ent) {
    vaddr_t vjump = (vaddr_t) ent->code;
    vaddr_t vdest = (vaddr_t) ent->target;
    vaddr_t rel32ptr = vjump + 1;       /* A byte over */
    uint32_t rel32 = vdest - vjump - 5; /* For the jump insn */

    /* Write 0xE9 to the jump place, then the rel32 dest one byte over LE */
    WRITE_ONCE(*(uint8_t *) ent->code, (uint8_t) 0xE9);
    WRITE_ONCE(*(uint32_t *) rel32ptr, (uint32_t) rel32);
}

static void bootstage_write_nop(struct bootstage_condition_entry *ent) {
    uint8_t *vjump = ent->code;

    /* ingenuity */
    const uint8_t nop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    memcpy(vjump, nop5, 5);
}

static bool bootstage_taken(struct bootstage_condition_entry *ent,
                            enum bootstage bs) {
    enum bootstage ebs = ent->stage;
    enum bootstage_condition cond = ent->cond;
    switch (cond) {
    case BOOTSTAGE_CONDITION_EQ: return bs == ebs;
    case BOOTSTAGE_CONDITION_LE: return bs <= ebs;
    case BOOTSTAGE_CONDITION_LT: return bs < ebs;
    case BOOTSTAGE_CONDITION_GE: return bs >= ebs;
    case BOOTSTAGE_CONDITION_GT: return bs > ebs;
    default: kassert_unreachable("invalid bootstage_condition_entry condition");
    }
}

static void bootstage_patch_all(enum bootstage bs) {
    struct bootstage_condition_entry *ent;
    linker_section_for_each_object(ent, bootstage_condition_entries) {
        if (bootstage_taken(ent, bs)) {
            bootstage_write_jump(ent);
        } else {
            bootstage_write_nop(ent);
        }
    }
}

void bootstage_advance(enum bootstage new) {
    /* disable interrupts to be safe */
    bool ints = are_interrupts_enabled();
    disable_interrupts();

    global.current_bootstage = new;
    atomic_thread_fence(memory_order_seq_cst);

    /* EARLY_FB leaves the kernel RO, we don't have patchability */
    if (new > BOOTSTAGE_EARLY_FB)
        bootstage_patch_all(new);

    if (ints)
        enable_interrupts();

    log_info_global(LOG_HANDLE(bootstage), "Reached bootstage \'%s\'",
                    bootstage_str[new]);
}
