/* @title: ACPI CPU */
#pragma once
#include <acpi/acpi.h>
#include <compiler.h>
#include <types/types.h>

/* This is the big struct we gotta declare in here: */
struct acpi_cpu;

#define ACPI_CPU_BUSY_METRIC 10

#define ACPI_CPU_MAX_POWER 8
#define ACPI_CPU_MAX_C2_LATENCY 100
#define ACPI_CPU_MAX_C3_LATENCY 1000

#define ACPI_CPU_MAX_THROTTLING 16
#define ACPI_CPU_MAX_THROTTLE 100 /* 10% */
#define ACPI_CPU_MAX_DUTY_WIDTH 4

#define ACPI_PDC_REVISION_ID 0x1

#define ACPI_PSD_REV0_REVISION 0
#define ACPI_PSD_REV0_ENTRIES 5

#define ACPI_TSD_REV0_REVISION 0
#define ACPI_TSD_REV0_ENTRIES 5

#define ACPI_CONTEXT_DESCRIPTOR_LENGTH 32

struct acpi_power_reg {
    uint8_t descriptor;
    uint16_t length;
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t addr;
} __packed;

struct acpi_cpu_context {
    bool valid;
    uint8_t type;
    uint32_t addr;
    uint8_t entry_method;
    uint8_t index;
    uint32_t latency;
    uint8_t bm_status_skip;
    char desc[ACPI_CONTEXT_DESCRIPTOR_LENGTH];
};

struct acpi_lpi_state {
    uint32_t min_residency;
    uint32_t wake_latency;
    uint32_t flags;
    uint32_t resolution_counter_frequency;
    uint32_t enable_parent_state;
    uint64_t addr;
    uint8_t index;
    uint8_t entry_method;
    char desc[ACPI_CONTEXT_DESCRIPTOR_LENGTH];
};

struct acpi_cpu_power {
    size_t count;
    union {
        struct acpi_cpu_context states[ACPI_CPU_MAX_POWER];
        struct acpi_lpi_state lpi_states[ACPI_CPU_MAX_POWER];
    };
    size_t timer_broadcast_on_state;
};

struct acpi_psd_pkg {
    uint64_t num_entries;
    uint64_t revision;
    uint64_t domain;
    uint64_t coord_type;
    uint64_t num_cpus;
} __packed;

struct acpi_pct_reg {
    uint8_t desc;
    uint16_t len;
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t reserved0;
    uint64_t addr;
} __packed;

struct acpi_cpu_px {
    uint64_t frequency; /* megahertz */
    uint64_t power;     /* milliwatts */
    uint64_t transition_latency_us;
    uint64_t bus_master_latency_us;
    uint64_t control;
    uint64_t status;
};

struct acpi_cpu_perf {
    uint32_t state;
    uint32_t platform_limit;
    struct acpi_pct_reg control_reg;
    struct acpi_pct_reg status_reg;
    uint32_t state_count;
    struct acpi_cpu_px *states;
    struct acpi_psd_pkg domain_info;
    uint32_t type;
};

struct acpi_tsd_pkg {
    uint64_t num_entries;
    uint64_t revision;
    uint64_t domain;
    uint64_t coord_type;
    uint64_t num_cpus;
} __packed;

struct acpi_ptc_reg {
    uint8_t desc;
    uint16_t len;
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t reserved0;
    uint64_t addr;
} __packed;

struct acpi_cpu_tx_tss {
    uint64_t freq_pct;
    uint64_t power; /* milliwatts */
    uint64_t transition_latency_us;
    uint64_t control;
    uint64_t status;
};

struct acpi_cpu_tx {
    uint16_t power;
    uint16_t performance;
};

struct acpi_cpu_throttling {
    uint32_t state;
    uint32_t platform_limit;
    struct acpi_pct_reg control_reg;
    struct acpi_pct_reg status_reg;
    uint32_t state_count;
    struct acpi_cpu_tx_tss *states_tss;
    struct acpi_tsd_pkg domain_info;
    int32_t (*get_throttling)(struct acpi_cpu *);
    int32_t (*set_throttling)(struct acpi_cpu *, int32_t);
    uint32_t addr;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t tsd_valid_flag;
    uint32_t shared_type;
    struct acpi_cpu_tx states[ACPI_CPU_MAX_THROTTLING];
};

struct acpi_cpu_lx {
    int32_t px; /* perf state */
    int32_t tx; /* throttle level */
};

struct acpi_cpu_limit {
    struct acpi_cpu_lx state;   /* what are we right now */
    struct acpi_cpu_lx thermal; /* thermal limit */
    struct acpi_cpu_lx user;    /* limit the user wants */
};

struct acpi_cpu_flags {
    uint8_t power : 1;
    uint8_t perf : 1;
    uint8_t throttling : 1;
    uint8_t limit : 1;
    uint8_t has_cst : 1;
    uint8_t has_lpi : 1;
    uint8_t setup : 1;
};

struct acpi_cpu {
    cpu_id_t id;
    int32_t perf_throttle_limit;
    int32_t throttle_platform_limit;
    struct acpi_cpu_flags flags;
    struct acpi_cpu_power power;
    struct acpi_cpu_perf *performance;
    struct acpi_cpu_throttling throttling;
    struct acpi_cpu_limit limit;
};
