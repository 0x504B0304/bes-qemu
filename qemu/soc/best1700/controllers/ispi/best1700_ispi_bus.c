/* Board endpoints on the shared AP/CP ISPI transport.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "best1700_ispi_bus.h"

#define TYPE_BEST1700_ISPI_PORT "best1700-ispi-port"
typedef struct Best1700IspiBus {
    BusState parent_obj;
    Best1700Ispi *controller;
} Best1700IspiBus;

typedef struct Best1700IspiPort {
    SysBusDevice parent_obj;
    Best1700IspiBus *bus;
    qemu_irq irq;
    uint16_t irq_levels;
} Best1700IspiPort;

static bool ispi_slave_read(void *opaque, unsigned page, unsigned reg,
                            uint16_t *value)
{
    Best1700IspiSlave *s = opaque;
    Best1700IspiSlaveClass *sc = BEST1700_ISPI_SLAVE_GET_CLASS(s);
    return sc->read && sc->read(s, page, reg, value);
}

static bool ispi_slave_write(void *opaque, unsigned page, unsigned reg,
                             uint16_t value)
{
    Best1700IspiSlave *s = opaque;
    Best1700IspiSlaveClass *sc = BEST1700_ISPI_SLAVE_GET_CLASS(s);
    return sc->write && sc->write(s, page, reg, value);
}

static void ispi_slave_write_observed(void *opaque, unsigned page, unsigned reg,
                                      uint16_t value)
{
    Best1700IspiSlave *s = opaque;
    Best1700IspiSlaveClass *sc = BEST1700_ISPI_SLAVE_GET_CLASS(s);
    if (sc->write_observed) {
        sc->write_observed(s, page, reg, value);
    }
}

void best1700_ispi_slave_realize(DeviceState *dev, Error **errp)
{
    Best1700IspiSlave *s = BEST1700_ISPI_SLAVE(dev);
    Best1700IspiBus *bus = (Best1700IspiBus *)qdev_get_parent_bus(dev);
    Best1700IspiEndpoint ep = {
        .opaque = s, .read = ispi_slave_read, .write = ispi_slave_write,
        .write_observed = ispi_slave_write_observed,
    };

    if (!bus || !bus->controller || s->chip_select >= BEST1700_ISPI_CS_COUNT) {
        error_setg(errp, "ISPI endpoint needs a controller and chip-select 0..15");
        return;
    }
    if (bus->controller->endpoints[s->chip_select].opaque) {
        error_setg(errp, "ISPI chip-select %u is occupied", s->chip_select);
        return;
    }
    best1700_ispi_connect_endpoint(bus->controller, s->chip_select, &ep);
    qdev_connect_gpio_out(dev, 0,
        qdev_get_gpio_in_named(bus->parent_obj.parent, "irq", s->chip_select));
}

void best1700_ispi_slave_unrealize(DeviceState *dev)
{
    Best1700IspiSlave *s = BEST1700_ISPI_SLAVE(dev);
    Best1700IspiBus *bus = (Best1700IspiBus *)qdev_get_parent_bus(dev);
    qemu_set_irq(s->irq, 0);
    best1700_ispi_connect_endpoint(bus->controller, s->chip_select, NULL);
}

static void ispi_slave_init(Object *obj)
{
    Best1700IspiSlave *s = BEST1700_ISPI_SLAVE(obj);
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
}

static Property ispi_slave_properties[] = {
    DEFINE_PROP_UINT32("chip-select", Best1700IspiSlave, chip_select, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void ispi_slave_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->bus_type = TYPE_BEST1700_ISPI_BUS;
    dc->hotpluggable = false;
    device_class_set_props(dc, ispi_slave_properties);
}

static void ispi_port_irq(void *opaque, int n, int level)
{
    Best1700IspiPort *s = opaque;
    s->irq_levels = (s->irq_levels & ~(1u << n)) | (!!level << n);
    qemu_set_irq(s->irq, s->irq_levels != 0);
}

static void ispi_port_reset(DeviceState *dev)
{
    Best1700IspiPort *s = (Best1700IspiPort *)dev;
    Best1700Ispi *controller = s->bus->controller;
    Best1700IspiEndpoint endpoints[BEST1700_ISPI_CS_COUNT];
    memcpy(endpoints, controller->endpoints, sizeof(endpoints));
    best1700_ispi_init(controller);
    memcpy(controller->endpoints, endpoints, sizeof(endpoints));
    s->irq_levels = 0;
    qemu_set_irq(s->irq, 0);
}

static void ispi_port_init(Object *obj)
{
    Best1700IspiPort *s = (Best1700IspiPort *)obj;
    s->bus = (Best1700IspiBus *)qbus_new(TYPE_BEST1700_ISPI_BUS, DEVICE(obj), "ispi");
    qdev_init_gpio_in_named(DEVICE(obj), ispi_port_irq, "irq", BEST1700_ISPI_CS_COUNT);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void ispi_port_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = ispi_port_reset;
    dc->user_creatable = false;
}

void best1700_ispi_create_bus(Best1700Ispi *controller, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_BEST1700_ISPI_PORT);
    Best1700IspiPort *s = (Best1700IspiPort *)dev;
    dev->id = g_strdup("best1700-ispi");
    s->bus->controller = controller;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
}

static const TypeInfo ispi_bus_types[] = {
    { .name = TYPE_BEST1700_ISPI_BUS, .parent = TYPE_BUS,
      .instance_size = sizeof(Best1700IspiBus) },
    { .name = TYPE_BEST1700_ISPI_PORT, .parent = TYPE_SYS_BUS_DEVICE,
      .instance_size = sizeof(Best1700IspiPort), .instance_init = ispi_port_init,
      .class_init = ispi_port_class_init },
    { .name = TYPE_BEST1700_ISPI_SLAVE, .parent = TYPE_DEVICE, .abstract = true,
      .instance_size = sizeof(Best1700IspiSlave),
      .class_size = sizeof(Best1700IspiSlaveClass),
      .instance_init = ispi_slave_init, .class_init = ispi_slave_class_init },
};
DEFINE_TYPES(ispi_bus_types)
