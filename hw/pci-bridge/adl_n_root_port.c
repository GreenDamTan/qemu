/*
 * Intel Alder Lake-N PCI Express Root Port device IDs.
 *
 * This provides QOM device types with the visible PCI identity of the
 * Alder Lake-N PCH root ports while reusing QEMU's generic root-port
 * implementation for bridge windows, secondary buses, slots and AER.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/msix.h"
#include "hw/pci/pcie_port.h"
#include "migration/vmstate.h"

#define TYPE_ADL_N_PCIE_ROOT_PORT_1C0 "adl-n-pcie-root-port-1c0"
#define TYPE_ADL_N_PCIE_ROOT_PORT_1C1 "adl-n-pcie-root-port-1c1"
#define TYPE_ADL_N_PCIE_ROOT_PORT_1C2 "adl-n-pcie-root-port-1c2"
#define TYPE_ADL_N_PCIE_ROOT_PORT_1C3 "adl-n-pcie-root-port-1c3"
#define TYPE_ADL_N_PCIE_ROOT_PORT_1C6 "adl-n-pcie-root-port-1c6"
#define TYPE_ADL_N_PCIE_ROOT_PORT_1D0 "adl-n-pcie-root-port-1d0"
#define TYPE_ADL_N_PCIE_ROOT_PORT_1D2 "adl-n-pcie-root-port-1d2"
#define TYPE_ADL_N_PCIE_ROOT_PORT_1D3 "adl-n-pcie-root-port-1d3"

#define ADL_N_ROOT_PORT_REVISION       0x00
#define ADL_N_ROOT_PORT_SSVID_OFFSET   0x40
#define ADL_N_ROOT_PORT_EXP_OFFSET     0x90
#define ADL_N_ROOT_PORT_AER_OFFSET     0x100
#define ADL_N_ROOT_PORT_ACS_OFFSET \
        (ADL_N_ROOT_PORT_AER_OFFSET + PCI_ERR_SIZEOF)
#define ADL_N_ROOT_PORT_MSIX_NR_VECTOR 1

typedef struct ADLNRootPortInfo {
    const char *name;
    uint16_t device_id;
    const char *desc;
} ADLNRootPortInfo;

static uint8_t adl_n_root_port_aer_vector(const PCIDevice *d)
{
    return 0;
}

static int adl_n_root_port_interrupts_init(PCIDevice *d, Error **errp)
{
    int rc;

    rc = msix_init_exclusive_bar(d, ADL_N_ROOT_PORT_MSIX_NR_VECTOR, 0, errp);
    if (rc < 0) {
        assert(rc == -ENOTSUP);
    } else {
        msix_vector_use(d, 0);
    }

    return rc;
}

static void adl_n_root_port_interrupts_uninit(PCIDevice *d)
{
    msix_uninit_exclusive_bar(d);
}

static const VMStateDescription vmstate_adl_n_root_port = {
    .name = "adl-n-pcie-root-port",
    .priority = MIG_PRI_PCI_BUS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcie_cap_slot_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent_obj.parent_obj, PCIESlot),
        VMSTATE_STRUCT(parent_obj.parent_obj.parent_obj.exp.aer_log,
                       PCIESlot, 0, vmstate_pcie_aer_log, PCIEAERLog),
        VMSTATE_MSIX(parent_obj.parent_obj.parent_obj, PCIESlot),
        VMSTATE_END_OF_LIST()
    }
};

static void adl_n_root_port_class_init(ObjectClass *klass, const void *data)
{
    const ADLNRootPortInfo *info = data;
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    PCIERootPortClass *rpc = PCIE_ROOT_PORT_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = info->device_id;
    k->revision = ADL_N_ROOT_PORT_REVISION;
    dc->desc = info->desc;
    dc->vmsd = &vmstate_adl_n_root_port;

    rpc->aer_vector = adl_n_root_port_aer_vector;
    rpc->interrupts_init = adl_n_root_port_interrupts_init;
    rpc->interrupts_uninit = adl_n_root_port_interrupts_uninit;
    rpc->exp_offset = ADL_N_ROOT_PORT_EXP_OFFSET;
    rpc->aer_offset = ADL_N_ROOT_PORT_AER_OFFSET;
    rpc->acs_offset = ADL_N_ROOT_PORT_ACS_OFFSET;
    rpc->ssvid_offset = ADL_N_ROOT_PORT_SSVID_OFFSET;
    rpc->ssid = 0;
}

static const ADLNRootPortInfo adl_n_root_ports[] = {
    {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C0,
        .device_id = 0x54b8,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1c.0",
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C1,
        .device_id = 0x54b9,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1c.1",
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C2,
        .device_id = 0x54ba,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1c.2",
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C3,
        .device_id = 0x54bb,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1c.3",
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C6,
        .device_id = 0x54be,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1c.6",
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1D0,
        .device_id = 0x54b0,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1d.0",
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1D2,
        .device_id = 0x54b2,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1d.2",
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1D3,
        .device_id = 0x54b3,
        .desc = "Intel Alder Lake-N PCI Express Root Port 00:1d.3",
    },
};

static const TypeInfo adl_n_root_port_types[] = {
    {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C0,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[0],
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C1,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[1],
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C2,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[2],
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C3,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[3],
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1C6,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[4],
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1D0,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[5],
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1D2,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[6],
    }, {
        .name = TYPE_ADL_N_PCIE_ROOT_PORT_1D3,
        .parent = TYPE_PCIE_ROOT_PORT,
        .class_init = adl_n_root_port_class_init,
        .class_data = &adl_n_root_ports[7],
    },
};

static void adl_n_root_port_register_types(void)
{
    type_register_static_array(adl_n_root_port_types,
                               ARRAY_SIZE(adl_n_root_port_types));
}

type_init(adl_n_root_port_register_types)
