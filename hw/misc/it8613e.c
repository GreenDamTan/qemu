/*
 * ITE IT8613E Super I/O hardware monitor emulation
 *
 * This model implements the Super I/O configuration ports and enough of the
 * IT87 hardware monitor register file for the Linux it87 driver to bind to an
 * IT8613E and report controllable sensor values.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/qdev-properties.h"
#include "hw/isa/isa.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IT8613E_SUPERIO "it8613e-superio"
OBJECT_DECLARE_SIMPLE_TYPE(IT8613EState, IT8613E_SUPERIO)

#define IT8613E_DEVID              0x8613
#define IT8613E_DEVREV             0x01

#define IT8613E_DEFAULT_SIO_ADDR   0x2e
#define IT8613E_DEFAULT_HWM_ADDR   0x290
#define IT8613E_HWM_EXTENT         8
#define IT8613E_HWM_INDEX_OFFSET   5
#define IT8613E_HWM_DATA_OFFSET    6

#define IT8613E_SIO_DEV            0x07
#define IT8613E_SIO_DEVID          0x20
#define IT8613E_SIO_DEVREV         0x22
#define IT8613E_SIO_ACT            0x30
#define IT8613E_SIO_BASE           0x60
#define IT8613E_SIO_GPIO3          0x27
#define IT8613E_SIO_GPIO5          0x29
#define IT8613E_SIO_PINX1          0x2a
#define IT8613E_SIO_BEEP_PIN       0xf6

#define IT8613E_LDN_PME            0x04
#define IT8613E_LDN_GPIO           0x07

#define IT8613E_REG_BANK           0x06
#define IT8613E_REG_FAN_16BIT      0x0c
#define IT8613E_REG_FAN_MAIN_CTRL  0x13
#define IT8613E_REG_FAN_CTL        0x14
#define IT8613E_REG_VIN_ENABLE     0x50
#define IT8613E_REG_TEMP_ENABLE    0x51
#define IT8613E_REG_TEMP_EXTRA     0x55
#define IT8613E_REG_CHIPID         0x58

#define IT8613E_NUM_FANS           6
#define IT8613E_NUM_TEMPS          6
#define IT8613E_NUM_INS            13

typedef enum IT8613EFanMode {
    IT8613E_FAN_MODE_FIXED = 0,
    IT8613E_FAN_MODE_PWM,
} IT8613EFanMode;

typedef enum IT8613EPropKind {
    IT8613E_PROP_FAN_RPM,
    IT8613E_PROP_FAN_MAX_RPM,
    IT8613E_PROP_FAN_MODE,
    IT8613E_PROP_FAN_LOCK,
    IT8613E_PROP_TEMP,
    IT8613E_PROP_IN,
} IT8613EPropKind;

struct IT8613EState {
    ISADevice parent_obj;

    uint32_t sio_addr;
    uint32_t hwm_addr;

    MemoryRegion sio_io;
    MemoryRegion hwm_io;

    uint8_t sio_index;
    uint8_t sio_ldn;
    uint8_t sio_enter_step;
    bool sio_unlocked;

    uint8_t sio_global_regs[256];
    uint8_t sio_ldn_regs[16][256];

    uint8_t hwm_index;
    uint8_t hwm_regs[3][256];

    uint32_t fan_rpm[IT8613E_NUM_FANS];
    uint32_t fan_max_rpm[IT8613E_NUM_FANS];
    uint8_t fan_mode[IT8613E_NUM_FANS];
    bool fan_locked[IT8613E_NUM_FANS];
    int32_t temp_mc[IT8613E_NUM_TEMPS];
    uint32_t in_mv[IT8613E_NUM_INS];
};

#define DEFINE_IT8613E_PROP_UINT32(_name, _kind, _index, _defval) { \
        .name = (_name),                                            \
        .info = &it8613e_prop_uint32,                               \
        .set_default = true,                                        \
        .defval.u = (_defval),                                      \
        .arrayoffset = (_kind),                                     \
        .arrayfieldsize = (_index),                                 \
    }

#define DEFINE_IT8613E_PROP_INT32(_name, _kind, _index, _defval) {  \
        .name = (_name),                                            \
        .info = &it8613e_prop_int32,                                \
        .set_default = true,                                        \
        .defval.i = (_defval),                                      \
        .arrayoffset = (_kind),                                     \
        .arrayfieldsize = (_index),                                 \
    }

static const PropertyInfo it8613e_prop_uint32;
static const PropertyInfo it8613e_prop_int32;

static const uint8_t it8613e_fan_lsb_regs[IT8613E_NUM_FANS] = {
    0x0d, 0x0e, 0x0f, 0x80, 0x82, 0x4c,
};

static const uint8_t it8613e_fan_msb_regs[IT8613E_NUM_FANS] = {
    0x18, 0x19, 0x1a, 0x81, 0x83, 0x4d,
};

static const uint8_t it8613e_fan_min_lsb_regs[IT8613E_NUM_FANS] = {
    0x10, 0x11, 0x12, 0x84, 0x86, 0x4e,
};

static const uint8_t it8613e_fan_min_msb_regs[IT8613E_NUM_FANS] = {
    0x1b, 0x1c, 0x1d, 0x85, 0x87, 0x4f,
};

static const uint8_t it8613e_pwm_regs[IT8613E_NUM_FANS] = {
    0x15, 0x16, 0x17, 0x1e, 0x1f, 0x92,
};

static const uint8_t it8613e_vin_regs[IT8613E_NUM_INS] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
    0x27, 0x28, 0x2f, 0x2c, 0x2d, 0x2e,
};

static const uint8_t it8613e_temp_limit_high_regs[IT8613E_NUM_TEMPS] = {
    0x40, 0x42, 0x44, 0x46, 0xb4, 0xb6,
};

static const uint8_t it8613e_temp_limit_low_regs[IT8613E_NUM_TEMPS] = {
    0x41, 0x43, 0x45, 0x47, 0xb5, 0xb7,
};

static uint8_t it8613e_current_bank(IT8613EState *s)
{
    return (s->hwm_regs[0][IT8613E_REG_BANK] >> 5) & 0x7;
}

static uint8_t *it8613e_current_hwm_regs(IT8613EState *s)
{
    uint8_t bank = it8613e_current_bank(s);

    if (bank >= ARRAY_SIZE(s->hwm_regs)) {
        bank = 0;
    }
    return s->hwm_regs[bank];
}

static uint16_t it8613e_rpm_to_fan16(uint32_t rpm)
{
    uint64_t val;

    if (rpm == 0) {
        return 0xffff;
    }

    val = (1350000ULL + rpm) / (rpm * 2ULL);
    val = MAX(1, MIN(val, 0xfffe));
    return val;
}

static uint8_t it8613e_temp_to_reg(int32_t mc)
{
    int64_t val = mc;

    if (val < 0) {
        val = (val - 500) / 1000;
    } else {
        val = (val + 500) / 1000;
    }

    val = MAX(-128, MIN(val, 127));
    return (uint8_t)(int8_t)val;
}

static uint8_t it8613e_in_to_reg(uint32_t mv)
{
    return (MIN(mv, 2805) * 10 + 55) / 110;
}

static void it8613e_set_fan_registers(IT8613EState *s, unsigned fan,
                                      uint32_t rpm)
{
    uint16_t raw;

    if (fan >= IT8613E_NUM_FANS) {
        return;
    }

    raw = it8613e_rpm_to_fan16(rpm);
    s->hwm_regs[0][it8613e_fan_lsb_regs[fan]] = raw & 0xff;
    s->hwm_regs[0][it8613e_fan_msb_regs[fan]] = raw >> 8;
}

static void it8613e_set_fan_min_registers(IT8613EState *s, unsigned fan,
                                          uint32_t rpm)
{
    uint16_t raw;

    if (fan >= IT8613E_NUM_FANS) {
        return;
    }

    raw = it8613e_rpm_to_fan16(rpm);
    s->hwm_regs[0][it8613e_fan_min_lsb_regs[fan]] = raw & 0xff;
    s->hwm_regs[0][it8613e_fan_min_msb_regs[fan]] = raw >> 8;
}

static bool it8613e_is_fan_tach_reg(unsigned fan, uint8_t reg)
{
    return reg == it8613e_fan_lsb_regs[fan] ||
           reg == it8613e_fan_msb_regs[fan];
}

static void it8613e_sync_fan_from_pwm(IT8613EState *s, unsigned fan)
{
    uint8_t pwm;
    uint32_t rpm;

    if (fan >= IT8613E_NUM_FANS ||
        s->fan_locked[fan] ||
        s->fan_mode[fan] != IT8613E_FAN_MODE_PWM) {
        return;
    }

    pwm = s->hwm_regs[0][it8613e_pwm_regs[fan]];
    rpm = (s->fan_max_rpm[fan] * pwm + 127) / 255;
    s->fan_rpm[fan] = rpm;
    it8613e_set_fan_registers(s, fan, rpm);
}

static void it8613e_set_temperature_register(IT8613EState *s, unsigned temp)
{
    if (temp >= IT8613E_NUM_TEMPS) {
        return;
    }

    s->hwm_regs[0][0x29 + temp] = it8613e_temp_to_reg(s->temp_mc[temp]);
}

static void it8613e_set_voltage_register(IT8613EState *s, unsigned vin)
{
    if (vin >= IT8613E_NUM_INS) {
        return;
    }

    s->hwm_regs[0][it8613e_vin_regs[vin]] = it8613e_in_to_reg(s->in_mv[vin]);
}

static void it8613e_reset_registers(IT8613EState *s)
{
    unsigned i;
    uint16_t hwm_base = s->hwm_addr & ~(IT8613E_HWM_EXTENT - 1);

    memset(s->sio_global_regs, 0, sizeof(s->sio_global_regs));
    memset(s->sio_ldn_regs, 0, sizeof(s->sio_ldn_regs));
    memset(s->hwm_regs, 0, sizeof(s->hwm_regs));

    s->sio_index = 0;
    s->sio_ldn = 0;
    s->sio_enter_step = 0;
    s->sio_unlocked = false;

    s->sio_global_regs[IT8613E_SIO_DEVID] = IT8613E_DEVID >> 8;
    s->sio_global_regs[IT8613E_SIO_DEVID + 1] = IT8613E_DEVID & 0xff;
    s->sio_global_regs[IT8613E_SIO_DEVREV] = IT8613E_DEVREV;

    s->sio_ldn_regs[IT8613E_LDN_PME][IT8613E_SIO_ACT] = 0x01;
    s->sio_ldn_regs[IT8613E_LDN_PME][IT8613E_SIO_BASE] = hwm_base >> 8;
    s->sio_ldn_regs[IT8613E_LDN_PME][IT8613E_SIO_BASE + 1] = hwm_base & 0xff;

    /*
     * These GPIO defaults expose fan2-fan5 and hide fan1, matching the
     * IT8613E-specific selection logic in the Linux it87 driver.
     */
    s->sio_ldn_regs[IT8613E_LDN_GPIO][IT8613E_SIO_GPIO3] = BIT(1);
    s->sio_ldn_regs[IT8613E_LDN_GPIO][IT8613E_SIO_GPIO5] = 0x00;
    s->sio_ldn_regs[IT8613E_LDN_GPIO][IT8613E_SIO_PINX1] = BIT(0);
    s->sio_ldn_regs[IT8613E_LDN_GPIO][IT8613E_SIO_BEEP_PIN] = 0x00;

    s->hwm_regs[0][IT8613E_REG_CHIPID] = 0x90;
    s->hwm_regs[0][IT8613E_REG_FAN_MAIN_CTRL] = BIT(5) | BIT(6);
    s->hwm_regs[0][IT8613E_REG_FAN_16BIT] = BIT(4) | BIT(5);
    s->hwm_regs[0][IT8613E_REG_FAN_CTL] = BIT(7);
    s->hwm_regs[0][IT8613E_REG_VIN_ENABLE] = 0xff;
    s->hwm_regs[0][IT8613E_REG_TEMP_ENABLE] = 0x07;
    s->hwm_regs[0][IT8613E_REG_TEMP_EXTRA] = 0x00;

    for (i = 0; i < IT8613E_NUM_FANS; i++) {
        s->hwm_regs[0][it8613e_pwm_regs[i]] = 0x7f;
        it8613e_set_fan_registers(s, i, s->fan_rpm[i]);
        it8613e_set_fan_min_registers(s, i, 300);
        it8613e_sync_fan_from_pwm(s, i);
    }

    for (i = 0; i < IT8613E_NUM_TEMPS; i++) {
        it8613e_set_temperature_register(s, i);
        s->hwm_regs[0][it8613e_temp_limit_high_regs[i]] = 80;
        s->hwm_regs[0][it8613e_temp_limit_low_regs[i]] = 0;
    }

    for (i = 0; i < IT8613E_NUM_INS; i++) {
        it8613e_set_voltage_register(s, i);
        if (i < 8) {
            s->hwm_regs[0][0x30 + i * 2] = 0xff;
            s->hwm_regs[0][0x31 + i * 2] = 0x00;
        }
    }
}

static uint8_t *it8613e_sio_selected_regs(IT8613EState *s)
{
    switch (s->sio_index) {
    case IT8613E_SIO_DEVID:
    case IT8613E_SIO_DEVID + 1:
    case IT8613E_SIO_DEVREV:
    case IT8613E_SIO_DEV:
        return s->sio_global_regs;
    default:
        if (s->sio_ldn < ARRAY_SIZE(s->sio_ldn_regs)) {
            return s->sio_ldn_regs[s->sio_ldn];
        }
        return s->sio_global_regs;
    }
}

static uint64_t it8613e_sio_read(void *opaque, hwaddr addr, unsigned size)
{
    IT8613EState *s = opaque;
    uint8_t *regs;

    if (addr == 0) {
        return s->sio_index;
    }

    if (!s->sio_unlocked) {
        return 0xff;
    }

    if (s->sio_index == IT8613E_SIO_DEV) {
        return s->sio_ldn;
    }

    regs = it8613e_sio_selected_regs(s);
    return regs[s->sio_index];
}

static void it8613e_sio_enter_step(IT8613EState *s, uint8_t val)
{
    static const uint8_t seq_2e[] = { 0x87, 0x01, 0x55, 0x55 };
    static const uint8_t seq_4e[] = { 0x87, 0x01, 0x55, 0xaa };
    const uint8_t *seq = s->sio_addr == 0x4e ? seq_4e : seq_2e;

    if (val == seq[s->sio_enter_step]) {
        s->sio_enter_step++;
        if (s->sio_enter_step == 4) {
            s->sio_unlocked = true;
            s->sio_enter_step = 0;
        }
        return;
    }

    s->sio_enter_step = val == 0x87 ? 1 : 0;
}

static void it8613e_sio_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    IT8613EState *s = opaque;
    uint8_t *regs;
    uint8_t data = val;

    if (addr == 0) {
        if (!s->sio_unlocked) {
            it8613e_sio_enter_step(s, data);
        } else {
            s->sio_index = data;
        }
        return;
    }

    if (!s->sio_unlocked) {
        return;
    }

    if (s->sio_index == 0x02 && data == 0x02) {
        s->sio_unlocked = false;
        s->sio_enter_step = 0;
        return;
    }

    if (s->sio_index == IT8613E_SIO_DEV) {
        s->sio_ldn = data & 0x0f;
        return;
    }

    regs = it8613e_sio_selected_regs(s);
    if (s->sio_ldn == IT8613E_LDN_PME &&
        (s->sio_index == IT8613E_SIO_ACT ||
         s->sio_index == IT8613E_SIO_BASE ||
         s->sio_index == IT8613E_SIO_BASE + 1)) {
        /*
         * The emulated I/O regions are fixed by QEMU properties at realize
         * time. Keep the Super I/O config view in sync with those regions.
         */
        return;
    }
    regs[s->sio_index] = data;
}

static uint64_t it8613e_hwm_read(void *opaque, hwaddr addr, unsigned size)
{
    IT8613EState *s = opaque;
    uint8_t *regs;

    switch (addr) {
    case IT8613E_HWM_INDEX_OFFSET:
        return s->hwm_index;
    case IT8613E_HWM_DATA_OFFSET:
        if (s->hwm_index == IT8613E_REG_BANK) {
            return s->hwm_regs[0][IT8613E_REG_BANK];
        }
        regs = it8613e_current_hwm_regs(s);
        return regs[s->hwm_index];
    default:
        return 0xff;
    }
}

static void it8613e_hwm_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    IT8613EState *s = opaque;
    uint8_t *regs;
    uint8_t data = val;
    unsigned i;

    switch (addr) {
    case IT8613E_HWM_INDEX_OFFSET:
        s->hwm_index = data;
        break;
    case IT8613E_HWM_DATA_OFFSET:
        if (s->hwm_index == IT8613E_REG_BANK) {
            s->hwm_regs[0][IT8613E_REG_BANK] = data;
            break;
        }
        regs = it8613e_current_hwm_regs(s);
        regs[s->hwm_index] = data;
        for (i = 1; i <= 4; i++) {
            if (s->fan_locked[i] &&
                it8613e_is_fan_tach_reg(i, s->hwm_index)) {
                it8613e_set_fan_registers(s, i, s->fan_rpm[i]);
                break;
            }
            if (s->hwm_index == it8613e_pwm_regs[i]) {
                it8613e_sync_fan_from_pwm(s, i);
                break;
            }
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps it8613e_sio_ops = {
    .read = it8613e_sio_read,
    .write = it8613e_sio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps it8613e_hwm_ops = {
    .read = it8613e_hwm_read,
    .write = it8613e_hwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void it8613e_get_fan_rpm(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned fan = (uintptr_t)opaque;
    uint32_t value = s->fan_rpm[fan];

    visit_type_uint32(v, name, &value, errp);
}

static void it8613e_set_fan_rpm(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned fan = (uintptr_t)opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    if (value > 1000000) {
        error_setg(errp, "%s must be <= 1000000", name);
        return;
    }

    s->fan_rpm[fan] = value;
    it8613e_set_fan_registers(s, fan, value);
}

static void it8613e_get_fan_max_rpm(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned fan = (uintptr_t)opaque;
    uint32_t value = s->fan_max_rpm[fan];

    visit_type_uint32(v, name, &value, errp);
}

static void it8613e_set_fan_max_rpm(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned fan = (uintptr_t)opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    if (value > 1000000) {
        error_setg(errp, "%s must be <= 1000000", name);
        return;
    }

    s->fan_max_rpm[fan] = value;
    it8613e_sync_fan_from_pwm(s, fan);
}

static void it8613e_get_temp_mc(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned temp = (uintptr_t)opaque;
    int64_t value = s->temp_mc[temp];

    visit_type_int(v, name, &value, errp);
}

static void it8613e_set_temp_mc(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned temp = (uintptr_t)opaque;
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }

    if (value < -128000 || value > 127000) {
        error_setg(errp, "%s must be between -128000 and 127000", name);
        return;
    }

    s->temp_mc[temp] = value;
    it8613e_set_temperature_register(s, temp);
}

static void it8613e_get_in_mv(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned vin = (uintptr_t)opaque;
    uint32_t value = s->in_mv[vin];

    visit_type_uint32(v, name, &value, errp);
}

static void it8613e_set_in_mv(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned vin = (uintptr_t)opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    if (value > 2805) {
        error_setg(errp, "%s must be <= 2805", name);
        return;
    }

    s->in_mv[vin] = value;
    it8613e_set_voltage_register(s, vin);
}

static void it8613e_prop_set_default_uint(ObjectProperty *op,
                                          const Property *prop)
{
    object_property_set_default_uint(op, prop->defval.u);
}

static void it8613e_prop_set_default_int(ObjectProperty *op,
                                         const Property *prop)
{
    object_property_set_default_int(op, prop->defval.i);
}

static void it8613e_prop_get_uint32(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    const Property *prop = opaque;
    IT8613EPropKind kind = prop->arrayoffset;
    unsigned index = prop->arrayfieldsize;

    switch (kind) {
    case IT8613E_PROP_FAN_RPM:
        it8613e_get_fan_rpm(obj, v, name, (void *)(uintptr_t)index, errp);
        break;
    case IT8613E_PROP_FAN_MAX_RPM:
        it8613e_get_fan_max_rpm(obj, v, name, (void *)(uintptr_t)index,
                                errp);
        break;
    case IT8613E_PROP_FAN_MODE:
    {
        IT8613EState *s = IT8613E_SUPERIO(obj);
        uint32_t value = s->fan_mode[index];

        visit_type_uint32(v, name, &value, errp);
        break;
    }
    case IT8613E_PROP_FAN_LOCK:
    {
        IT8613EState *s = IT8613E_SUPERIO(obj);
        uint32_t value = s->fan_locked[index];

        visit_type_uint32(v, name, &value, errp);
        break;
    }
    case IT8613E_PROP_IN:
        it8613e_get_in_mv(obj, v, name, (void *)(uintptr_t)index, errp);
        break;
    default:
        error_setg(errp, "invalid IT8613E uint32 property %s", name);
        break;
    }
}

static void it8613e_prop_set_uint32(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    const Property *prop = opaque;
    IT8613EPropKind kind = prop->arrayoffset;
    unsigned index = prop->arrayfieldsize;

    switch (kind) {
    case IT8613E_PROP_FAN_RPM:
        it8613e_set_fan_rpm(obj, v, name, (void *)(uintptr_t)index, errp);
        break;
    case IT8613E_PROP_FAN_MAX_RPM:
        it8613e_set_fan_max_rpm(obj, v, name, (void *)(uintptr_t)index,
                                errp);
        break;
    case IT8613E_PROP_FAN_MODE:
    {
        IT8613EState *s = IT8613E_SUPERIO(obj);
        uint32_t value;

        if (!visit_type_uint32(v, name, &value, errp)) {
            return;
        }

        if (value > IT8613E_FAN_MODE_PWM) {
            error_setg(errp, "%s must be 0 (fixed) or 1 (pwm)", name);
            return;
        }

        s->fan_mode[index] = value;
        it8613e_sync_fan_from_pwm(s, index);
        break;
    }
    case IT8613E_PROP_FAN_LOCK:
    {
        IT8613EState *s = IT8613E_SUPERIO(obj);
        uint32_t value;

        if (!visit_type_uint32(v, name, &value, errp)) {
            return;
        }

        if (value > 1) {
            error_setg(errp, "%s must be 0 (unlocked) or 1 (locked)", name);
            return;
        }

        s->fan_locked[index] = value;
        it8613e_set_fan_registers(s, index, s->fan_rpm[index]);
        break;
    }
    case IT8613E_PROP_IN:
        it8613e_set_in_mv(obj, v, name, (void *)(uintptr_t)index, errp);
        break;
    default:
        error_setg(errp, "invalid IT8613E uint32 property %s", name);
        break;
    }
}

static void it8613e_prop_get_int32(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    const Property *prop = opaque;
    unsigned index = prop->arrayfieldsize;

    it8613e_get_temp_mc(obj, v, name, (void *)(uintptr_t)index, errp);
}

static void it8613e_prop_set_int32(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    const Property *prop = opaque;
    unsigned index = prop->arrayfieldsize;

    it8613e_set_temp_mc(obj, v, name, (void *)(uintptr_t)index, errp);
}

static const PropertyInfo it8613e_prop_uint32 = {
    .type = "uint32",
    .get = it8613e_prop_get_uint32,
    .set = it8613e_prop_set_uint32,
    .set_default_value = it8613e_prop_set_default_uint,
    .realized_set_allowed = true,
};

static const PropertyInfo it8613e_prop_int32 = {
    .type = "int32",
    .get = it8613e_prop_get_int32,
    .set = it8613e_prop_set_int32,
    .set_default_value = it8613e_prop_set_default_int,
    .realized_set_allowed = true,
};

static void it8613e_init(Object *obj)
{
    IT8613EState *s = IT8613E_SUPERIO(obj);
    unsigned i;

    s->fan_rpm[1] = 1000;
    s->fan_rpm[2] = 1200;
    s->fan_rpm[3] = 900;
    s->fan_rpm[4] = 800;

    for (i = 0; i < IT8613E_NUM_FANS; i++) {
        s->fan_max_rpm[i] = 2000;
        s->fan_mode[i] = IT8613E_FAN_MODE_FIXED;
    }

    s->temp_mc[0] = 40000;
    s->temp_mc[1] = 42000;
    s->temp_mc[2] = 38000;
    s->temp_mc[3] = 0;
    s->temp_mc[4] = 0;
    s->temp_mc[5] = 0;

    s->in_mv[0] = 900;
    s->in_mv[1] = 1000;
    s->in_mv[2] = 1100;
    s->in_mv[3] = 0;
    s->in_mv[4] = 1200;
    s->in_mv[5] = 1500;
    s->in_mv[6] = 0;
    s->in_mv[7] = 1800;
    s->in_mv[8] = 2805;
    s->in_mv[9] = 2805;
    s->in_mv[10] = 1000;
    s->in_mv[11] = 1200;
    s->in_mv[12] = 1500;
}

static void it8613e_reset(DeviceState *dev)
{
    IT8613EState *s = IT8613E_SUPERIO(dev);

    it8613e_reset_registers(s);
}

static void it8613e_realize(DeviceState *dev, Error **errp)
{
    IT8613EState *s = IT8613E_SUPERIO(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    if (s->sio_addr > 0xfffe) {
        error_setg(errp, "sio-addr must be <= 0xfffe");
        return;
    }

    if (s->hwm_addr > 0xfff8 || (s->hwm_addr & (IT8613E_HWM_EXTENT - 1))) {
        error_setg(errp, "hwm-addr must be 8-byte aligned and <= 0xfff8");
        return;
    }

    it8613e_reset_registers(s);

    memory_region_init_io(&s->sio_io, OBJECT(dev), &it8613e_sio_ops, s,
                          "it8613e-sio", 2);
    isa_register_ioport(isa, &s->sio_io, s->sio_addr);

    memory_region_init_io(&s->hwm_io, OBJECT(dev), &it8613e_hwm_ops, s,
                          "it8613e-hwm", IT8613E_HWM_EXTENT);
    isa_register_ioport(isa, &s->hwm_io, s->hwm_addr);
}

static const Property it8613e_properties[] = {
    DEFINE_PROP_UINT32("sio-addr", IT8613EState, sio_addr,
                       IT8613E_DEFAULT_SIO_ADDR),
    DEFINE_PROP_UINT32("hwm-addr", IT8613EState, hwm_addr,
                       IT8613E_DEFAULT_HWM_ADDR),
    DEFINE_IT8613E_PROP_UINT32("fan2", IT8613E_PROP_FAN_RPM, 1, 1000),
    DEFINE_IT8613E_PROP_UINT32("fan3", IT8613E_PROP_FAN_RPM, 2, 1200),
    DEFINE_IT8613E_PROP_UINT32("fan4", IT8613E_PROP_FAN_RPM, 3, 900),
    DEFINE_IT8613E_PROP_UINT32("fan5", IT8613E_PROP_FAN_RPM, 4, 800),
    DEFINE_IT8613E_PROP_UINT32("fan2-rpm", IT8613E_PROP_FAN_RPM, 1, 1000),
    DEFINE_IT8613E_PROP_UINT32("fan3-rpm", IT8613E_PROP_FAN_RPM, 2, 1200),
    DEFINE_IT8613E_PROP_UINT32("fan4-rpm", IT8613E_PROP_FAN_RPM, 3, 900),
    DEFINE_IT8613E_PROP_UINT32("fan5-rpm", IT8613E_PROP_FAN_RPM, 4, 800),
    DEFINE_IT8613E_PROP_UINT32("fan2-max-rpm", IT8613E_PROP_FAN_MAX_RPM, 1,
                               2000),
    DEFINE_IT8613E_PROP_UINT32("fan3-max-rpm", IT8613E_PROP_FAN_MAX_RPM, 2,
                               2000),
    DEFINE_IT8613E_PROP_UINT32("fan4-max-rpm", IT8613E_PROP_FAN_MAX_RPM, 3,
                               2000),
    DEFINE_IT8613E_PROP_UINT32("fan5-max-rpm", IT8613E_PROP_FAN_MAX_RPM, 4,
                               2000),
    DEFINE_IT8613E_PROP_UINT32("fan2-mode", IT8613E_PROP_FAN_MODE, 1,
                               IT8613E_FAN_MODE_FIXED),
    DEFINE_IT8613E_PROP_UINT32("fan3-mode", IT8613E_PROP_FAN_MODE, 2,
                               IT8613E_FAN_MODE_FIXED),
    DEFINE_IT8613E_PROP_UINT32("fan4-mode", IT8613E_PROP_FAN_MODE, 3,
                               IT8613E_FAN_MODE_FIXED),
    DEFINE_IT8613E_PROP_UINT32("fan5-mode", IT8613E_PROP_FAN_MODE, 4,
                               IT8613E_FAN_MODE_FIXED),
    DEFINE_IT8613E_PROP_UINT32("fan2-lock", IT8613E_PROP_FAN_LOCK, 1, 0),
    DEFINE_IT8613E_PROP_UINT32("fan3-lock", IT8613E_PROP_FAN_LOCK, 2, 0),
    DEFINE_IT8613E_PROP_UINT32("fan4-lock", IT8613E_PROP_FAN_LOCK, 3, 0),
    DEFINE_IT8613E_PROP_UINT32("fan5-lock", IT8613E_PROP_FAN_LOCK, 4, 0),
    DEFINE_IT8613E_PROP_INT32("temp1-mc", IT8613E_PROP_TEMP, 0, 40000),
    DEFINE_IT8613E_PROP_INT32("temp2-mc", IT8613E_PROP_TEMP, 1, 42000),
    DEFINE_IT8613E_PROP_INT32("temp3-mc", IT8613E_PROP_TEMP, 2, 38000),
    DEFINE_IT8613E_PROP_UINT32("in0-mv", IT8613E_PROP_IN, 0, 900),
    DEFINE_IT8613E_PROP_UINT32("in1-mv", IT8613E_PROP_IN, 1, 1000),
    DEFINE_IT8613E_PROP_UINT32("in2-mv", IT8613E_PROP_IN, 2, 1100),
    DEFINE_IT8613E_PROP_UINT32("in3-mv", IT8613E_PROP_IN, 3, 0),
    DEFINE_IT8613E_PROP_UINT32("in4-mv", IT8613E_PROP_IN, 4, 1200),
    DEFINE_IT8613E_PROP_UINT32("in5-mv", IT8613E_PROP_IN, 5, 1500),
    DEFINE_IT8613E_PROP_UINT32("in6-mv", IT8613E_PROP_IN, 6, 0),
    DEFINE_IT8613E_PROP_UINT32("in7-mv", IT8613E_PROP_IN, 7, 1800),
    DEFINE_IT8613E_PROP_UINT32("in8-mv", IT8613E_PROP_IN, 8, 2805),
    DEFINE_IT8613E_PROP_UINT32("in9-mv", IT8613E_PROP_IN, 9, 2805),
    DEFINE_IT8613E_PROP_UINT32("in10-mv", IT8613E_PROP_IN, 10, 1000),
    DEFINE_IT8613E_PROP_UINT32("in11-mv", IT8613E_PROP_IN, 11, 1200),
    DEFINE_IT8613E_PROP_UINT32("in12-mv", IT8613E_PROP_IN, 12, 1500),
};

static const VMStateDescription vmstate_it8613e = {
    .name = "it8613e-superio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(sio_index, IT8613EState),
        VMSTATE_UINT8(sio_ldn, IT8613EState),
        VMSTATE_UINT8(sio_enter_step, IT8613EState),
        VMSTATE_BOOL(sio_unlocked, IT8613EState),
        VMSTATE_UINT8_ARRAY(sio_global_regs, IT8613EState, 256),
        VMSTATE_UINT8_2DARRAY(sio_ldn_regs, IT8613EState, 16, 256),
        VMSTATE_UINT8(hwm_index, IT8613EState),
        VMSTATE_UINT8_2DARRAY(hwm_regs, IT8613EState, 3, 256),
        VMSTATE_UINT32_ARRAY(fan_rpm, IT8613EState, IT8613E_NUM_FANS),
        VMSTATE_UINT32_ARRAY(fan_max_rpm, IT8613EState, IT8613E_NUM_FANS),
        VMSTATE_UINT8_ARRAY(fan_mode, IT8613EState, IT8613E_NUM_FANS),
        VMSTATE_BOOL_ARRAY(fan_locked, IT8613EState, IT8613E_NUM_FANS),
        VMSTATE_INT32_ARRAY(temp_mc, IT8613EState, IT8613E_NUM_TEMPS),
        VMSTATE_UINT32_ARRAY(in_mv, IT8613EState, IT8613E_NUM_INS),
        VMSTATE_END_OF_LIST()
    }
};

static void it8613e_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = it8613e_realize;
    dc->vmsd = &vmstate_it8613e;
    device_class_set_legacy_reset(dc, it8613e_reset);
    device_class_set_props(dc, it8613e_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo it8613e_info = {
    .name = TYPE_IT8613E_SUPERIO,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(IT8613EState),
    .instance_init = it8613e_init,
    .class_init = it8613e_class_init,
};

static void it8613e_register_types(void)
{
    type_register_static(&it8613e_info);
}

type_init(it8613e_register_types)
