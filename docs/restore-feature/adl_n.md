# Alder Lake-N PCIe 拓扑维护记录

本文记录在当前 QEMU 树中新增 Alder Lake-N PCIe Root Port 外观设备的背景、实现方式、构建入口、命令行拓扑、NVMe 盘位挂载、UEFI 启动和排错方法。

这份文档面向后续维护者。目标不是讲 PCIe 规范，而是说明如何在当前 QEMU 代码树里维护一组用于复刻实机 `lspci` 拓扑的定制 root port，并为后续补齐 NVMe、网卡和 PCH endpoint 外观留出清晰路径。

## 目标和边界

本次新增的目标：

- 让 QEMU 可以创建一组带 Intel Alder Lake-N PCH Root Port PCI ID 的 PCIe bridge。
- 让 `lspci -nn` 中的 root-port bridge 层显示为 Intel ID，而不是默认的 Red Hat `pcie-root-port` ID。
- 让命令行可以把这些 root port 固定放在 `00:1c.0`、`00:1c.1`、`00:1c.2`、`00:1c.3`、`00:1c.6`、`00:1d.0`、`00:1d.2`、`00:1d.3`。
- 让 NVMe endpoint 可以挂在指定 root port 下，得到类似 `+-1d.0-[06]----00.0` 的树形结构。
- 复用 QEMU 现有 PCIe Root Port 实现，包括 bridge window、secondary bus、slot、AER、ACS 和 MSI-X。

本次新增不保证：

- 完整复刻实机所有 PCI config space 字节。
- 完整复刻 `lspci -vv` 中的 Link Capabilities、Link Status、PM、AER、ACS、Slot Capabilities 等细节。
- 自动创建整机上所有 Alder Lake-N PCH endpoint，例如 xHCI、CNVi、HECI、SMBus、SPI、HDA 等。
- 让 NVMe endpoint 自动显示为 `8086:2522` 或 `1e4b:1202`。当前只是把 QEMU 的 NVMe 控制器挂到对应 root port 下。
- 保证 guest 中 bus number 永远与实机完全一致。bus number 是固件枚举结果，当前通过创建顺序和拓扑位置尽量靠齐。

如果目标是 `lspci -nntvv` 级别完美复刻，需要额外收集实机每个设备的 `lspci -xxxx`、`lspci -vv`、ACPI 表、固件枚举结果和 kernel dmesg，再继续补 endpoint 外观、capability 字段、BAR 尺寸、link speed/width、slot capability 和 ACPI 描述。

## 目标实机拓扑

参考目标输出：

```text
-1c.2-[03]----00.0  Intel Corporation NVMe Optane Memory Series [8086:2522]
-1c.3-[04]----00.0  Intel Corporation NVMe Optane Memory Series [8086:2522]
-1c.6-[05]----00.0  Intel Corporation NVMe Optane Memory Series [8086:2522]
-1d.0-[06]----00.0  MAXIO Technology (Hangzhou) Ltd. NVMe SSD Controller MAP1202 [1e4b:1202]
-1d.2-[07]----00.0  Intel Corporation NVMe Optane Memory Series [8086:2522]
\-1d.3-[08]----00.0  Intel Corporation NVMe Optane Memory Series [8086:2522]
```

对应 root port PCI ID：

```text
00:1c.0  8086:54b8
00:1c.1  8086:54b9
00:1c.2  8086:54ba
00:1c.3  8086:54bb
00:1c.6  8086:54be
00:1d.0  8086:54b0
00:1d.2  8086:54b2
00:1d.3  8086:54b3
```

当前实现优先复刻 root port 位置和 root port PCI ID。NVMe endpoint 的 vendor/device ID 仍是 QEMU NVMe 默认值，后续可以继续扩展。

## QEMU PCIe Bridge 相关代码

维护这个功能时，先理解这些文件的职责：

```text
hw/pci/pci_bridge.c
include/hw/pci/pci_bridge.h
hw/pci/pcie_port.c
include/hw/pci/pcie_port.h
hw/pci-bridge/pcie_root_port.c
hw/pci-bridge/gen_pcie_root_port.c
hw/pci-bridge/ioh3420.c
hw/pci-bridge/adl_n_root_port.c
hw/pci-bridge/meson.build
```

核心职责：

- `hw/pci/pci_bridge.c` 提供通用 PCI bridge 逻辑，包括 secondary bus、I/O window、MMIO window、prefetchable window、config write 后更新 mapping、reset、exit。
- `hw/pci/pcie_port.c` 提供 PCIe port 和 PCIe slot 的基础属性，例如 `port`、`chassis`、`slot`。
- `hw/pci-bridge/pcie_root_port.c` 是 QEMU 通用 PCIe Root Port 基类，处理 root port realize、AER、ACS、slot、hotplug、reset。
- `hw/pci-bridge/gen_pcie_root_port.c` 是默认 `pcie-root-port` 设备，vendor/device ID 是 QEMU/Red Hat 风格。
- `hw/pci-bridge/ioh3420.c` 是一个 Intel root port 的历史样例，适合参考“只改 class 参数”的写法。
- `hw/pci-bridge/adl_n_root_port.c` 是本次新增的 Alder Lake-N root port 外观层。

不要在 `adl_n_root_port.c` 里重新实现 bridge window 或 secondary bus。这些由 `TYPE_PCIE_ROOT_PORT` 及其父类完成。新增文件只负责定义不同 QOM type 的可见 PCI 身份。

## 为什么继承 TYPE_PCIE_ROOT_PORT

有三种可能做法：

```text
1. 从 TYPE_PCI_BRIDGE 继承，自己实现完整 PCIe root port
2. 从 TYPE_PCIE_SLOT / TYPE_PCIE_PORT 继承，复制 pcie_root_port.c 的逻辑
3. 从 TYPE_PCIE_ROOT_PORT 继承，只设置 class 参数
```

当前选择第 3 种。

原因：

- `TYPE_PCIE_ROOT_PORT` 已经实现 root port 所需的标准行为。
- Alder Lake-N root port 当前主要是为了让 guest 枚举看到指定 Intel PCI ID。
- 继承基类能减少与上游 QEMU PCIe 逻辑的分叉。
- 后续如果 QEMU 修复 root port、AER、slot、ACS、hotplug 行为，新类型能自动继承。

这也意味着当前实现不是 Alder Lake-N PCH root port 的完整硬件模型，而是“基于 QEMU 通用 root port 的 Alder Lake-N PCI ID 外观设备”。

## 新增文件

核心文件：

```text
hw/pci-bridge/adl_n_root_port.c
```

该文件定义 8 个 QOM 设备类型：

```text
adl-n-pcie-root-port-1c0
adl-n-pcie-root-port-1c1
adl-n-pcie-root-port-1c2
adl-n-pcie-root-port-1c3
adl-n-pcie-root-port-1c6
adl-n-pcie-root-port-1d0
adl-n-pcie-root-port-1d2
adl-n-pcie-root-port-1d3
```

这些类型都继承：

```c
.parent = TYPE_PCIE_ROOT_PORT
```

实例大小没有自定义结构体，因为当前没有 per-device runtime state。所有差异都来自 `class_data` 中的 device ID 和描述字符串。

## 设备 ID 表

新增文件中用一个表描述每个 root port：

```c
typedef struct ADLNRootPortInfo {
    const char *name;
    uint16_t device_id;
    const char *desc;
} ADLNRootPortInfo;
```

设备映射：

```text
adl-n-pcie-root-port-1c0 -> 0x54b8
adl-n-pcie-root-port-1c1 -> 0x54b9
adl-n-pcie-root-port-1c2 -> 0x54ba
adl-n-pcie-root-port-1c3 -> 0x54bb
adl-n-pcie-root-port-1c6 -> 0x54be
adl-n-pcie-root-port-1d0 -> 0x54b0
adl-n-pcie-root-port-1d2 -> 0x54b2
adl-n-pcie-root-port-1d3 -> 0x54b3
```

QOM 注册使用 `type_register_static_array()`，每个 `TypeInfo` 通过 `class_data` 指向对应的 `ADLNRootPortInfo`。

这样避免写 8 份重复 class init 函数。

## Class Init 说明

`adl_n_root_port_class_init()` 是最重要的函数。它设置：

```c
k->vendor_id = PCI_VENDOR_ID_INTEL;
k->device_id = info->device_id;
k->revision = ADL_N_ROOT_PORT_REVISION;
dc->desc = info->desc;
dc->vmsd = &vmstate_adl_n_root_port;
```

这些字段影响 `lspci -nn` 中看到的 vendor/device ID 和 revision。

Root port class 参数：

```c
rpc->aer_vector = adl_n_root_port_aer_vector;
rpc->interrupts_init = adl_n_root_port_interrupts_init;
rpc->interrupts_uninit = adl_n_root_port_interrupts_uninit;
rpc->exp_offset = ADL_N_ROOT_PORT_EXP_OFFSET;
rpc->aer_offset = ADL_N_ROOT_PORT_AER_OFFSET;
rpc->acs_offset = ADL_N_ROOT_PORT_ACS_OFFSET;
rpc->ssvid_offset = ADL_N_ROOT_PORT_SSVID_OFFSET;
rpc->ssid = 0;
```

当前 offset 取值：

```text
SSVID offset  0x40
PCIe offset   0x90
AER offset    0x100
ACS offset    0x100 + PCI_ERR_SIZEOF
```

这些值参考 QEMU 现有 root port 写法，尚未按实机 `lspci -xxxx` 校准。后续如果要做更真实的 `lspci -vv`，应先拿到实机 root port config dump，再决定是否调整。

## Root Port Realize 路径

`adl_n_root_port.c` 没有自己的 realize。它继承 `hw/pci-bridge/pcie_root_port.c` 中的 `rp_realize()`。

关键流程：

```c
pci_config_set_interrupt_pin(d->config, 1);
pci_bridge_initfn(d, TYPE_PCIE_BUS);
pcie_port_init_reg(d);
pci_bridge_ssvid_init(d, rpc->ssvid_offset, dc->vendor_id, rpc->ssid, errp);
rpc->interrupts_init(d, errp);
pcie_cap_init(d, rpc->exp_offset, PCI_EXP_TYPE_ROOT_PORT, p->port, errp);
pcie_cap_arifwd_init(d);
pcie_cap_deverr_init(d);
pcie_cap_slot_init(d, s);
pcie_cap_root_init(d);
pcie_chassis_create(s->chassis);
pcie_chassis_add_slot(s);
pcie_aer_init(d, PCI_ERR_VER, rpc->aer_offset, PCI_ERR_SIZEOF, errp);
pcie_aer_root_init(d);
pcie_acs_init(d, rpc->acs_offset);
```

这解释了为什么命令行要传：

```text
port=<n>
chassis=<n>
slot=<n>
```

`port` 会进入 PCIe capability 的 port number。`chassis` 和 `slot` 用于 QEMU PCIe slot/chassis 管理，避免 slot 冲突。

## VMState 说明

新增类型使用：

```c
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
```

这里直接继承 `TYPE_PCIE_ROOT_PORT`，实际对象布局是：

```text
PCIESlot
  PCIEPort parent_obj
    PCIBridge parent_obj
      PCIDevice parent_obj
```

所以 `VMSTATE_PCI_DEVICE(parent_obj.parent_obj.parent_obj, PCIESlot)` 指向 `PCIDevice`。

如果后续新增自己的 instance struct，例如：

```c
typedef struct ADLNRootPort {
    PCIESlot parent_obj;
    ...
} ADLNRootPort;
```

则 VMState 字段路径要重新调整，不能照抄当前写法。

## MSI-X 说明

当前中断初始化使用：

```c
msix_init_exclusive_bar(d, ADL_N_ROOT_PORT_MSIX_NR_VECTOR, 0, errp);
msix_vector_use(d, 0);
```

这与 QEMU generic `pcie-root-port` 的做法接近。AER vector 固定返回 0：

```c
static uint8_t adl_n_root_port_aer_vector(const PCIDevice *d)
{
    return 0;
}
```

如果后续希望更贴近实机，需要比较实机 root port 的 MSI/MSI-X capability。某些 Intel root port 样例使用 MSI 而不是 MSI-X，例如 `ioh3420.c`。是否切换取决于目标实机 config space。

## 构建入口

构建入口在：

```text
hw/pci-bridge/meson.build
```

`adl_n_root_port.c` 跟随 `CONFIG_PCIE_PORT` 编译：

```meson
pci_ss.add(when: 'CONFIG_PCIE_PORT', if_true: files('pcie_root_port.c',
                                                    'gen_pcie_root_port.c',
                                                    'adl_n_root_port.c'))
```

这样只要构建中包含普通 `pcie-root-port`，Alder Lake-N root port 类型也会被编进系统模拟器。

不单独新增 Kconfig 项的原因：

- 该文件依赖 `TYPE_PCIE_ROOT_PORT`。
- 它和 generic root port 属于同一类功能。
- 单独 Kconfig 项容易出现源码已改但 build 配置没启用，导致运行时报 device model name 无效。

## 构建和验证

MINGW64 环境示例：

```sh
cd /d/git/GreenDamTan/qemu/build
meson setup --reconfigure . ..
ninja qemu-system-x86_64.exe
```

确认 build graph 里包含新文件：

```sh
grep adl_n_root_port build.ninja
```

确认二进制里包含新 QOM 类型：

```sh
strings ./qemu-system-x86_64.exe | grep adl-n-pcie-root-port
```

确认 QEMU device help 能列出新类型：

```sh
./qemu-system-x86_64.exe -device help | grep adl-n
```

期望输出包含：

```text
adl-n-pcie-root-port-1c0
adl-n-pcie-root-port-1c1
adl-n-pcie-root-port-1c2
adl-n-pcie-root-port-1c3
adl-n-pcie-root-port-1c6
adl-n-pcie-root-port-1d0
adl-n-pcie-root-port-1d2
adl-n-pcie-root-port-1d3
```

如果 `build.ninja` 里没有 `adl_n_root_port.c`，说明 Meson 没有重新生成。先跑 `meson setup --reconfigure . ..`。

如果 `build.ninja` 有新文件，但 `strings` 没有输出，说明没有重新链接目标 exe。跑 `ninja qemu-system-x86_64.exe`。

## 命令行 Root Port 拓扑

Root port 固定拓扑：

```sh
-device adl-n-pcie-root-port-1c0,id=rp01,bus=pcie.0,addr=1c.0,multifunction=on,port=0,chassis=1,slot=1 \
-device adl-n-pcie-root-port-1c1,id=rp02,bus=pcie.0,addr=1c.1,port=1,chassis=2,slot=2 \
-device adl-n-pcie-root-port-1c2,id=rp03,bus=pcie.0,addr=1c.2,port=2,chassis=3,slot=3 \
-device adl-n-pcie-root-port-1c3,id=rp04,bus=pcie.0,addr=1c.3,port=3,chassis=4,slot=4 \
-device adl-n-pcie-root-port-1c6,id=rp05,bus=pcie.0,addr=1c.6,port=6,chassis=5,slot=5 \
-device adl-n-pcie-root-port-1d0,id=rp06,bus=pcie.0,addr=1d.0,multifunction=on,port=8,chassis=6,slot=6 \
-device adl-n-pcie-root-port-1d2,id=rp07,bus=pcie.0,addr=1d.2,port=10,chassis=7,slot=7 \
-device adl-n-pcie-root-port-1d3,id=rp08,bus=pcie.0,addr=1d.3,port=11,chassis=8,slot=8
```

字段解释：

```text
id=rp03          QEMU 对象 ID，同时作为该 root port 的 secondary bus 名
bus=pcie.0       挂在 q35 root complex 的 PCIe root bus 上
addr=1c.2        固定 root port 在 bus 0 的 slot/function
multifunction=on 同一 slot 下存在多个 function 时，function 0 必须设置
port=2           PCIe capability 中的 port number
chassis=3        QEMU PCIe chassis 编号
slot=3           QEMU PCIe physical slot 编号
```

`multifunction=on` 必须放在同一 slot 的 function 0 上。这里 `1c.0` 和 `1d.0` 是 function 0，所以它们需要该属性。`1c.1`、`1c.2`、`1c.3`、`1c.6`、`1d.2`、`1d.3` 不需要重复设置。

## NVMe 挂载关系

QEMU 的 root port `id` 同时也是该 root port 的 secondary bus 名。把 endpoint 挂到某个 root port 下时，使用：

```text
bus=<root-port-id>,addr=0.0
```

推荐盘位关系：

```text
1c.2 -> rp03 -> nvm_1c2
1c.3 -> rp04 -> nvm_1c3
1c.6 -> rp05 -> nvm_1c6
1d.0 -> rp06 -> nvm_sys  系统盘
1d.2 -> rp07 -> nvm_1d2
1d.3 -> rp08 -> nvm_1d3
```

命令行片段：

```sh
-drive file=/d/vm/qemu_fnos/nvme_1c2.img,format=raw,if=none,id=nvm_1c2 \
-device nvme,serial=nvm00000001,drive=nvm_1c2,bus=rp03,addr=0.0 \
-drive file=/d/vm/qemu_fnos/nvme_1c3.img,format=raw,if=none,id=nvm_1c3 \
-device nvme,serial=nvm00000002,drive=nvm_1c3,bus=rp04,addr=0.0 \
-drive file=/d/vm/qemu_fnos/nvme_1c6.img,format=raw,if=none,id=nvm_1c6 \
-device nvme,serial=nvm00000003,drive=nvm_1c6,bus=rp05,addr=0.0 \
-drive file=/d/vm/qemu_fnos/sys.qcow2,format=qcow2,if=none,id=nvm_sys \
-device nvme,serial=nvm00000004,drive=nvm_sys,bus=rp06,addr=0.0,bootindex=1 \
-drive file=/d/vm/qemu_fnos/nvme_1d2.img,format=raw,if=none,id=nvm_1d2 \
-device nvme,serial=nvm00000005,drive=nvm_1d2,bus=rp07,addr=0.0 \
-drive file=/d/vm/qemu_fnos/nvme_1d3.img,format=raw,if=none,id=nvm_1d3 \
-device nvme,serial=nvm00000006,drive=nvm_1d3,bus=rp08,addr=0.0
```

系统盘放在 `1d.0-[06]` 下时，应给对应 NVMe device 加：

```text
bootindex=1
```

否则固件可能先尝试其它数据盘。

## Bus Number 说明

目标拓扑中有：

```text
1c.2-[03]
1c.3-[04]
1c.6-[05]
1d.0-[06]
1d.2-[07]
1d.3-[08]
```

这些 `[03]`、`[04]`、`[05]` 是 secondary bus number。当前 QEMU 下它们通常由固件按 root port 枚举顺序分配。只要 root port 创建顺序固定，一般能得到稳定的递增 bus number。

需要注意：

- QEMU 命令行中的 `id=rp03` 不是 guest 看到的 bus number。
- `bus=rp03` 表示把 endpoint 挂到该 root port 的 secondary bus 上。
- guest 里最终显示 `[03]` 还是 `[04]` 取决于固件分配。
- 若加入额外 PCIe 设备，bus number 可能变化。

如果必须强制 bus number，需要更深层修改固件枚举或使用额外 host bridge/pxb 方案。当前实现不做这件事。

## BIOS 启动示例

SeaBIOS 启动示例，系统盘挂在 `1d.0` 下：

```sh
./build/qemu-system-x86_64.exe \
  -machine q35 \
  -m 2048 \
  -smp 4,sockets=1,cores=4,threads=1 \
  -cpu Skylake-Client-noTSX-IBRS \
  -boot order=c \
  -accel whpx \
  -vga virtio \
  -device adl-n-pcie-root-port-1c0,id=rp01,bus=pcie.0,addr=1c.0,multifunction=on,port=0,chassis=1,slot=1 \
  -device adl-n-pcie-root-port-1c1,id=rp02,bus=pcie.0,addr=1c.1,port=1,chassis=2,slot=2 \
  -device adl-n-pcie-root-port-1c2,id=rp03,bus=pcie.0,addr=1c.2,port=2,chassis=3,slot=3 \
  -device adl-n-pcie-root-port-1c3,id=rp04,bus=pcie.0,addr=1c.3,port=3,chassis=4,slot=4 \
  -device adl-n-pcie-root-port-1c6,id=rp05,bus=pcie.0,addr=1c.6,port=6,chassis=5,slot=5 \
  -device adl-n-pcie-root-port-1d0,id=rp06,bus=pcie.0,addr=1d.0,multifunction=on,port=8,chassis=6,slot=6 \
  -device adl-n-pcie-root-port-1d2,id=rp07,bus=pcie.0,addr=1d.2,port=10,chassis=7,slot=7 \
  -device adl-n-pcie-root-port-1d3,id=rp08,bus=pcie.0,addr=1d.3,port=11,chassis=8,slot=8 \
  -drive file=/d/vm/qemu_fnos/sys.qcow2,format=qcow2,if=none,id=nvm_sys \
  -device nvme,serial=nvm00000004,drive=nvm_sys,bus=rp06,addr=0.0,bootindex=1 \
  -device e1000,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::5666-:5666,hostfwd=tcp::2222-:22
```

如果本机 22 端口没有占用，也可以改成：

```sh
hostfwd=tcp::22-:22
```

## UEFI 启动

不要用下面这种方式加载 `edk2-x86_64-code.fd`：

```sh
-bios ./build/pc-bios/edk2-x86_64-code.fd
```

该固件是 pflash 固件，应使用 `if=pflash`。

首次启动前复制一份可写 NVRAM 变量文件：

```sh
cp ./build/pc-bios/edk2-i386-vars.fd /d/vm/qemu_fnos/OVMF_VARS.fd
```

启动时加入：

```sh
-drive if=pflash,format=raw,unit=0,readonly=on,file=./build/pc-bios/edk2-x86_64-code.fd \
-drive if=pflash,format=raw,unit=1,file=/d/vm/qemu_fnos/OVMF_VARS.fd
```

UEFI 从 ISO 安装时使用：

```sh
-boot d \
-cdrom /d/BaiduYunDownload/fnos_Mainland-PE_x86_1.1.3107_1808.iso
```

安装完成后去掉 `-cdrom`，改回：

```sh
-boot order=c
```

UEFI 完整示例：

```sh
./build/qemu-system-x86_64.exe \
  -machine q35 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=./build/pc-bios/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,unit=1,file=/d/vm/qemu_fnos/OVMF_VARS.fd \
  -m 2048 \
  -smp 4,sockets=1,cores=4,threads=1 \
  -cpu Skylake-Client-noTSX-IBRS \
  -boot order=c \
  -accel whpx \
  -vga virtio \
  -device adl-n-pcie-root-port-1c0,id=rp01,bus=pcie.0,addr=1c.0,multifunction=on,port=0,chassis=1,slot=1 \
  -device adl-n-pcie-root-port-1c1,id=rp02,bus=pcie.0,addr=1c.1,port=1,chassis=2,slot=2 \
  -device adl-n-pcie-root-port-1c2,id=rp03,bus=pcie.0,addr=1c.2,port=2,chassis=3,slot=3 \
  -device adl-n-pcie-root-port-1c3,id=rp04,bus=pcie.0,addr=1c.3,port=3,chassis=4,slot=4 \
  -device adl-n-pcie-root-port-1c6,id=rp05,bus=pcie.0,addr=1c.6,port=6,chassis=5,slot=5 \
  -device adl-n-pcie-root-port-1d0,id=rp06,bus=pcie.0,addr=1d.0,multifunction=on,port=8,chassis=6,slot=6 \
  -device adl-n-pcie-root-port-1d2,id=rp07,bus=pcie.0,addr=1d.2,port=10,chassis=7,slot=7 \
  -device adl-n-pcie-root-port-1d3,id=rp08,bus=pcie.0,addr=1d.3,port=11,chassis=8,slot=8 \
  -drive file=/d/vm/qemu_fnos/sys.qcow2,format=qcow2,if=none,id=nvm_sys \
  -device nvme,serial=nvm00000004,drive=nvm_sys,bus=rp06,addr=0.0,bootindex=1 \
  -device e1000,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::5666-:5666,hostfwd=tcp::2222-:22
```

## Guest 内验证

进入 guest 后检查 root port：

```sh
lspci -nn | grep 'PCI bridge'
```

期望看到：

```text
00:1c.0 PCI bridge [0604]: Intel Corporation ... [8086:54b8]
00:1c.1 PCI bridge [0604]: Intel Corporation ... [8086:54b9]
00:1c.2 PCI bridge [0604]: Intel Corporation ... [8086:54ba]
00:1c.3 PCI bridge [0604]: Intel Corporation ... [8086:54bb]
00:1c.6 PCI bridge [0604]: Intel Corporation ... [8086:54be]
00:1d.0 PCI bridge [0604]: Intel Corporation ... [8086:54b0]
00:1d.2 PCI bridge [0604]: Intel Corporation ... [8086:54b2]
00:1d.3 PCI bridge [0604]: Intel Corporation ... [8086:54b3]
```

检查树形结构：

```sh
lspci -nnt
lspci -nntvv
```

检查某个 root port config space：

```sh
lspci -s 00:1c.2 -nnvv
lspci -s 00:1c.2 -xxxx
```

检查 NVMe：

```sh
lspci -nn | grep -i nvme
nvme list
lsblk
```

注意：当前 NVMe vendor/device ID 不会显示为目标实机 ID。拓扑位置先对齐，endpoint 外观后续再做。

## 常见问题

### 设备类型无效

报错：

```text
'adl-n-pcie-root-port-1c0' is not a valid device model name
```

原因通常是当前运行的 exe 没编进新设备。

检查：

```sh
grep adl_n_root_port build/build.ninja
strings ./build/qemu-system-x86_64.exe | grep adl-n-pcie-root-port
./build/qemu-system-x86_64.exe -device help | grep adl-n
```

没有输出就重新生成并编译：

```sh
cd /d/git/GreenDamTan/qemu/build
meson setup --reconfigure . ..
ninja qemu-system-x86_64.exe
```

### Root port function 冲突

如果在 `1c.1`、`1c.2` 等 function 上创建设备时报 multifunction 相关错误，检查 function 0：

```sh
-device adl-n-pcie-root-port-1c0,...,addr=1c.0,multifunction=on
```

同理 `1d.0` 也需要：

```sh
-device adl-n-pcie-root-port-1d0,...,addr=1d.0,multifunction=on
```

### NVMe 不作为系统盘启动

如果系统盘挂在 `rp06`，确保：

```sh
-device nvme,serial=nvm00000004,drive=nvm_sys,bus=rp06,addr=0.0,bootindex=1
```

并且安装完成后不要继续使用：

```sh
-boot d
```

改用：

```sh
-boot order=c
```

### 数据盘挡住系统盘

有多块 NVMe 时，固件可能先枚举数据盘。解决方式：

- 系统盘 device 加 `bootindex=1`。
- 数据盘不要加 bootindex。
- 安装阶段可以暂时只挂系统盘，安装完成后再挂回所有数据盘。

### UEFI 固件加载失败

报错：

```text
qemu: could not load PC BIOS './build/pc-bios/edk2-x86_64-code.fd'
```

原因是把 pflash 固件当成传统 BIOS 加载。改用：

```sh
-drive if=pflash,format=raw,unit=0,readonly=on,file=./build/pc-bios/edk2-x86_64-code.fd
```

### TCG CPU feature warning

使用：

```sh
-accel tcg
-cpu Skylake-Client-noTSX-IBRS
```

可能出现：

```text
TCG doesn't support requested feature: CPUID...
```

这些 warning 与 PCIe/UEFI 拓扑无关。若只需要跑通，可改用：

```sh
-cpu qemu64
```

如果宿主支持 WHPX，优先使用：

```sh
-accel whpx
```

### 本机 22 端口占用

如果使用：

```sh
-netdev user,id=net0,hostfwd=tcp::22-:22
```

但 Windows 本机已运行 OpenSSH Server 或其它程序占用 22 端口，QEMU 会启动失败。改用：

```sh
-netdev user,id=net0,hostfwd=tcp::2222-:22
```

宿主连接 guest：

```sh
ssh -p 2222 root@127.0.0.1
```

## 后续复刻 NVMe ID

当前 QEMU NVMe 控制器的 PCI ID 不是目标中的：

```text
Intel Optane  8086:2522
MAXIO MAP1202 1e4b:1202
```

后续有两种做法：

### 做法一：给 nvme 增加可配置 PCI ID

在 NVMe 设备 state 中增加参数：

```text
x-pci-vendor-id
x-pci-device-id
x-pci-revision
```

realize 时用这些参数覆盖：

```c
pci_config_set_vendor_id(pci_conf, params.vendor_id);
pci_config_set_device_id(pci_conf, params.device_id);
pci_conf[PCI_REVISION_ID] = params.revision;
```

优点：

- 一个 `nvme` 类型即可模拟不同外观。
- 命令行灵活。

缺点：

- 对上游风格来说，任意覆盖 PCI ID 可能需要谨慎命名为 experimental/x- 属性。

### 做法二：新增外观类型

新增：

```text
intel-optane-nvme
maxio-map1202-nvme
```

它们继承或复用 QEMU NVMe 逻辑，只设置固定 PCI ID。

优点：

- 命令行更接近硬件名。
- 不暴露任意 PCI ID 参数。

缺点：

- 需要更多 QOM 类型。
- 每新增一个外观就要维护一个类型。

如果目标是固定复刻某台机器，做法二更直接。如果目标是通用伪装，做法一更灵活。

## 后续补齐 PCH Endpoint

目标实机中还有很多 root bus 上的 PCH 设备，例如：

```text
00:14.0  USB xHCI             [8086:54ed]
00:14.2  Shared SRAM          [8086:54ef]
00:14.3  CNVi Wi-Fi           [8086:54f0]
00:15.0  Serial bus           [8086:54e8]
00:15.1  Serial bus           [8086:54e9]
00:16.0  HECI                 [8086:54e0]
00:1e.0  UART                 [8086:54a8]
00:1e.3  Serial bus           [8086:54ab]
00:1f.0  eSPI                 [8086:5481]
00:1f.3  HD Audio             [8086:54c8]
00:1f.4  SMBus                [8086:54a3]
00:1f.5  SPI                  [8086:54a4]
```

这些设备目前没有在本次 root port 工作中实现。

可选策略：

- 如果 guest 只依赖 `lspci` 外观，可以新增 dummy PCI devices，只提供 PCI config space 和 BAR 占位。
- 如果 guest 需要功能，例如 xHCI、HDA、SMBus，应优先复用 QEMU 已有功能设备，再调整 PCI ID。
- 如果只是为了安装/启动某个系统，先不要补太多 dummy 设备，避免驱动绑定后访问未实现寄存器导致异常。

## 是否要做专用 Machine

当前命令行很长，适合调试，但不适合长期使用。

后续可以新增一个专用 machine 或 machine 属性，例如：

```text
-machine q35,adl-n-topology=on
```

实现位置可考虑：

```text
hw/i386/pc_q35.c
```

在 q35 host bridge 和 root bus 创建完成后，自动创建 root port：

```c
PCIDevice *rp;

rp = pci_new_multifunction(PCI_DEVFN(0x1c, 0), "adl-n-pcie-root-port-1c0");
qdev_prop_set_uint8(DEVICE(rp), "port", 0);
qdev_prop_set_uint8(DEVICE(rp), "chassis", 1);
qdev_prop_set_uint16(DEVICE(rp), "slot", 1);
pci_realize_and_unref(rp, pcms->pcibus, &error_fatal);
```

这样用户命令行只需要挂 endpoint。缺点是会污染 machine ABI，需要考虑版本兼容和默认开关。

建议顺序：

1. 先稳定当前命令行方案。
2. 补齐 endpoint ID 复刻。
3. 再决定是否新增 machine 自动创建拓扑。

## 维护检查清单

修改 root port 实现后，至少检查：

```sh
meson setup --reconfigure . ..
ninja qemu-system-x86_64.exe
./qemu-system-x86_64.exe -device help | grep adl-n
strings ./qemu-system-x86_64.exe | grep adl-n-pcie-root-port
```

启动 guest 后检查：

```sh
lspci -nn | grep 'PCI bridge'
lspci -nnt
lspci -s 00:1c.2 -nnvv
lspci -s 00:1c.2 -xxxx
```

如果涉及迁移或保存恢复，还要测试 VMState。当前功能主要服务本地启动和拓扑复刻，暂未做迁移兼容性承诺。
