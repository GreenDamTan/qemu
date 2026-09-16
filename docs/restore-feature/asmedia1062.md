# ASMedia ASM1062 SATA 控制器

`asmedia1062` 是一个双端口 PCIe AHCI 控制器，复用 QEMU 的 AHCI 和 IDE 数据通路。客户机可使用标准 AHCI 驱动访问硬盘；每个端口可显式连接一个 `ide-hd`，支持通过 QMP 运行时插入、拔出和重新插入硬盘。磁盘镜像格式由 block backend 决定，可以使用 qcow2 或 raw。

## 创建控制器并指定盘位

建议使用 q35，并将控制器接到 PCIe root port 下游：

```sh
qemu-system-x86_64 \
  -machine q35 \
  -device pcie-root-port,id=sata_rp,chassis=1,slot=1 \
  -device asmedia1062,id=asm0,bus=sata_rp \
  -drive if=none,id=disk2,file=/path/to/disk2.qcow2,format=qcow2 \
  -device ide-hd,id=sata_disk2,drive=disk2,bus=asm0.1
```

将这些设备参数加入已有的虚拟机启动命令，并保留原有系统盘、固件和网络配置。控制器不会自动创建磁盘镜像。

| 物理盘位 | AHCI port | 显式 QEMU bus |
| --- | --- | --- |
| 盘位 1 | 0 | `asm0.0` |
| 盘位 2 | 1 | `asm0.1` |

盘位从 1 编号，QEMU bus 后缀从 0 编号。因此，把 qcow2 接到盘位 2 时使用 `bus=asm0.1`。每个端口只允许一个设备，`unit` 默认为 0，不能使用 `unit=1`。

也可以使用命名 block node：

```sh
-blockdev driver=file,node-name=disk2_file,filename=/path/to/disk2.qcow2 \
-blockdev driver=qcow2,node-name=disk2_node,file=disk2_file \
-device ide-hd,id=sata_disk2,drive=disk2_node,bus=asm0.1
```

连接两块磁盘时，为另一块磁盘分配独立的 backend 和设备 ID，并指定 `bus=asm0.0`。多个控制器也使用独立 ID，例如 `id=asm1` 的盘位是 `asm1.0` 和 `asm1.1`。不要省略 `bus=`，以免设备被自动分配到主板自带的 IDE/AHCI 控制器。

## QMP 热插拔

以下示例假定 `asm0` 已存在且盘位 2 空闲，镜像 `/path/to/hotdisk.qcow2` 已创建，并在启动命令中启用了 QMP，例如：

```sh
-qmp unix:/tmp/asmedia1062.qmp,server=on,wait=off
```

连接 QMP 后，先协商能力，再创建后端并插入盘位 2：

```json
{"execute":"qmp_capabilities"}
{"execute":"blockdev-add","arguments":{"driver":"file","node-name":"hotdisk_file","filename":"/path/to/hotdisk.qcow2"}}
{"execute":"blockdev-add","arguments":{"driver":"qcow2","node-name":"hotdisk_node","file":"hotdisk_file"}}
{"execute":"device_add","arguments":{"driver":"ide-hd","id":"hotdisk","drive":"hotdisk_node","bus":"asm0.1"}}
```

每条命令都应等待成功响应后再执行下一条。客户机 Linux 的 `ahci` 驱动会收到 SATA 连接变化中断并重扫，可用 `dmesg`、`lsblk` 和 `/dev/disk/by-id/` 确认新磁盘。客户机分配的 `/dev/sdX` 名称不保证固定。

拔出前，应在客户机停止对该盘的使用，卸载文件系统，并按需退出 RAID、LVM 或存储池；确认待写数据已完成写回。随后执行：

```json
{"execute":"device_del","arguments":{"id":"hotdisk"}}
```

等待 QMP `DEVICE_DELETED` 事件，并在客户机确认设备消失，然后删除不再使用的 block node：

```json
{"execute":"blockdev-del","arguments":{"node-name":"hotdisk_node"}}
{"execute":"blockdev-del","arguments":{"node-name":"hotdisk_file"}}
```

`device_del` 是同步的 surprise removal：QEMU 等待已提交的后端 I/O 收敛并移除磁盘，但不会替客户机卸载文件系统或刷新客户机内存中的缓存。QMP 返回成功和收到删除事件也不表示客户机已完成自身的设备清理。

拔出后可用不同后端重复上述 `blockdev-add`/`device_add` 流程，在同一 `asm0.1` 盘位重新插盘。插拔仅改变目标 SATA 端口，不复位整个控制器。已经占用的盘位、非法 unit、正在迁移时的设备删除，以及仍被引用的 block node 删除，按 QEMU 既有规则报错；失败的磁盘 realize 会释放盘位占用。通过 `blockdev-add` 显式命名的 node 由用户删除；legacy `-drive` 后端遵循 QEMU 原有的自动删除语义。

现有 AHCI 的 `ide-cd` 连接能力保留，本文的运行时热插拔使用方式针对 `ide-hd` 硬盘。

## PCI 外观与实现范围

设备身份以两份实机 `lspci` 转储的共同静态字段为参考：

| 项目 | 配置 |
| --- | --- |
| Vendor / Device | `1b21:1062` |
| Revision | `02` |
| Class / Programming Interface | SATA `0106` / AHCI `01` |
| Subsystem | `1b21:2116` |
| Cache Line Size | `0x10`，即 64 bytes |
| Interrupt Pin | A |
| BAR0 | 8 KiB，32-bit、non-prefetchable MMIO 保留区 |
| BAR5 | 8 KiB，32-bit、non-prefetchable MMIO，起始处为 AHCI 寄存器 |
| Expansion ROM | 默认 512 KiB 空白窗口，默认禁用 |
| PM / MSI / PCI Express | `0x40` / `0x50` / `0x80` |
| MSI | 单向量、64-bit 地址，无 per-vector masking |
| PCI Express | version 2，x1、8 GT/s，最大 payload 256 bytes |
| AER / Secondary PCI Express | version 1，`0x100` / `0x130` |

BDF、IRQ、IOMMU group、BAR/ROM 基址和 MSI message address/data 由虚拟机拓扑、固件和客户机决定，没有复制实机运行时地址。直接挂到 PCIe root complex 时，QEMU 的通用 PCIe helper 可能报告 Root Complex Integrated Endpoint；挂到 root port 下游时报告 Endpoint。PCIe 链路速率字段不代表实际吞吐量保证。

两个 SATA 端口使用现有 AHCI core 的能力和传输模型：`CAP.NP=1`、`PI=3`，并报告只读的 `PxCMD.HPCP`。连接变化置位 `PxSERR.DIAG.N`、`DIAG.X` 与 `PxIS.PCS`、`PRCS`，通过单向量 MSI 或 INTx 通知客户机。热插拔能力在控制器复位、客户机 HBA reset 和迁移后保留。冷启动挂盘不产生虚假的连接变化事件。

BAR0 的厂商寄存器语义尚未实现，保留区读为零、写入无效。BAR5 只包含标准 AHCI core 提供的寄存器。AER 和 Secondary PCI Express 只提供配置寄存器、可写掩码、W1C 与复位语义，不模拟物理 PCIe 错误、链路训练或通用 AER 注入日志。PM 提供标准配置状态与 PCI 通用 BAR 访问控制，不模拟物理功耗。该模型不包含 ASMedia RAID 固件或完整厂商私有功能。

## ROM 文件

默认空白 ROM 窗口读取为 `0xff`，不包含可执行固件。`romsize=` 可覆盖窗口大小；`rombar=0` 不创建空白 ROM BAR。指定非空 `romfile=` 时由 QEMU 通用 PCI ROM loader 加载，不再注册空白窗口。显式 ROM 配合 `rombar=0` 时遵循 QEMU 通过固件配置提供 Option ROM 的既有语义，并非禁止加载 ROM 文件。

两份参考文件均是 128 KiB 固件转储：

| 文件 | 内含 legacy Option ROM 版本 |
| --- | --- |
| `asmedia_asm1062_fw19092.rom` | v1.70 |
| `asmedia_asm1062_210906_00_76_01_A22.ROM` | v1.80 |

两者的 legacy x86 Option ROM 位于 offset `0x2000`，长度 `0xc000`（48 KiB），即 `0x2000..0xdfff`。完整的 128 KiB 转储不应直接当作标准 PCI Option ROM 使用。若自行试验，可在仓库外提取该段后指定 `romfile=/path/to/extracted.rom`。

两段 Option ROM 的 PCIR identity 是 `1b21:0625`、RAID class，与本设备的 `1b21:1062` SATA AHCI identity 不同；未发现 UEFI image，且固件可能访问尚未实现的 ASMedia BAR0、flash 或 RAID 寄存器，因此不保证其执行或启动兼容性。QEMU 不默认加载或分发这两份专有固件。
