#include <asm.h>
#include <console/printf.h>
#include <devices/ahci.h>
#include <devices/ata.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <s_assert.h>
#include <sleep.h>
#include <string.h>

// TODO: Hand this off to IDE if the GHC bit 31 is OFF
// It won't be AHCI - Sometimes we are in IDE emul mode

uint32_t find_free_cmd_slot(struct ahci_port *port) {
    uint32_t slots_in_use = port->sact | port->ci;
    for (int slot = 0; slot < 32; slot++) {
        if ((slots_in_use & (1U << slot)) == 0) {
            return slot; // free slot found
        }
    }

    return -1;
}

// TODO: big time reorganization

void ahci_print_ctrlr(struct ahci_controller *ctrl) {
    uint32_t version_major = (ctrl->vs >> 16) & 0xFFFF;
    uint32_t version_minor = ctrl->vs & 0xFFFF;
    uint32_t cap_np = (ctrl->cap >> 8) & 0x1F;

    k_printf("AHCI Controller Information:\n");
    k_printf("  Version: %d.%d\n", version_major, version_minor);
    k_printf("  Ports Implemented: 0x%08X\n", ctrl->pi);
    k_printf("  Number of Ports: %d\n", cap_np + 1);
    k_printf("  Capabilities: 0x%08X\n", ctrl->cap);

    bool s64a = !!(ctrl->cap & (1U << 31));
    if (!s64a) {
        k_printf("AHCI controller does not support 64-bit addressing\n");
        return;
    }

    while ((ctrl->ghc & AHCI_GHC_HR) != 0)
        ;
    ctrl->ghc |= AHCI_GHC_AE;

    struct ahci_device *dev = kzalloc(sizeof(struct ahci_device));

    uint32_t pi = ctrl->pi;
    for (uint32_t i = 0; i < 32; i++) {
        if (pi & (1U << i)) {
            uint32_t ssts = ctrl->ports[i].ssts;
            if ((ssts & 0x0F) == AHCI_DET_PRESENT &&
                ((ssts >> 8) & 0x0F) == AHCI_IPM_ACTIVE) {
                struct ahci_port *port = &ctrl->ports[i];
                port->cmd &= ~AHCI_CMD_ST;
                port->cmd &= ~AHCI_CMD_FRE;

                uint64_t cmdlist_phys = (uint64_t) pmm_alloc_page(false);
                uint64_t fis_phys = (uint64_t) pmm_alloc_page(false);
                void *cmdlist = vmm_map_phys(cmdlist_phys, 4096);
                void *fis = vmm_map_phys(fis_phys, 4096);
                memset(cmdlist, 0, 4096);
                memset(fis, 0, 4096);

                port->clb = cmdlist_phys & 0xFFFFFFFFUL;
                port->clbu = cmdlist_phys >> 32;
                port->fb = fis_phys & 0xFFFFFFFFUL;
                port->fbu = fis_phys >> 32;

                port->cmd |= AHCI_CMD_FRE;
                port->cmd |= AHCI_CMD_ST;

                while (port->cmd & AHCI_CMD_CR)
                    ;

                k_printf("Port %d is active\n", i);
                struct ahci_cmd_table **arr =
                    kzalloc(sizeof(struct ahci_cmd_table *) * 32);

                struct ahci_cmd_header **hdrr =
                    kzalloc(sizeof(struct ahci_cmd_header *) * 32);
                for (int slot = 0; slot < 32; slot++) {
                    uint64_t cmdtbl_phys = (uint64_t) pmm_alloc_page(false);
                    void *cmdtbl_virt = vmm_map_phys(cmdtbl_phys, 4096);
                    memset(cmdtbl_virt, 0, 4096);

                    struct ahci_cmd_header *cmd_header =
                        (struct ahci_cmd_header
                             *) (cmdlist +
                                 slot * sizeof(struct ahci_cmd_header));
                    cmd_header->ctba = (uint32_t) (cmdtbl_phys & 0xFFFFFFFF);
                    cmd_header->ctbau = (uint32_t) (cmdtbl_phys >> 32);
                    cmd_header->prdtl = 1;
                    arr[slot] = cmdtbl_virt;
                    hdrr[slot] = cmd_header;
                }
                struct ahci_full_port p = {.port = port,
                                           .fis = fis,
                                           .cmd_list_base = cmdlist,
                                           .cmd_tables = arr,
                                           .cmd_hdrs = hdrr};
                dev->regs[i] = p;
            }
        }
    }

    // We know port 0 is always going to be available for now
    struct ahci_full_port *port = &dev->regs[0];
    // TODO: Each AHCI SATA disk gets its own port
    uint32_t slot = find_free_cmd_slot(port->port);
    struct ahci_cmd_header *hdr = port->cmd_hdrs[slot];
    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);
    void *buffer = vmm_map_phys(buffer_phys, 4096);
    hdr->cfl = sizeof(struct ahci_fis_reg_h2d) / sizeof(uint32_t);
    hdr->w = 0;
    hdr->p = 0;
    hdr->a = 0;
    hdr->c = 1;
    hdr->prdbc = 0;
    cmd_tbl->prdt_entry[0].dba = (uint32_t) (buffer_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t) (buffer_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc = 511;
    cmd_tbl->prdt_entry[0].i = 1;
    struct ahci_fis_reg_h2d *fis = (struct ahci_fis_reg_h2d *) cmd_tbl->cfis;
    memset(fis, 0, sizeof(struct ahci_fis_reg_h2d));
    fis->fis_type = FIS_TYPE_REG_H2D; // 0x27
    fis->c = 1;                       // Command (not control)
    fis->command = 0xEC;
    port->port->is = (uint32_t) -1; // Clear pending interrupt bits
    port->port->ci |= 1 << slot;   // go go go! 
    while (port->port->ci & (1 << slot)) {}
    struct ata_identify *ident = buffer;
    ata_ident_print(ident);
}
