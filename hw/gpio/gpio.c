/*
 * 可由监控命令控制的独立 GPIO 寄存器组
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/vmstate-if.h"
#include "migration/vmstate.h"
#include "monitor/hmp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc.h"
#include "qobject/qdict.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/reset.h"

#define TYPE_GPIO "gpio"
OBJECT_DECLARE_SIMPLE_TYPE(GPIOState, GPIO)

struct GPIOState {
    DeviceState parent_obj;
    MemoryRegion mmio;
    QEMUTimer timer;
    uint64_t addr;
    uint32_t size;
    uint32_t gpio_mask;
    uint32_t write_mask;
    uint32_t default_value;
    uint32_t value;
};

static uint32_t gpio_width_mask(const GPIOState *s)
{
    return (uint32_t)((UINT64_C(1) << (s->size * 8)) - 1);
}

static void gpio_set_value(GPIOState *s, uint32_t value)
{
    s->value = (s->value & ~s->gpio_mask) | (value & s->gpio_mask);
}

static bool gpio_accepts(void *opaque, hwaddr addr, unsigned size,
                         bool is_write, MemTxAttrs attrs)
{
    GPIOState *s = opaque;

    return addr < s->size && size <= s->size - addr;
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    GPIOState *s = opaque;
    uint64_t mask = (UINT64_C(1) << (size * 8)) - 1;

    return (s->value >> (addr * 8)) & mask;
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t data,
                       unsigned size)
{
    GPIOState *s = opaque;
    unsigned shift = addr * 8;
    uint64_t access_mask = ((UINT64_C(1) << (size * 8)) - 1) << shift;
    uint32_t mask = (uint32_t)access_mask & s->write_mask;

    s->value = (s->value & ~mask) | ((data << shift) & mask);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
        .accepts = gpio_accepts,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void gpio_timer_expired(void *opaque)
{
    GPIOState *s = opaque;

    gpio_set_value(s, s->default_value);
}

void qmp_gpio(const char *id, uint32_t value, bool has_duration,
              uint32_t duration, Error **errp)
{
    Object *obj = object_resolve_path_component(
        machine_get_container("peripheral"), id);
    GPIOState *s;
    int64_t deadline = 0;

    if (!obj || !object_dynamic_cast(obj, TYPE_GPIO) ||
        !DEVICE(obj)->realized) {
        error_setg(errp, "Device '%s' is not a realized gpio device", id);
        return;
    }
    s = GPIO(obj);
    if (value & ~gpio_width_mask(s)) {
        error_setg(errp, "Value does not fit in the %u-byte GPIO register",
                   s->size);
        return;
    }
    if (has_duration) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t delay = (int64_t)duration * NANOSECONDS_PER_SECOND;

        if (!duration || now > INT64_MAX - delay) {
            error_setg(errp,
                       "Duration must be positive and fit in virtual time");
            return;
        }
        deadline = now + delay;
    }

    timer_del(&s->timer);
    gpio_set_value(s, value);
    if (has_duration) {
        timer_mod(&s->timer, deadline);
    }
}

void hmp_gpio(Monitor *mon, const QDict *qdict)
{
    const char *id = qdict_get_str(qdict, "id");
    const char *value_str = qdict_get_str(qdict, "value");
    const char *duration_str = qdict_get_try_str(qdict, "duration");
    uint64_t value;
    uint64_t duration = 0;
    Error *err = NULL;

    if (parse_uint_full(value_str, 0, &value) < 0 || value > UINT32_MAX) {
        error_setg(&err, "Value must be between 0 and 0xffffffff");
    } else if (duration_str &&
               (parse_uint_full(duration_str, 0, &duration) < 0 ||
                !duration || duration > UINT32_MAX)) {
        error_setg(&err, "Duration must be between 1 and %" PRIu32
                   " seconds", UINT32_MAX);
    } else {
        qmp_gpio(id, value, duration_str != NULL, duration, &err);
    }
    hmp_handle_error(mon, err);
}

static void gpio_reset_hold(Object *obj, ResetType type)
{
    GPIOState *s = GPIO(obj);

    timer_del(&s->timer);
    s->value = s->default_value;
}

static void gpio_realize(DeviceState *dev, Error **errp)
{
    GPIOState *s = GPIO(dev);
    MemoryRegionSection section;
    uint32_t width_mask;

    if (!dev->id) {
        error_setg(errp, "gpio requires an id");
        return;
    }
    if (s->addr == UINT64_MAX) {
        error_setg(errp, "gpio requires an addr");
        return;
    }
    if (s->size != 1 && s->size != 2 && s->size != 4) {
        error_setg(errp, "size must be 1, 2 or 4 bytes");
        return;
    }
    if (s->addr > UINT64_MAX - (s->size - 1) ||
        s->addr % s->size) {
        error_setg(errp, "addr must be aligned to size without range overflow");
        return;
    }

    width_mask = gpio_width_mask(s);
    if (s->gpio_mask == UINT32_MAX) {
        s->gpio_mask = width_mask;
    }
    if (!s->gpio_mask) {
        error_setg(errp, "gpio-mask must not be zero");
        return;
    }
    if ((s->gpio_mask | s->write_mask | s->default_value) & ~width_mask) {
        error_setg(errp, "gpio-mask, write-mask and default must fit in the "
                   "%u-byte register", s->size);
        return;
    }
    if (s->write_mask & ~s->gpio_mask) {
        error_setg(errp, "write-mask must be a subset of gpio-mask");
        return;
    }

    section = memory_region_find(get_system_memory(), s->addr, s->size);
    if (section.mr) {
        error_setg(errp, "GPIO range at 0x%" PRIx64
                   " overlaps region '%s'", s->addr,
                   memory_region_name(section.mr));
        memory_region_unref(section.mr);
        return;
    }

    s->value = s->default_value;
    memory_region_init_io(&s->mmio, OBJECT(s), &gpio_ops, s,
                          TYPE_GPIO, s->size);
    memory_region_add_subregion(get_system_memory(), s->addr, &s->mmio);
    /* 无总线设备不在系统总线的复位树中。 */
    qemu_register_resettable(OBJECT(s));
}

static void gpio_unrealize(DeviceState *dev)
{
    GPIOState *s = GPIO(dev);

    timer_del(&s->timer);
    qemu_unregister_resettable(OBJECT(s));
    memory_region_del_subregion(get_system_memory(), &s->mmio);
}

static void gpio_init(Object *obj)
{
    GPIOState *s = GPIO(obj);

    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, gpio_timer_expired, s);
}

static void gpio_finalize(Object *obj)
{
    GPIOState *s = GPIO(obj);

    timer_del(&s->timer);
    timer_deinit(&s->timer);
}

static char *gpio_vmstate_id(VMStateIf *obj)
{
    return g_strdup(DEVICE(obj)->id);
}

static const VMStateDescription vmstate_gpio = {
    .name = TYPE_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(addr, GPIOState, "addr differs"),
        VMSTATE_UINT32_EQUAL(size, GPIOState, "size differs"),
        VMSTATE_UINT32_EQUAL(gpio_mask, GPIOState, "gpio-mask differs"),
        VMSTATE_UINT32_EQUAL(write_mask, GPIOState, "write-mask differs"),
        VMSTATE_UINT32_EQUAL(default_value, GPIOState, "default differs"),
        VMSTATE_UINT32(value, GPIOState),
        VMSTATE_TIMER(timer, GPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property gpio_properties[] = {
    DEFINE_PROP_UINT64("addr", GPIOState, addr, UINT64_MAX),
    DEFINE_PROP_UINT32("size", GPIOState, size, 4),
    DEFINE_PROP_UINT32("gpio-mask", GPIOState, gpio_mask, UINT32_MAX),
    DEFINE_PROP_UINT32("write-mask", GPIOState, write_mask, 0),
    DEFINE_PROP_UINT32("default", GPIOState, default_value, 0),
};

static void gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    VMStateIfClass *vc = VMSTATE_IF_CLASS(klass);

    dc->realize = gpio_realize;
    dc->unrealize = gpio_unrealize;
    dc->vmsd = &vmstate_gpio;
    dc->desc = "Configurable MMIO GPIO bank";
    dc->hotpluggable = false;
    device_class_set_props(dc, gpio_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    rc->phases.hold = gpio_reset_hold;
    vc->get_id = gpio_vmstate_id;
}

static const TypeInfo gpio_info = {
    .name = TYPE_GPIO,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(GPIOState),
    .instance_init = gpio_init,
    .instance_finalize = gpio_finalize,
    .class_init = gpio_class_init,
};

static void gpio_register_types(void)
{
    type_register_static(&gpio_info);
}

type_init(gpio_register_types)
