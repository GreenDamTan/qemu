/*
 * 可由监控命令控制的 MMIO 测试寄存器
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

#define TYPE_TRIM_MMIO "trim-mmio"
OBJECT_DECLARE_SIMPLE_TYPE(TrimMMIOState, TRIM_MMIO)

struct TrimMMIOState {
    DeviceState parent_obj;
    MemoryRegion mmio;
    QEMUTimer timer;
    uint64_t addr;
    uint32_t size;
    uint32_t default_value;
    uint32_t value;
};

static bool trim_mmio_accepts(void *opaque, hwaddr addr, unsigned size,
                              bool is_write, MemTxAttrs attrs)
{
    TrimMMIOState *s = opaque;

    return addr < s->size && size <= s->size - addr;
}

static uint64_t trim_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    TrimMMIOState *s = opaque;
    uint64_t mask = (1ULL << (size * 8)) - 1;

    return (s->value >> (addr * 8)) & mask;
}

static void trim_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    TrimMMIOState *s = opaque;
    unsigned shift = addr * 8;
    uint64_t mask = ((1ULL << (size * 8)) - 1) << shift;

    s->value = (s->value & ~mask) | ((data << shift) & mask);
}

static const MemoryRegionOps trim_mmio_ops = {
    .read = trim_mmio_read,
    .write = trim_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
        .accepts = trim_mmio_accepts,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void trim_mmio_restore(void *opaque)
{
    TrimMMIOState *s = opaque;

    s->value = s->default_value;
}

void qmp_trim_mmio(const char *id, uint32_t value, bool has_duration,
                   uint32_t duration, Error **errp)
{
    Object *obj = object_resolve_path_component(
        machine_get_container("peripheral"), id);
    TrimMMIOState *s;
    int64_t deadline = 0;

    if (!obj || !object_dynamic_cast(obj, TYPE_TRIM_MMIO) ||
        !DEVICE(obj)->realized) {
        error_setg(errp, "Device '%s' is not a realized trim-mmio device", id);
        return;
    }
    s = TRIM_MMIO(obj);
    if (value >= (1ULL << (s->size * 8))) {
        error_setg(errp, "Value does not fit in the %u-byte register", s->size);
        return;
    }
    if (has_duration) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t delay = duration * NANOSECONDS_PER_SECOND;

        if (!duration || now > INT64_MAX - delay) {
            error_setg(errp,
                       "Duration must be positive and fit in virtual time");
            return;
        }
        deadline = now + delay;
    }

    timer_del(&s->timer);
    s->value = value;
    if (has_duration) {
        timer_mod(&s->timer, deadline);
    }
}

void hmp_trim_mmio(Monitor *mon, const QDict *qdict)
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
        error_setg(&err, "Duration must be between 1 and %" PRIu32 " seconds",
                   UINT32_MAX);
    } else {
        qmp_trim_mmio(id, value, duration_str != NULL, duration, &err);
    }
    hmp_handle_error(mon, err);
}

static void trim_mmio_reset_hold(Object *obj, ResetType type)
{
    TrimMMIOState *s = TRIM_MMIO(obj);

    timer_del(&s->timer);
    s->value = s->default_value;
}

static void trim_mmio_realize(DeviceState *dev, Error **errp)
{
    TrimMMIOState *s = TRIM_MMIO(dev);
    MemoryRegionSection section;

    if (!dev->id) {
        error_setg(errp, "trim-mmio requires an id");
        return;
    }
    if (s->addr == UINT64_MAX) {
        error_setg(errp, "trim-mmio requires an addr");
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
    if (s->default_value >= (1ULL << (s->size * 8))) {
        error_setg(errp, "default does not fit in the %u-byte register",
                   s->size);
        return;
    }

    section = memory_region_find(get_system_memory(), s->addr, s->size);
    if (section.mr) {
        error_setg(errp, "MMIO range at 0x%" PRIx64 " overlaps region '%s'",
                   s->addr, memory_region_name(section.mr));
        memory_region_unref(section.mr);
        return;
    }

    s->value = s->default_value;
    memory_region_init_io(&s->mmio, OBJECT(s), &trim_mmio_ops, s,
                          TYPE_TRIM_MMIO, s->size);
    memory_region_add_subregion(get_system_memory(), s->addr, &s->mmio);
    /* 无总线设备不在系统总线的复位树中。 */
    qemu_register_resettable(OBJECT(s));
}

static void trim_mmio_unrealize(DeviceState *dev)
{
    TrimMMIOState *s = TRIM_MMIO(dev);

    timer_del(&s->timer);
    qemu_unregister_resettable(OBJECT(s));
    memory_region_del_subregion(get_system_memory(), &s->mmio);
}

static void trim_mmio_init(Object *obj)
{
    TrimMMIOState *s = TRIM_MMIO(obj);

    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, trim_mmio_restore, s);
}

static void trim_mmio_finalize(Object *obj)
{
    TrimMMIOState *s = TRIM_MMIO(obj);

    timer_del(&s->timer);
    timer_deinit(&s->timer);
}

static char *trim_mmio_vmstate_id(VMStateIf *obj)
{
    return g_strdup(DEVICE(obj)->id);
}

static const VMStateDescription vmstate_trim_mmio = {
    .name = TYPE_TRIM_MMIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(addr, TrimMMIOState, "addr differs"),
        VMSTATE_UINT32_EQUAL(size, TrimMMIOState, "size differs"),
        VMSTATE_UINT32_EQUAL(default_value, TrimMMIOState, "default differs"),
        VMSTATE_UINT32(value, TrimMMIOState),
        VMSTATE_TIMER(timer, TrimMMIOState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property trim_mmio_properties[] = {
    DEFINE_PROP_UINT64("addr", TrimMMIOState, addr, UINT64_MAX),
    DEFINE_PROP_UINT32("size", TrimMMIOState, size, 4),
    DEFINE_PROP_UINT32("default", TrimMMIOState, default_value, 0),
};

static void trim_mmio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    VMStateIfClass *vc = VMSTATE_IF_CLASS(klass);

    dc->realize = trim_mmio_realize;
    dc->unrealize = trim_mmio_unrealize;
    dc->vmsd = &vmstate_trim_mmio;
    dc->desc = "Monitor-controlled MMIO test register";
    dc->hotpluggable = false;
    device_class_set_props(dc, trim_mmio_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    rc->phases.hold = trim_mmio_reset_hold;
    vc->get_id = trim_mmio_vmstate_id;
}

static const TypeInfo trim_mmio_info = {
    .name = TYPE_TRIM_MMIO,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TrimMMIOState),
    .instance_init = trim_mmio_init,
    .instance_finalize = trim_mmio_finalize,
    .class_init = trim_mmio_class_init,
};

static void trim_mmio_register_types(void)
{
    type_register_static(&trim_mmio_info);
}

type_init(trim_mmio_register_types)
