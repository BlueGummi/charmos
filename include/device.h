/* @title: Device Structure */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <structures/rbt.h>
#include <sync/mutex.h>
#include <sync/spinlock.h>
#include <types/types.h>

#define DEVICE_MAX_RESOURCES 8

struct device;
enum device_physical_location_panel {
    DEVICE_PANEL_TOP,
    DEVICE_PANEL_BOTTOM,
    DEVICE_PANEL_LEFT,
    DEVICE_PANEL_RIGHT,
    DEVICE_PANEL_FRONT,
    DEVICE_PANEL_BACK,
    DEVICE_PANEL_UNKNOWN,
};

enum device_physical_location_vertical_position {
    DEVICE_VERT_POS_UPPER,
    DEVICE_VERT_POS_CENTER,
    DEVICE_VERT_POS_LOWER,
};

enum device_physical_location_horizontal_position {
    DEVICE_HORI_POS_LEFT,
    DEVICE_HORI_POS_CENTER,
    DEVICE_HORI_POS_RIGHT,
};

struct device_physical_location {
    enum device_physical_location_panel panel;
    enum device_physical_location_vertical_position vertical_position;
    enum device_physical_location_horizontal_position horizontal_position;
    bool dock;
    bool lid;
};

enum device_bus_type {
    DEVICE_BUS_TYPE_PLATFORM, /* pseudo-bus for SoC / board-level devices  */
    DEVICE_BUS_TYPE_PCI,
    DEVICE_BUS_TYPE_USB,
    DEVICE_BUS_TYPE_I2C,
    DEVICE_BUS_TYPE_SPI,
    DEVICE_BUS_TYPE_VIRTIO,
    DEVICE_BUS_TYPE_ACPI,
};

enum device_type {
    DEVICE_TYPE_UNKNOWN,
    DEVICE_TYPE_BLOCK,
    DEVICE_TYPE_CHAR,
    DEVICE_TYPE_NET,
    DEVICE_TYPE_INPUT,
    DEVICE_TYPE_DISPLAY,
    DEVICE_TYPE_AUDIO,
    DEVICE_TYPE_SERIAL,
    DEVICE_TYPE_GPU,
    DEVICE_TYPE_TIMER,
    DEVICE_TYPE_BUS, /* bridge / host controller                  */
};

enum device_power_state {
    DEVICE_POWER_D0,      /* fully operational                         */
    DEVICE_POWER_D1,      /* light sleep (clock-gated)                 */
    DEVICE_POWER_D2,      /* deeper sleep (power-gated, context kept)  */
    DEVICE_POWER_D3_HOT,  /* off, aux power, software-recoverable      */
    DEVICE_POWER_D3_COLD, /* off, no power, full re-init required      */
    DEVICE_POWER_REMOVED, /* hot-unplugged                             */
};

enum device_state {
    DEVICE_STATE_UNBOUND,   /* in the tree but no driver attached    */
    DEVICE_STATE_PROBING,   /* driver probe() in progress            */
    DEVICE_STATE_BOUND,     /* driver matched and probe() succeeded  */
    DEVICE_STATE_SUSPENDED, /* low-power, context saved              */
    DEVICE_STATE_REMOVING,  /* being torn down                       */
    DEVICE_STATE_REMOVED,   /* gone, awaiting final release          */
    DEVICE_STATE_ERROR,     /* unrecoverable fault                   */
};

enum device_flags {
    DEVICE_FLAG_HOTPLUG = (1u << 0),   /* supports hot-plug/remove    */
    DEVICE_FLAG_DMA = (1u << 1),       /* DMA-capable                 */
    DEVICE_FLAG_MMIO = (1u << 2),      /* memory-mapped I/O           */
    DEVICE_FLAG_PIO = (1u << 3),       /* port I/O                    */
    DEVICE_FLAG_MSI = (1u << 4),       /* MSI / MSI-X capable         */
    DEVICE_FLAG_VIRTUAL = (1u << 5),   /* software-only / emulated    */
    DEVICE_FLAG_REMOVABLE = (1u << 6), /* user-removable              */
    DEVICE_FLAG_WAKEUP = (1u << 7),    /* can wake system from sleep  */
};

struct device_driver {
    const char *name;
    enum device_bus_type bus;

    enum err (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
    enum err (*suspend)(struct device *dev, enum device_power_state target);
    enum err (*resume)(struct device *dev);
};

enum device_resource_type {
    DEVICE_RESOURCE_MEM,
    DEVICE_RESOURCE_IO,
    DEVICE_RESOURCE_IRQ,
    DEVICE_RESOURCE_DMA,
};

struct device_resource {
    enum device_resource_type type;
    uint64_t start;
    uint64_t size; /* length in bytes / count for IRQ,DMA   */
    uint32_t flags;
};

struct device_ops {
    void (*print)(struct device *);
};

struct device {
    const char *name;
    uint32_t id; /* unique device-model ID       */
    enum device_type type;
    enum device_bus_type bus;

    struct device *parent;
    struct list_head children;  /* Children list */
    struct rbt_node tree_node;  /* global device tree indexing  */
    struct list_node list_node; /* List node to attach to parent 'children' */

    struct device_ops *ops;
    struct device_driver *driver;
    void *driver_data; /* opaque, owned by the driver  */

    enum device_state state;
    enum device_power_state power;
    enum device_flags flags;
    refcount_t refcount;

    struct device_resource resources[DEVICE_MAX_RESOURCES];
    uint8_t resource_count;

    numa_node_t numa_node;
    struct device_physical_location location;

    struct iommu_domain *iommu_domain;

    struct mutex mutex;
    struct spinlock lock;

    void *private;
};

void device_init(struct device *dev);
