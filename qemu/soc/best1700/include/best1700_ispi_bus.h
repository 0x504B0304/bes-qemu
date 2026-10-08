#ifndef BEST1700_ISPI_BUS_H
#define BEST1700_ISPI_BUS_H

#include "hw/sysbus.h"
#include "best1700_ispi.h"

#define TYPE_BEST1700_ISPI_BUS "best1700-ispi-bus"
#define TYPE_BEST1700_ISPI_SLAVE "best1700-ispi-slave"
OBJECT_DECLARE_TYPE(Best1700IspiSlave, Best1700IspiSlaveClass,
                    BEST1700_ISPI_SLAVE)

struct Best1700IspiSlave {
    DeviceState parent_obj;
    uint32_t chip_select;
    qemu_irq irq;
};

struct Best1700IspiSlaveClass {
    DeviceClass parent_class;
    bool (*read)(Best1700IspiSlave *dev, unsigned page, unsigned reg,
                 uint16_t *value);
    bool (*write)(Best1700IspiSlave *dev, unsigned page, unsigned reg,
                  uint16_t value);
    void (*write_observed)(Best1700IspiSlave *dev, unsigned page, unsigned reg,
                           uint16_t value);
};

/* The port shares the same transaction state used by AP and CP MMIO. */
void best1700_ispi_create_bus(Best1700Ispi *controller, qemu_irq irq);
void best1700_ispi_slave_realize(DeviceState *dev, Error **errp);
void best1700_ispi_slave_unrealize(DeviceState *dev);

#endif
