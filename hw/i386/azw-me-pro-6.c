/*
 * AZW ME Pro-6 平台。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/units.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/firmware/smbios.h"
#include "hw/i386/pc.h"
#include "hw/ide/ide-bus.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-bridge/adl_n_root_port.h"
#include "hw/rtc/mc146818rtc_regs.h"
#include "hw/southbridge/ich9.h"
#include "monitor/qdev.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/system.h"
#include "e820_memory_layout.h"

#define TYPE_AZW_ME_PRO_6 MACHINE_TYPE_NAME("azw-me-pro-6")
OBJECT_DECLARE_SIMPLE_TYPE(AZWMEPro6State, AZW_ME_PRO_6)

#define AZW_NUM_ROOT_PORTS 8
#define AZW_NUM_SATA_PORTS 6
#define AZW_GPIO_PAGE 0xfd6d0000ULL
#define AZW_GPIO_ADDR (AZW_GPIO_PAGE + 0x960)

struct AZWMEPro6State {
    PCMachineState parent_obj;
    PCIBus *pcie[AZW_NUM_ROOT_PORTS];
    IDEBus *sata[AZW_NUM_SATA_PORTS];
    ICH9LPCState *lpc;
    MemoryRegion pm1_alias;
    DeviceState *fixed_devices[AZW_NUM_ROOT_PORTS + 3];
    Error *unplug_blocker;
};

typedef struct AZWRootPort {
    const char *type;
    const char *id;
    uint8_t devfn;
    uint8_t port;
} AZWRootPort;

static const AZWRootPort azw_root_ports[] = {
    { TYPE_ADL_N_PCIE_ROOT_PORT_1C0, "pcie.00.1c.0", PCI_DEVFN(0x1c, 0), 0 },
    { TYPE_ADL_N_PCIE_ROOT_PORT_1C2, "pcie.00.1c.2", PCI_DEVFN(0x1c, 2), 2 },
    { TYPE_ADL_N_PCIE_ROOT_PORT_1C3, "pcie.00.1c.3", PCI_DEVFN(0x1c, 3), 3 },
    { TYPE_ADL_N_PCIE_ROOT_PORT_1C6, "pcie.00.1c.6", PCI_DEVFN(0x1c, 6), 6 },
    { TYPE_ADL_N_PCIE_ROOT_PORT_1D0, "pcie.00.1d.0", PCI_DEVFN(0x1d, 0), 8 },
    { TYPE_ADL_N_PCIE_ROOT_PORT_1D1, "pcie.00.1d.1", PCI_DEVFN(0x1d, 1), 9 },
    { TYPE_ADL_N_PCIE_ROOT_PORT_1D2, "pcie.00.1d.2", PCI_DEVFN(0x1d, 2), 10 },
    { TYPE_ADL_N_PCIE_ROOT_PORT_1D3, "pcie.00.1d.3", PCI_DEVFN(0x1d, 3), 11 },
};

static const PCMachineIOReservation azw_io_reservations[] = {
    { 0xa30, 8 },
    { 0x1800, 4 },
};

static void (*azw_parent_init)(MachineState *machine);

static char *azw_disk_serial(unsigned bridge, int devfn, int sata_port)
{
    const AZWRootPort *rp = &azw_root_ports[bridge];
    g_autofree char *bdf = g_strdup_printf("00:%02x.%x-%02x:%02x.%x",
                                          PCI_SLOT(rp->devfn),
                                          PCI_FUNC(rp->devfn), bridge + 1,
                                          PCI_SLOT(devfn), PCI_FUNC(devfn));

    return sata_port < 0 ? g_steal_pointer(&bdf) :
           g_strdup_printf("%s-P%d", bdf, sata_port);
}

static bool azw_nvme_devfn(QDict *opts, int *devfn, Error **errp)
{
    QObject *addr = qdict_get(opts, "addr");
    uint64_t slot, function = 0;
    int64_t value;
    const char *str, *end;

    *devfn = 0;
    if (!addr) {
        qdict_put_str(opts, "addr", "0.0");
        return true;
    }
    if (qobject_type(addr) == QTYPE_QNUM) {
        if (qnum_get_try_int(qobject_to(QNum, addr), &value) &&
            value >= -1 && value <= 255) {
            *devfn = value == -1 ? 0 : value;
            return true;
        }
    } else if (qobject_type(addr) == QTYPE_QSTRING) {
        str = qstring_get_str(qobject_to(QString, addr));
        if (parse_uint(str, &end, 16, &slot) || slot > 31) {
            goto invalid;
        }
        if (*end == '.') {
            if (parse_uint_full(end + 1, 16, &function) || function > 7) {
                goto invalid;
            }
        } else if (*end) {
            goto invalid;
        }
        *devfn = PCI_DEVFN(slot, function);
        return true;
    }
invalid:
    error_setg(errp, "Invalid NVMe PCI address; use slot[.function]");
    return false;
}

static bool azw_complete_device_add(MachineState *machine, DeviceClass *dc,
                                    BusState **bus, QDict *opts, bool from_json,
                                    Error **errp)
{
    AZWMEPro6State *s = AZW_ME_PRO_6(machine);
    const char *type = object_class_get_name(OBJECT_CLASS(dc));
    static const unsigned nvme_bridges[] = { 0, 3, 4 };
    g_autofree char *serial = NULL;
    int i, devfn;
    bool nvme = !strcmp(type, "nvme");
    bool sata = !strcmp(type, "ide-hd") || !strcmp(type, "ide-cd");

    if (!nvme && !sata) {
        return true;
    }
    if (qdict_haskey(opts, "bus") && !qdict_get_try_str(opts, "bus")) {
        error_setg(errp, "Parameter 'bus' must be a string");
        return false;
    }

    if (!*bus) {
        if (nvme) {
            for (i = 0; i < ARRAY_SIZE(nvme_bridges); i++) {
                BusState *candidate = BUS(s->pcie[nvme_bridges[i]]);

                if (QTAILQ_EMPTY(&candidate->children)) {
                    *bus = candidate;
                    break;
                }
            }
        } else {
            for (i = 0; i < AZW_NUM_SATA_PORTS; i++) {
                BusState *candidate = BUS(s->sata[i]);

                if (!s->sata[i]->master && QTAILQ_EMPTY(&candidate->children)) {
                    *bus = candidate;
                    break;
                }
            }
        }
        if (!*bus) {
            error_setg(errp, "azw-me-pro-6 has no free %s slot",
                       nvme ? "NVMe" : "SATA");
            return false;
        }
    }

    if (nvme) {
        for (i = 0; i < AZW_NUM_ROOT_PORTS; i++) {
            if (*bus == BUS(s->pcie[i])) {
                if (!azw_nvme_devfn(opts, &devfn, errp)) {
                    return false;
                }
                if (PCI_SLOT(devfn)) {
                    error_setg(errp, "Root port '%s' only provides PCI slot 0",
                               azw_root_ports[i].id);
                    return false;
                }
                if (pci_find_device(s->pcie[i], pci_bus_num(s->pcie[i]),
                                    devfn)) {
                    error_setg(errp, "PCI address 00.%x on '%s' is occupied",
                               PCI_FUNC(devfn), azw_root_ports[i].id);
                    return false;
                }
                serial = azw_disk_serial(i, devfn, -1);
                break;
            }
        }
    } else {
        for (i = 0; i < AZW_NUM_SATA_PORTS; i++) {
            if (*bus == BUS(s->sata[i])) {
                serial = azw_disk_serial(5 + i / 2, 0, i % 2);
                break;
            }
        }
    }

    if (!qdict_haskey(opts, "serial")) {
        if (!serial) {
            error_setg(errp, "Specify serial when using a disk bus outside "
                       "the fixed azw-me-pro-6 topology");
            return false;
        }
        qdict_put_str(opts, "serial", serial);
    }
    return true;
}

static void azw_create_storage(AZWMEPro6State *s)
{
    PCMachineState *pcms = PC_MACHINE(s);
    unsigned i;

    error_setg(&s->unplug_blocker,
               "The fixed azw-me-pro-6 bridges and SATA controllers "
               "cannot be unplugged");
    for (i = 0; i < ARRAY_SIZE(azw_root_ports); i++) {
        const AZWRootPort *def = &azw_root_ports[i];
        PCIDevice *rp = PCI_FUNC(def->devfn) ? pci_new(def->devfn, def->type) :
                       pci_new_multifunction(def->devfn, def->type);
        DeviceState *dev = DEVICE(rp);
        PCIResReserve reserve = {
            .bus = -1,
            .io = 0,
            .mem_non_pref = 2 * MiB,
            .mem_pref_32 = -1,
            .mem_pref_64 = -1,
        };

        qdev_set_id(dev, g_strdup(def->id), &error_fatal);
        qdev_prop_set_uint8(dev, "port", def->port);
        qdev_prop_set_uint8(dev, "chassis", i + 1);
        qdev_prop_set_uint16(dev, "slot", i + 1);
        pci_realize_and_unref(rp, pcms->pcibus, &error_fatal);
        pci_bridge_qemu_reserve_cap_init(rp, 0, reserve, &error_fatal);
        s->pcie[i] = pci_bridge_get_sec_bus(PCI_BRIDGE(rp));
        s->fixed_devices[i] = dev;
        object_ref(OBJECT(dev));
        qdev_add_unplug_blocker(dev, s->unplug_blocker);
    }

    for (i = 0; i < 3; i++) {
        PCIDevice *controller = pci_new(0, "asmedia1062");
        DeviceState *dev = DEVICE(controller);
        g_autofree char *id = g_strdup_printf("asm%u", i);
        unsigned port;

        qdev_set_id(dev, g_steal_pointer(&id), &error_fatal);
        pci_realize_and_unref(controller, s->pcie[5 + i], &error_fatal);
        s->fixed_devices[AZW_NUM_ROOT_PORTS + i] = dev;
        object_ref(OBJECT(dev));
        qdev_add_unplug_blocker(dev, s->unplug_blocker);
        for (port = 0; port < 2; port++) {
            g_autofree char *name = g_strdup_printf("asm%u.%u", i, port);

            s->sata[i * 2 + port] = IDE_BUS(qdev_get_child_bus(dev, name));
        }
    }

    if (drive_get_max_bus(IF_IDE) >= AZW_NUM_SATA_PORTS) {
        error_report("azw-me-pro-6 has only six SATA ports (index 0..5)");
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < AZW_NUM_SATA_PORTS; i++) {
        DriveInfo *drive = drive_get(IF_IDE, i, 0);
        DeviceState *dev;
        g_autofree char *serial = NULL;
        g_autofree char *id = NULL;

        if (!drive) {
            continue;
        }
        dev = qdev_new(drive->media_cd ? "ide-cd" : "ide-hd");
        id = g_strdup_printf("sata%u", i);
        qdev_set_id(dev, g_steal_pointer(&id), &error_fatal);
        serial = azw_disk_serial(5 + i / 2, 0, i % 2);
        if (!IDE_DEVICE(dev)->serial) {
            qdev_prop_set_string(dev, "serial", serial);
        }
        qdev_prop_set_uint32(dev, "unit", 0);
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(drive),
                                &error_fatal);
        qdev_realize_and_unref(dev, BUS(s->sata[i]), &error_fatal);
    }
}

static QDict *azw_alarm_component(uint8_t raw, bool binary, bool hour_12,
                                 bool day)
{
    QDict *result = qdict_new();
    bool wildcard = day ? !(raw & 0x3f) :
                    (raw & RTC_ALARM_DONT_CARE) == RTC_ALARM_DONT_CARE;
    unsigned encoded = day ? raw & 0x3f : hour_12 ? raw & 0x7f : raw;
    unsigned value = binary ? encoded : (encoded >> 4) * 10 + (encoded & 15);
    unsigned min = day || hour_12 ? 1 : 0;
    unsigned max = day ? 31 : hour_12 ? 12 : 59;
    bool valid = wildcard || ((binary || ((encoded & 15) <= 9 &&
                                          (encoded >> 4) <= 9)) &&
                              value >= min && value <= max);

    if (valid && !wildcard && hour_12) {
        value = value % 12 + ((raw & 0x80) ? 12 : 0);
    }
    qdict_put_int(result, "raw", raw);
    qdict_put_bool(result, "wildcard", wildcard);
    qdict_put_bool(result, "valid", valid);
    if (valid && !wildcard) {
        qdict_put_int(result, "value", value);
    }
    return result;
}

static void azw_get_rtc_alarm(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    AZWMEPro6State *s = AZW_ME_PRO_6(obj);
    MC146818RtcState *rtc;
    uint8_t reg_b, hour;
    bool binary, hour_24;
    QDict *alarm, *hour_value;
    QObject *result;

    if (!s->lpc) {
        error_setg(errp, "RTC is not initialized");
        return;
    }
    rtc = &s->lpc->rtc;
    reg_b = mc146818rtc_get_cmos_data(rtc, RTC_REG_B);
    binary = reg_b & REG_B_DM;
    hour_24 = reg_b & REG_B_24H;
    hour = mc146818rtc_get_cmos_data(rtc, RTC_HOURS_ALARM);
    alarm = qdict_new();
    qdict_put_bool(alarm, "enabled", reg_b & REG_B_AIE);
    qdict_put_bool(alarm, "pm1-enabled", s->lpc->pm.acpi_regs.pm1.evt.en &
                   ACPI_BITMASK_RT_CLOCK_ENABLE);
    qdict_put_bool(alarm, "binary", binary);
    qdict_put_bool(alarm, "hour-24", hour_24);
    qdict_put(alarm, "day", azw_alarm_component(
                  mc146818rtc_get_cmos_data(rtc, RTC_REG_D), binary, false,
                  true));
    qdict_put(alarm, "second", azw_alarm_component(
                  mc146818rtc_get_cmos_data(rtc, RTC_SECONDS_ALARM), binary,
                  false, false));
    qdict_put(alarm, "minute", azw_alarm_component(
                  mc146818rtc_get_cmos_data(rtc, RTC_MINUTES_ALARM), binary,
                  false, false));
    hour_value = azw_alarm_component(hour, binary, !hour_24, false);
    if (hour_24 && qdict_haskey(hour_value, "value") &&
        qdict_get_int(hour_value, "value") > 23) {
        qdict_put_bool(hour_value, "valid", false);
        qdict_del(hour_value, "value");
    }
    qdict_put(alarm, "hour", hour_value);
    result = QOBJECT(alarm);
    visit_type_any(v, name, &result, errp);
    qobject_unref(result);
}

static void azw_smbios_defaults(void)
{
    if (!smbios_type1.manufacturer) {
        smbios_type1.manufacturer = g_strdup("AZW");
    }
    if (!smbios_type1.product) {
        smbios_type1.product = g_strdup("ME Pro-6");
    }
    if (!smbios_type1.version) {
        smbios_type1.version = g_strdup("V1.0");
    }
    if (!smbios_type1.serial) {
        smbios_type1.serial = g_strdup("TESTcf30dd1180414a188f6d3dd4d73a7540");
    }
    if (!smbios_type1.sku) {
        smbios_type1.sku = g_strdup("G01");
    }
    if (!smbios_type1.family) {
        smbios_type1.family = g_strdup("Default string");
    }
    if (!qemu_uuid_set) {
        qemu_uuid_parse("cf30dd11-8041-4a18-8f6d-3dd4d73a7540", &qemu_uuid);
        qemu_uuid_set = true;
    }
}

static void azw_init(MachineState *machine)
{
    AZWMEPro6State *s = AZW_ME_PRO_6(machine);
    PCMachineState *pcms = PC_MACHINE(machine);
    DeviceState *dev;
    PCIDevice *xhci;
    BusState *isa;
    unsigned fan;

    if (pcms->sata_enabled || machine_usb(machine)) {
        error_report("azw-me-pro-6 requires sata=off,usb=off; "
                     "ASM1062 and xHCI are built in");
        exit(EXIT_FAILURE);
    }
    azw_smbios_defaults();
    object_register_sugar_prop(TYPE_MC146818_RTC, "alarm-day-at-reg-d",
                               "true", false);
    object_register_sugar_prop(TYPE_ICH9_LPC_DEVICE,
                               ACPI_PM_PROP_ACPI_PCIHP_BRIDGE, "false", false);
    azw_parent_init(machine);

    s->lpc = ICH9_LPC_DEVICE(
        DEVICE(X86_MACHINE(machine)->rtc)->parent_bus->parent);
    isa = qdev_get_child_bus(DEVICE(s->lpc), "isa.0");
    azw_create_storage(s);

    e820_add_entry(AZW_GPIO_PAGE, 4096, E820_RESERVED);
    dev = qdev_new("gpio");
    qdev_set_id(dev, g_strdup("PORT_RESET_PRESS_SECONDS"), &error_fatal);
    qdev_prop_set_uint64(dev, "addr", AZW_GPIO_ADDR);
    qdev_prop_set_uint32(dev, "size", 2);
    qdev_prop_set_uint32(dev, "gpio-mask", 0x0002);
    qdev_prop_set_uint32(dev, "write-mask", 0x0700);
    qdev_prop_set_uint32(dev, "default", 0x0102);
    qdev_realize_and_unref(dev, NULL, &error_fatal);

    dev = qdev_new("it8613e-superio");
    qdev_set_id(dev, g_strdup("it8613e"), &error_fatal);
    qdev_prop_set_uint32(dev, "sio-addr", 0x2e);
    qdev_prop_set_uint32(dev, "hwm-addr", 0xa30);
    qdev_prop_set_uint8(dev, "pme-f4-default", 0x80);
    for (fan = 2; fan <= 5; fan++) {
        g_autofree char *property = g_strdup_printf("fan%u-rpm", fan);

        object_property_set_uint(OBJECT(dev), property, fan * 111,
                                 &error_fatal);
    }
    qdev_realize_and_unref(dev, isa, &error_fatal);

    xhci = pci_new(PCI_DEVFN(0x14, 0), "qemu-xhci");
    qdev_set_id(DEVICE(xhci), g_strdup("xhci"), &error_fatal);
    pci_realize_and_unref(xhci, pcms->pcibus, &error_fatal);
    dev = qdev_new("fnoscheck-disk");
    qdev_set_id(dev, g_strdup("fnoscheck"), &error_fatal);
    qdev_realize_and_unref(dev, qdev_get_child_bus(DEVICE(xhci), "xhci.0"),
                           &error_fatal);

    memory_region_init_alias(&s->pm1_alias, OBJECT(s), "azw-pm1",
                              &s->lpc->pm.acpi_regs.pm1.evt.io, 0, 4);
    memory_region_add_subregion(get_system_io(), 0x1800, &s->pm1_alias);
}

static void azw_instance_init(Object *obj)
{
    PC_MACHINE(obj)->sata_enabled = false;
    MACHINE(obj)->usb = false;
    object_property_add(obj, "rtc-alarm", "any", azw_get_rtc_alarm,
                         NULL, NULL, NULL);
}

static void azw_finalize(Object *obj)
{
    AZWMEPro6State *s = AZW_ME_PRO_6(obj);
    unsigned i;

    if (s->unplug_blocker) {
        for (i = 0; i < ARRAY_SIZE(s->fixed_devices); i++) {
            if (s->fixed_devices[i]) {
                qdev_del_unplug_blocker(s->fixed_devices[i], s->unplug_blocker);
                object_unref(OBJECT(s->fixed_devices[i]));
            }
        }
        error_free(s->unplug_blocker);
    }
}

static void azw_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MachineClass *parent = MACHINE_CLASS(object_class_get_parent(oc));
    PCMachineClass *pcmc = PC_MACHINE_CLASS(oc);

    g_ptr_array_extend(mc->compat_props, parent->compat_props, NULL, NULL);
    azw_parent_init = parent->init;
    mc->init = azw_init;
    mc->alias = NULL;
    mc->is_default = false;
    mc->family = "azw-me-pro";
    mc->desc = "AZW ME Pro-6 (Q35 with fixed Alder Lake-N topology)";
    mc->no_cdrom = true;
    mc->no_nic = true;
    mc->complete_device_add = azw_complete_device_add;
    pcmc->adl_n_root_ports = false;
    pcmc->fixed_mmio_base = AZW_GPIO_PAGE;
    pcmc->fixed_mmio_size = 4096;
    pcmc->fixed_io_ranges = azw_io_reservations;
    pcmc->num_fixed_io_ranges = ARRAY_SIZE(azw_io_reservations);
}

static const TypeInfo azw_info = {
    .name = TYPE_AZW_ME_PRO_6,
    .parent = MACHINE_TYPE_NAME("pc-q35-11.0"),
    .instance_size = sizeof(AZWMEPro6State),
    .instance_init = azw_instance_init,
    .instance_finalize = azw_finalize,
    .class_init = azw_class_init,
};

static void azw_register_types(void)
{
    type_register_static(&azw_info);
}

type_init(azw_register_types)
