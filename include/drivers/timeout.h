#pragma once

#define AHCI_CMD_TIMEOUT_MS 5000    // Data commands (read/write)
#define AHCI_IDENT_TIMEOUT_MS 10000 // Identify, flush cache, etc.
#define AHCI_RESET_TIMEOUT_MS 30000 // Full controller reset or COMRESET

#define IDE_IDENT_TIMEOUT_MS 10000 // Identify or cache flush
#define IDE_RESET_TIMEOUT_MS 30000 // Controller reset or spin-up

#define ATAPI_CMD_TIMEOUT_MS 8000     // Short packet commands
#define ATAPI_SPINUP_TIMEOUT_MS 15000 // Spinning up disc or loading tray
#define ATAPI_EJECT_TIMEOUT_MS 20000  // Eject/load tray or seek disc

#define NVME_CMD_TIMEOUT_MS 2000    // Normal command timeout
#define NVME_ADMIN_TIMEOUT_MS 5000  // Admin commands
#define NVME_RESET_TIMEOUT_MS 30000 // Controller reset or format NVM
