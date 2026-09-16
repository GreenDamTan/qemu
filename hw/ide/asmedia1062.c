/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/hotplug.h"
#include "hw/core/irq.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "migration/vmstate.h"
#include "ahci-internal.h"

#define TYPE_ASMEDIA1062 "asmedia1062"
OBJECT_DECLARE_SIMPLE_TYPE(ASMedia1062State, ASMEDIA1062)

#define ASMEDIA1062_PORTS                 2
#define ASMEDIA1062_BAR_SIZE              (8 * KiB)
#define ASMEDIA1062_PM_OFFSET             0x40
#define ASMEDIA1062_MSI_OFFSET            0x50
#define ASMEDIA1062_PCIE_OFFSET           0x80
#define ASMEDIA1062_AER_OFFSET            0x100
#define ASMEDIA1062_AER_SIZE              0x30
#define ASMEDIA1062_SEC_OFFSET            0x130
#define ASMEDIA1062_SEC_SIZE              0x10

#define ASMEDIA1062_LNKCTL2_ENTER_MOD_COMP (1U << 10)
#define ASMEDIA1062_LNKCTL2_COMP_SOS       (1U << 11)
#define ASMEDIA1062_LNKCTL2_COMP_PRESET    0xf000

#define ASMEDIA1062_SEC_LNKCTL3           0x04
#define ASMEDIA1062_SEC_PERFORM_EQ        (1U << 0)
#define ASMEDIA1062_SEC_EQ_IRQ_EN         (1U << 1)
#define ASMEDIA1062_SEC_LANE_ERR          0x08
#define ASMEDIA1062_SEC_LANE_EQ           0x0c
#define ASMEDIA1062_SEC_LANE_EQ_MASK      0x7f7f

#define ASMEDIA1062_AER_UNC_SUPPORTED     (PCI_ERR_UNC_DLP | \
                                          PCI_ERR_UNC_SDN | \
                                          PCI_ERR_UNC_POISON_TLP | \
                                          PCI_ERR_UNC_FCP | \
                                          PCI_ERR_UNC_COMP_TIME | \
                                          PCI_ERR_UNC_COMP_ABORT | \
                                          PCI_ERR_UNC_UNX_COMP | \
                                          PCI_ERR_UNC_RX_OVER | \
                                          PCI_ERR_UNC_MALF_TLP | \
                                          PCI_ERR_UNC_ECRC | \
                                          PCI_ERR_UNC_UNSUP | \
                                          PCI_ERR_UNC_ACSV)
#define ASMEDIA1062_AER_UNC_SEVERITY      (PCI_ERR_UNC_DLP | \
                                          PCI_ERR_UNC_SDN | \
                                          PCI_ERR_UNC_FCP | \
                                          PCI_ERR_UNC_RX_OVER | \
                                          PCI_ERR_UNC_MALF_TLP)
#define ASMEDIA1062_AER_COR_SUPPORTED     (PCI_ERR_COR_RCVR | \
                                          PCI_ERR_COR_BAD_TLP | \
                                          PCI_ERR_COR_BAD_DLLP | \
                                          PCI_ERR_COR_REP_ROLL | \
                                          PCI_ERR_COR_REP_TIMER | \
                                          PCI_ERR_COR_ADV_NONFATAL)

struct ASMedia1062State {
    PCIDevice parent_obj;
    AHCIState ahci;
    IRQState irq;
    MemoryRegion bar0;
    MemoryRegion abar;
    MemoryRegion rom;
};

static uint64_t asmedia1062_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static uint64_t asmedia1062_rom_read(void *opaque, hwaddr addr, unsigned size)
{
    return UINT64_MAX;
}

static void asmedia1062_reserved_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
}

static const MemoryRegionOps asmedia1062_bar0_ops = {
    .read = asmedia1062_bar0_read,
    .write = asmedia1062_reserved_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static const MemoryRegionOps asmedia1062_rom_ops = {
    .read = asmedia1062_rom_read,
    .write = asmedia1062_reserved_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static void asmedia1062_update_irq(void *opaque, int irq_num, int level)
{
    ASMedia1062State *s = opaque;
    PCIDevice *dev = PCI_DEVICE(s);

    if (msi_enabled(dev)) {
        if (level) {
            msi_notify(dev, 0);
        }
    } else {
        pci_set_irq(dev, level);
    }
}

static unsigned asmedia1062_device_port(ASMedia1062State *s, DeviceState *dev)
{
    IDEBus *bus = IDE_BUS(dev->parent_bus);
    unsigned port = bus->bus_id;

    assert(port < s->ahci.ports);
    assert(dev->parent_bus == BUS(&s->ahci.dev[port].port));
    return port;
}

static void asmedia1062_plug(HotplugHandler *handler, DeviceState *dev,
                            Error **errp)
{
    ASMedia1062State *s = ASMEDIA1062(handler);
    unsigned port = asmedia1062_device_port(s, dev);

    if (dev->hotplugged) {
        ahci_port_device_changed(&s->ahci, port, true);
    }
}

static void asmedia1062_unplug(HotplugHandler *handler, DeviceState *dev,
                              Error **errp)
{
    ASMedia1062State *s = ASMEDIA1062(handler);
    unsigned port = asmedia1062_device_port(s, dev);

    ahci_port_device_changed(&s->ahci, port, false);
    qdev_simple_device_unplug_cb(handler, dev, errp);
}

static void asmedia1062_reset_config(PCIDevice *dev)
{
    uint8_t *exp = dev->config + ASMEDIA1062_PCIE_OFFSET;
    uint8_t *aer = dev->config + ASMEDIA1062_AER_OFFSET;
    uint8_t *sec = dev->config + ASMEDIA1062_SEC_OFFSET;

    pci_set_word(dev->config + ASMEDIA1062_PM_OFFSET + PCI_PM_CTRL, 0);
    pcie_cap_deverr_reset(dev);
    pcie_cap_lnkctl_reset(dev);
    pci_set_word(exp + PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN |
                 PCI_EXP_DEVCTL_NOSNOOP_EN | PCI_EXP_DEVCTL_READRQ_512B);
    pci_set_word(exp + PCI_EXP_DEVSTA, 0);
    pci_set_word(exp + PCI_EXP_LNKCTL, 0);
    pci_set_word(exp + PCI_EXP_DEVCTL2, 0);
    pci_set_word(exp + PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_TLS_8_0GT);

    memset(aer + PCI_ERR_UNCOR_STATUS, 0,
           ASMEDIA1062_AER_SIZE - PCI_ERR_UNCOR_STATUS);
    pci_set_long(aer + PCI_ERR_UNCOR_SEVER, ASMEDIA1062_AER_UNC_SEVERITY);
    pci_set_long(aer + PCI_ERR_COR_MASK, PCI_ERR_COR_ADV_NONFATAL);
    memset(sec + ASMEDIA1062_SEC_LNKCTL3, 0,
           ASMEDIA1062_SEC_SIZE - ASMEDIA1062_SEC_LNKCTL3);
}

static bool asmedia1062_init_caps(PCIDevice *dev, Error **errp)
{
    uint8_t *exp = dev->config + ASMEDIA1062_PCIE_OFFSET;
    uint8_t *exp_wmask = dev->wmask + ASMEDIA1062_PCIE_OFFSET;
    int ret;

    memset(dev->wmask + PCI_STD_HEADER_SIZEOF, 0,
           PCIE_CONFIG_SPACE_SIZE - PCI_STD_HEADER_SIZEOF);
    ret = pcie_endpoint_cap_init(dev, ASMEDIA1062_PCIE_OFFSET);
    if (ret < 0) {
        error_setg(errp, "Failed to initialize ASMedia1062 PCIe capability");
        return false;
    }

    ret = msi_init(dev, ASMEDIA1062_MSI_OFFSET, 1, true, false, NULL);
    assert(!ret || ret == -ENOTSUP);

    if (pci_pm_init(dev, ASMEDIA1062_PM_OFFSET, errp) < 0) {
        msi_uninit(dev);
        pcie_cap_exit(dev);
        return false;
    }
    pci_set_word(dev->config + ASMEDIA1062_PM_OFFSET + PCI_PM_PMC, 0xc823);
    pci_set_word(dev->wmask + ASMEDIA1062_PM_OFFSET + PCI_PM_CTRL,
                 PCI_PM_CTRL_STATE_MASK | PCI_PM_CTRL_PME_ENABLE);
    pci_set_word(dev->w1cmask + ASMEDIA1062_PM_OFFSET + PCI_PM_CTRL,
                 PCI_PM_CTRL_PME_STATUS);

    pci_set_long(exp + PCI_EXP_DEVCAP, 0x05908fe1);
    pci_set_long(exp + PCI_EXP_LNKCAP, 0x00476c13);
    pcie_cap_fill_link_ep_usp(dev, QEMU_PCI_EXP_LNK_X1,
                             QEMU_PCI_EXP_LNK_8GT, false);
    pcie_cap_deverr_init(dev);
    pcie_cap_lnkctl_init(dev);
    pci_word_test_and_set_mask(exp_wmask + PCI_EXP_DEVCTL,
                               PCI_EXP_DEVCTL_RELAX_EN |
                               PCI_EXP_DEVCTL_PAYLOAD |
                               PCI_EXP_DEVCTL_EXT_TAG |
                               PCI_EXP_DEVCTL_AUX_PME |
                               PCI_EXP_DEVCTL_NOSNOOP_EN |
                               PCI_EXP_DEVCTL_READRQ);
    pci_word_test_and_set_mask(exp_wmask + PCI_EXP_LNKCTL,
                               PCI_EXP_LNKCTL_ASPMC |
                               PCI_EXP_LNKCTL_RCB |
                               PCI_EXP_LNKCTL_CLKREQ_EN |
                               PCI_EXP_LNKCTL_HAWD);
    pci_set_long(exp + PCI_EXP_DEVCAP2, 0);
    pci_set_word(exp_wmask + PCI_EXP_DEVCTL2, 0);
    pci_set_word(exp_wmask + PCI_EXP_LNKCTL2,
                 PCI_EXP_LNKCTL2_TLS | PCI_EXP_LNKCTL2_ENTER_COMP |
                 PCI_EXP_LNKCTL2_HASD | PCI_EXP_LNKCTL2_TX_MARGIN |
                 ASMEDIA1062_LNKCTL2_ENTER_MOD_COMP |
                 ASMEDIA1062_LNKCTL2_COMP_SOS |
                 ASMEDIA1062_LNKCTL2_COMP_PRESET);

    /* AER 日志区会覆盖紧邻的 Secondary capability。 */
    pcie_add_capability(dev, PCI_EXT_CAP_ID_ERR, 1,
                        ASMEDIA1062_AER_OFFSET, ASMEDIA1062_AER_SIZE);
    pci_set_long(dev->w1cmask + ASMEDIA1062_AER_OFFSET + PCI_ERR_UNCOR_STATUS,
                 ASMEDIA1062_AER_UNC_SUPPORTED);
    pci_set_long(dev->wmask + ASMEDIA1062_AER_OFFSET + PCI_ERR_UNCOR_MASK,
                 ASMEDIA1062_AER_UNC_SUPPORTED);
    pci_set_long(dev->wmask + ASMEDIA1062_AER_OFFSET + PCI_ERR_UNCOR_SEVER,
                 ASMEDIA1062_AER_UNC_SUPPORTED);
    pci_set_long(dev->w1cmask + ASMEDIA1062_AER_OFFSET + PCI_ERR_COR_STATUS,
                 ASMEDIA1062_AER_COR_SUPPORTED);
    pci_set_long(dev->wmask + ASMEDIA1062_AER_OFFSET + PCI_ERR_COR_MASK,
                 ASMEDIA1062_AER_COR_SUPPORTED);

    pcie_add_capability(dev, PCI_EXT_CAP_ID_SECPCI, 1,
                        ASMEDIA1062_SEC_OFFSET, ASMEDIA1062_SEC_SIZE);
    pci_set_long(dev->wmask + ASMEDIA1062_SEC_OFFSET + ASMEDIA1062_SEC_LNKCTL3,
                 ASMEDIA1062_SEC_PERFORM_EQ | ASMEDIA1062_SEC_EQ_IRQ_EN);
    pci_set_long(dev->w1cmask + ASMEDIA1062_SEC_OFFSET +
                 ASMEDIA1062_SEC_LANE_ERR, 1);
    pci_set_word(dev->wmask + ASMEDIA1062_SEC_OFFSET + ASMEDIA1062_SEC_LANE_EQ,
                 ASMEDIA1062_SEC_LANE_EQ_MASK);
    asmedia1062_reset_config(dev);
    return true;
}

static void asmedia1062_write_config(PCIDevice *dev, uint32_t addr,
                                    uint32_t val, int len)
{
    pci_default_write_config(dev, addr, val, len);
    pci_long_test_and_clear_mask(dev->config + ASMEDIA1062_SEC_OFFSET +
                                 ASMEDIA1062_SEC_LNKCTL3,
                                 ASMEDIA1062_SEC_PERFORM_EQ);
}

static void asmedia1062_reset(DeviceState *dev)
{
    ASMedia1062State *s = ASMEDIA1062(dev);

    ahci_reset(&s->ahci);
    asmedia1062_reset_config(PCI_DEVICE(dev));
}

static void asmedia1062_init(Object *obj)
{
    ASMedia1062State *s = ASMEDIA1062(obj);

    qemu_init_irq_child(obj, "update-irq", &s->irq,
                        asmedia1062_update_irq, s, 0);
    ahci_init(&s->ahci, DEVICE(obj));
    s->ahci.irq = &s->irq;
    object_property_set_uint(obj, "romsize", 512 * KiB, &error_abort);
}

static void asmedia1062_realize(PCIDevice *dev, Error **errp)
{
    ASMedia1062State *s = ASMEDIA1062(dev);
    unsigned i;

    if (!asmedia1062_init_caps(dev, errp)) {
        return;
    }

    pci_config_set_prog_interface(dev->config, AHCI_PROGMODE_MAJOR_REV_1);
    dev->config[PCI_CACHE_LINE_SIZE] = 0x10;
    pci_config_set_interrupt_pin(dev->config, 1);

    s->ahci.ports = ASMEDIA1062_PORTS;
    ahci_realize(&s->ahci, DEVICE(dev), pci_get_address_space(dev));
    for (i = 0; i < s->ahci.ports; i++) {
        s->ahci.dev[i].port_regs.cmd |= PORT_CMD_HPCP;
        qbus_set_hotplug_handler(BUS(&s->ahci.dev[i].port), OBJECT(s));
    }

    memory_region_init_io(&s->bar0, OBJECT(s), &asmedia1062_bar0_ops, s,
                          "asmedia1062-bar0", ASMEDIA1062_BAR_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    memory_region_init(&s->abar, OBJECT(s), "asmedia1062-abar",
                       ASMEDIA1062_BAR_SIZE);
    memory_region_add_subregion(&s->abar, 0, &s->ahci.mem);
    pci_register_bar(dev, 5, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->abar);

    if ((!dev->romfile || !dev->romfile[0]) && dev->rom_bar != 0) {
        memory_region_init_io(&s->rom, OBJECT(s), &asmedia1062_rom_ops, s,
                              "asmedia1062-rom", dev->romsize);
        pci_register_bar(dev, PCI_ROM_SLOT, 0, &s->rom);
    }
}

static void asmedia1062_exit(PCIDevice *dev)
{
    ASMedia1062State *s = ASMEDIA1062(dev);
    unsigned i;

    for (i = 0; i < s->ahci.ports; i++) {
        qbus_set_hotplug_handler(BUS(&s->ahci.dev[i].port), NULL);
    }
    s->ahci.control_regs.ghc &= ~HOST_CTL_IRQ_EN;
    ahci_uninit(&s->ahci);
    memory_region_del_subregion(&s->abar, &s->ahci.mem);
    msi_uninit(dev);
    pci_del_capability(dev, PCI_CAP_ID_PM, PCI_PM_SIZEOF);
    dev->cap_present &= ~QEMU_PCI_CAP_PM;
    dev->pm_cap = 0;
    pcie_cap_exit(dev);
}

static const VMStateDescription vmstate_asmedia1062 = {
    .name = TYPE_ASMEDIA1062,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, ASMedia1062State),
        VMSTATE_AHCI(ahci, ASMedia1062State),
        VMSTATE_END_OF_LIST()
    },
};

static void asmedia1062_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(klass);

    pc->realize = asmedia1062_realize;
    pc->exit = asmedia1062_exit;
    pc->config_write = asmedia1062_write_config;
    pc->vendor_id = 0x1b21;
    pc->device_id = 0x1062;
    pc->revision = 0x02;
    pc->class_id = PCI_CLASS_STORAGE_SATA;
    pc->subsystem_vendor_id = 0x1b21;
    pc->subsystem_id = 0x2116;
    dc->desc = "ASMedia ASM1062 AHCI SATA controller";
    dc->vmsd = &vmstate_asmedia1062;
    device_class_set_legacy_reset(dc, asmedia1062_reset);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    hc->plug = asmedia1062_plug;
    hc->unplug = asmedia1062_unplug;
}

static const TypeInfo asmedia1062_info = {
    .name = TYPE_ASMEDIA1062,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ASMedia1062State),
    .instance_init = asmedia1062_init,
    .class_init = asmedia1062_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { TYPE_HOTPLUG_HANDLER },
        { },
    },
};

static void asmedia1062_register_types(void)
{
    type_register_static(&asmedia1062_info);
}

type_init(asmedia1062_register_types)
