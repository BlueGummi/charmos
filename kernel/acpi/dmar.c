#include <acpi/dmar.h>
#include <asm.h>
#include <console/panic.h>
#include <console/printf.h>
#include <drivers/iommu/vt_d.h>
#include <drivers/mmio.h>
#include <log.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <uacpi/tables.h>

#include "uacpi/acpi.h"
#include "uacpi/status.h"

LOG_HANDLE_DECLARE_PRINT(dmar);
LOG_SITE_DECLARE_PRINT(dmar);

static void print_device_scopes(const uint8_t *ptr, const uint8_t *end) {
    while (ptr < end) {
        const struct acpi_dmar_dss *sc = (const void *) ptr;
        if (sc->length < 6 || ptr + sc->length > end)
            break;

        printf("scope: type=%d start_bus=%02x", sc->type, sc->start_bus);

        size_t path_entries = (sc->length - 6) / 2;
        const uint8_t *path = ptr + sizeof(*sc);
        for (size_t i = 0; i < path_entries; i++)
            printf(" %02x.%x", path[i * 2] >> 3, path[i * 2] & 0x7);

        if (sc->type == ACPI_DSS_IOAPIC ||
            sc->type == ACPI_DSS_MSI_CAPABLE_HPET)
            printf(" id=%u", sc->enumeration_id);

        ptr += sc->length;
        printf("\n");
    }
}

static void handle_drhd(const struct acpi_dmar_drhd *drhd) {
    bool include_all = (drhd->flags & ACPI_INCLUDE_PCI_ALL) != 0;
    uint32_t reg_size = 1u << (drhd->size + 12);

    dmar_info("DRHD base=0x%016llx size=0x%x seg=%u %s", drhd->address,
              reg_size, drhd->segment, include_all ? "(INCLUDE_ALL)" : "");

    struct vtd_regs *regs = mmio_map(drhd->address, PAGE_SIZE);

    uint64_t cap = regs->capabilities;
    uint64_t ecap = regs->extended_capabilities;

    dmar_info("  VT-d ver=%u.%u domains=%u", (regs->version >> 4) & 0xF,
              regs->version & 0xF, vtd_cap_domain_count(cap));

    dmar_info("  CAP:  SAGAW=%02x MGAW=%u FRO=0x%x NFR=%u CM=%u RWBF=%u",
              CAP_SUPPORTED_ADDR_WIDTHS(cap), CAP_MAX_ADDR_WIDTH(cap),
              CAP_FAULT_RECORD_OFFSET(cap), CAP_NUM_FAULT_RECORDS(cap),
              CAP_CACHING_MODE(cap), CAP_REQUIRES_WRITE_BUF_FLUSH(cap));

    dmar_info("  ECAP: QI=%u IR=%u PT=%u SC=%u C=%u IRO=0x%x",
              ECAP_QUEUED_INVALIDATION(ecap), ECAP_INTERRUPT_REMAPPING(ecap),
              ECAP_PASS_THROUGH(ecap), ECAP_SNOOP_CONTROL(ecap),
              ECAP_COHERENCY(ecap), ECAP_IOTLB_REGISTER_OFFSET(ecap));

    if (!(CAP_SUPPORTED_ADDR_WIDTHS(cap) & ADDR_WIDTH_48BIT))
        dmar_warn("  WARNING: 4-level page tables not supported by this unit!");

    const uint8_t *scope_end = (const uint8_t *) drhd + drhd->hdr.length;
    print_device_scopes((void *) drhd->entries, scope_end);

    struct iommu *unit =
        vtd_unit_create(drhd->address, drhd->segment, drhd->size);
    if (!unit) {
        dmar_warn("failed to create VT-d unit for DRHD at 0x%llx",
                  drhd->address);
        return;
    }
}

static void handle_rmrr(const struct acpi_dmar_rmrr *rmrr) {
    dmar_info("RMRR base=0x%016llx limit=0x%016llx seg=%u\n", rmrr->base,
              rmrr->limit, rmrr->segment);

    const uint8_t *scope_ptr = (const uint8_t *) rmrr + sizeof(*rmrr);
    const uint8_t *scope_end = (const uint8_t *) rmrr + rmrr->hdr.length;
    print_device_scopes(scope_ptr, scope_end);
}

static void handle_atsr(const struct acpi_dmar_atsr *atsr) {
    dmar_info((atsr->flags & 1) ? "(ALL_PORTS)" : "");
}

bool dmar_init(void) {
    struct uacpi_table dmar_table;
    if (uacpi_table_find_by_signature("DMAR", &dmar_table) != UACPI_STATUS_OK) {
        return false;
    }

    const struct acpi_dmar *dmar = dmar_table.ptr;
    uint8_t haw = dmar->haw + 1;

    dmar_info("DMAR VT-d DMAR table found");
    dmar_info("DMAR Host address width: %u bits", haw);
    dmar_info("DMAR Flags: 0x%02x%s", dmar->flags,
              (dmar->flags & 1) ? " (INTR_REMAP supported)" : "");

    const uint8_t *ptr = (const uint8_t *) (dmar + 1);
    const uint8_t *end = (const uint8_t *) dmar + dmar->hdr.length;

    size_t drhd_count = 0, rmrr_count = 0, atsr_count = 0, unknown_count = 0;

    while (ptr < end) {
        const struct acpi_dmar_entry_hdr *rs = (const void *) ptr;

        /* guard: malformed table */
        if (rs->length < 4 || ptr + rs->length > end) {
            log_warn_global(
                LOG_HANDLE(dmar),
                "malformed remapping struct at offset %zu (len=%u), stopping",
                (size_t) (ptr - (const uint8_t *) dmar), rs->length);
            break;
        }

        switch (rs->type) {
        case ACPI_DMAR_ENTRY_TYPE_DRHD:
            drhd_count++;
            handle_drhd((const struct acpi_dmar_drhd *) rs);
            break;

        case ACPI_DMAR_ENTRY_TYPE_RMRR:
            rmrr_count++;
            handle_rmrr((const struct acpi_dmar_rmrr *) rs);
            break;

        case ACPI_DMAR_ENTRY_TYPE_ATSR:
            atsr_count++;
            handle_atsr((const struct acpi_dmar_atsr *) rs);
            break;

        case ACPI_DMAR_ENTRY_TYPE_RHSA:
            dmar_info("RHSA (NUMA affinity hint, skipped)");
            break;

        case ACPI_DMAR_ENTRY_TYPE_ANDD:
            dmar_info("ANDD (ACPI namespace decl, skipped)");
            break;

        default:
            unknown_count++;
            dmar_info("??? unknown type=%u len=%u", rs->type, rs->length);
            break;
        }

        ptr += rs->length;
    }

    dmar_info("DMAR Summary: %d DRHD, %d RMRR, %d ATSR, %d unknown", drhd_count,
              rmrr_count, atsr_count, unknown_count);

    if (drhd_count == 0) {
        log_warn_global(LOG_HANDLE(dmar),
                        "no DRHD units found, nothing to initialize");
    }

    return true;
}
