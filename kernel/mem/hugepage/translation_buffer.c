#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>

static inline bool tb_entry_on_cooldown(struct hugepage_tb *htb,
                                        struct hugepage_tb_entry *e) {
    return e->valid && (htb->gen_counter - e->gen) < HTB_COOLDOWN_TICKS;
}

static inline bool tb_entry_matches_addr(struct hugepage_tb_entry *e,
                                         vaddr_t addr) {
    return e->valid && (e->tag == (addr & HTB_TAG_MASK));
}

struct hugepage *hugepage_tb_lookup(struct hugepage_tb *htb, vaddr_t addr) {
    size_t idx = hugepage_tb_hash(addr, htb);
    struct hugepage_tb_entry *e = &htb->entries[idx];

    if (tb_entry_matches_addr(e, addr))
        return e->hp;

    return NULL;
}

bool hugepage_tb_ent_exists(struct hugepage_tb *htb, vaddr_t addr) {
    return hugepage_tb_lookup(htb, addr) ? true : false;
}

void hugepage_tb_remove(struct hugepage_tb *htb, struct hugepage *hp) {
    vaddr_t addr = hp->virt_base;
    size_t idx = hugepage_tb_hash(addr, htb);
    struct hugepage_tb_entry *e = &htb->entries[idx];
    if (!e->valid)
        return;

    bool iflag = hugepage_tb_entry_lock(e);

    e->valid = false;
    e->hp = NULL;
    e->tag = 0x0;
    e->gen = 0;

    hugepage_tb_entry_unlock(e, iflag);
}

bool hugepage_tb_insert(struct hugepage_tb *htb, struct hugepage *hp) {
    vaddr_t addr = hp->virt_base;
    size_t idx = hugepage_tb_hash(addr, htb);
    struct hugepage_tb_entry *e = &htb->entries[idx];

    bool i = hugepage_tb_entry_lock(e);

    /* Cooldown avoids thrashing, and there's no
     * need to re-insert the same hugepage */
    if (tb_entry_on_cooldown(htb, e) || e->hp == hp) {
        hugepage_tb_entry_unlock(e, i);
        return false;
    }

    e->tag = addr & HTB_TAG_MASK;
    e->hp = hp;
    e->valid = true;
    e->gen = htb->gen_counter++;
    hugepage_tb_entry_unlock(e, i);
    return true;
}

struct hugepage_tb *hugepage_tb_init(size_t size) {
    struct hugepage_tb *htb = kzalloc(sizeof(struct hugepage_tb));
    if (!htb)
        return NULL;

    size = size > HTB_MAX_ENTRIES ? HTB_MAX_ENTRIES : size;
    htb->gen_counter = 0;
    htb->entry_count = size;
    htb->entries = kzalloc(sizeof(struct hugepage_tb_entry) * size);
    return htb;
}
