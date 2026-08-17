#include <errno.h>

/* bsearch __skernel_err_facilities to __ekernel_err_facilities */
static struct err_facility *facility_for(uint16_t pref) {
    struct err_facility *facilities = __ekernel_err_facilities;
    size_t count = __ekernel_err_facilities - __skernel_err_facilities;

    size_t left = 0;
    size_t right = count - 1;

    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        uint16_t mid_fac = facilities[mid].prefix;

        if (mid_fac == pref) {
            return &facilities[mid];
        } else if (mid_fac < pref) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return NULL;
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
