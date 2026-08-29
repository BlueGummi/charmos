#include <errno.h>
#include <math/sort.h>

static int cmp_facility_prefix(const void *key, const void *elem) {
    uint16_t pref = *(const uint16_t *) key;
    const struct err_facility *f = elem;
    return (pref > f->prefix) - (pref < f->prefix);
}

/* bsearch __skernel_err_facilities to __ekernel_err_facilities */
static struct err_facility *facility_for(uint16_t pref) {
    size_t count = __ekernel_err_facilities - __skernel_err_facilities;
    return bsearch(&pref, __skernel_err_facilities, count,
                   sizeof(struct err_facility), cmp_facility_prefix);
}

const char *errno_facility_to_str(enum errno e) {
    uint16_t pref = ERR_GET_FACILITY(e);
    uint16_t del = ERR_GET_DELTA(e);
    kassert(pref && del);
    struct err_facility *this = kassert(facility_for(pref));
    if (this->to_str)
        return this->to_str(del);

    return NULL;
}

void err_facilities_init() {
    kassert(__ekernel_err_facilities - __skernel_err_facilities <= UINT16_MAX,
            "too many?");

    /* Simple: 1 + index in array, keeps it sorted, avoids 0 */
    for (struct err_facility *f = __skernel_err_facilities;
         f < __ekernel_err_facilities; f++)
        f->prefix = (f - __skernel_err_facilities) + 1;
}
