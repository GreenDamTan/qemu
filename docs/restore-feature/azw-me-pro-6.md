# AZW ME Pro-6 机型使用指南

`-machine azw-me-pro-6` 提供 AZW ME Pro-6 的软件可见平台接口，以明确版本的
`pc-q35-11.0` 为基础，固定装配 PCIe 桥、三个双端口 ASM1062、复位按键 MMIO、
IT8613E、RTC 配置接口以及只读 USB 设备。

本文描述机型的**使用契约和操作示例**。主要支持使用第 3.2 节配对 OVMF 的
UEFI/pflash 启动；更换其他固件可能改变客户机 PCI 总线编号。

## 1. 范围与默认行为

- 八个 PCIe Root Port 始终存在，不会因为没有下游磁盘或网卡而省略。
- 三个 ASM1062 始终存在，共六个 SATA 端口；没有输入磁盘时端口为空。
- NVMe 自动池有三个槽位；没有对应磁盘输入时，不创建空的 NVMe 控制器。
- 默认不创建网卡，包括 Q35 的隐式网卡。两个网卡位置只保留桥，需要用户显式加卡。
- 默认不创建隐式光驱。显式 `ide-cd` 可占用一个 SATA 端口。
- 继承的 ICH9 SATA/AHCI 和旧 EHCI/UHCI USB 关闭；内置 ASM1062 和 xHCI 不关闭。
- 默认有 `xhci` 和 `fnoscheck`，后者是 `fnoscheck-disk` 只读 USB 存储设备。
  机型只呈现介质，不主动执行其中的内容；本文也不要求运行介质中的程序。
- 按钮默认未按下，来电启动配置默认关闭。原始电平与寄存器值见后文，不能一概写零。
- 本机型不是完整真实芯片仿真，不包括实机全部显卡、HECI、音频或网卡型号，
  也不承诺外部检测程序一定通过。
- RTC 日期闹钟与来电启动提供配置和读回接口，不承诺日期匹配唤醒、真实断电恢复，
  或在 QEMU 进程退出后重新启动进程。

推荐明确写出以下 machine 参数，省略后两项时机型默认也是关闭状态。

```sh
-machine azw-me-pro-6,sata=off,usb=off
```

这里的 `sata=off`、`usb=off` 指**关闭继承平台的旧控制器**，不是关闭本机型的
SATA/USB 功能。显式设置 `sata=on` 或 `usb=on` 会因固定拓扑冲突被拒绝，
不能用它们调整三个 ASM1062 或内置 xHCI 的数量。

不要再手动添加同名的八桥、`asm0`～`asm2`、`xhci`、`fnoscheck`、
`PORT_RESET_PRESS_SECONDS` 或 `it8613e`。它们已经由 machine 创建。
`-nodefaults` 也不等于删除这些固定平台设备。

## 2. 固定拓扑、盘位与地址

### 2.1 八桥映射

根总线为 `pcie.0`。表中的 QEMU bus ID 可直接用于 `-device ... ,bus=...` 和
QMP/HMP `device_add`。BDF 的域号 `0000` 在表中省略。

| Root Port BDF | VID:DID | QEMU 桥 ID／下游 bus ID | 预期 secondary bus | 预期下游位置 | 用途 |
| --- | --- | --- | --- | --- | --- |
| `00:1c.0` | `8086:54b8` | `pcie.00.1c.0` | `01` | `01:00.0` | NVMe 盘位 101 |
| `00:1c.2` | `8086:54ba` | `pcie.00.1c.2` | `02` | `02:00.0` | 网卡位置，默认空 |
| `00:1c.3` | `8086:54bb` | `pcie.00.1c.3` | `03` | `03:00.0` | 网卡位置，默认空 |
| `00:1c.6` | `8086:54be` | `pcie.00.1c.6` | `04` | `04:00.0` | NVMe 盘位 102 |
| `00:1d.0` | `8086:54b0` | `pcie.00.1d.0` | `05` | `05:00.0` | NVMe 盘位 103 |
| `00:1d.1` | `8086:54b1` | `pcie.00.1d.1` | `06` | `06:00.0` | `asm0`，ASM1062 |
| `00:1d.2` | `8086:54b2` | `pcie.00.1d.2` | `07` | `07:00.0` | `asm1`，ASM1062 |
| `00:1d.3` | `8086:54b3` | `pcie.00.1d.3` | `08` | `08:00.0` | `asm2`，ASM1062 |

这些 secondary bus 是固定拓扑在本文配对 OVMF 下的**预期固件枚举编号**，不是
QEMU bus ID 强制写死的客户机总线号。其他固件、更改后的拓扑及客户机重分配都可能
改变实际 BDF；客户机当前的枚举结果可通过 `lspci` 查看。
固定桥为空槽预留 MMIO 资源以供热插拔使用，但 monitor 添加成功本身不证明
客户机已分配资源并能对设备进行 I/O。

八桥和三个默认 ASM1062 是平台基础设施，不是热拔插对象，`device_del` 删除它们会被拒绝。
需要拔出的是下游磁盘前端，例如后文的 `hot_nvme`、`hot_sata`，不是 `pcie.00.1c.0` 或 `asm0`。

### 2.2 自动池与缺省磁盘 serial

| 类型／功能盘位 | QEMU 连接位置 | 缺省 serial |
| --- | --- | --- |
| NVMe 101 | `bus=pcie.00.1c.0,addr=0.0` | `00:1c.0-01:00.0` |
| NVMe 102 | `bus=pcie.00.1c.6,addr=0.0` | `00:1c.6-04:00.0` |
| NVMe 103 | `bus=pcie.00.1d.0,addr=0.0` | `00:1d.0-05:00.0` |
| SATA 1 | `bus=asm0.0,unit=0` | `00:1d.1-06:00.0-P0` |
| SATA 2 | `bus=asm0.1,unit=0` | `00:1d.1-06:00.0-P1` |
| SATA 3 | `bus=asm1.0,unit=0` | `00:1d.2-07:00.0-P0` |
| SATA 4 | `bus=asm1.1,unit=0` | `00:1d.2-07:00.0-P1` |
| SATA 5 | `bus=asm2.0,unit=0` | `00:1d.3-08:00.0-P0` |
| SATA 6 | `bus=asm2.1,unit=0` | `00:1d.3-08:00.0-P1` |

- NVMe 自动顺序为 `1c.0 → 1c.6 → 1d.0`。系统盘同样占一个槽位，不自动排除。
- SATA 自动顺序为 `asm0.0 → asm0.1 → asm1.0 → asm1.1 → asm2.0 → asm2.1`。
- SATA bus 后缀及 serial 中的 `P0/P1` 从 **0** 编号；面向用户的每控制器端口号
  1/2 分别对应它们。第二端口要写 `bus=asm0.1`，不是 `bus=asm0.0,unit=1`。
- 序列号包含父桥 BDF、预期端点 BDF，以及 SATA 端口后缀；上述缺省值都不超过
  ATA/NVMe 的 20 字节范围。它是固定拓扑标签，不是每次从固件配置空间读取的实时 BDF。
- 功能盘位由桥及端口关系区分，serial 便于人工或自动化辨认，不应用 serial 替代拓扑判断。
- 显式 `-device`、QMP/HMP `device_add` 只在参数中**没有 `serial` 字段**时补缺省值。
  显式值不会被替换，空字符串也视为已提供；设备本身仍可能拒绝不符合其要求的值。

### 2.3 其他固定接口

| 设备／接口 | 实例 ID 或 QOM 路径 | 地址／默认配置 |
| --- | --- | --- |
| 复位按键 MMIO | `PORT_RESET_PRESS_SECONDS` | `0xFD6D0960`，2 字节，默认 `0x0102` |
| IT8613E | `/machine/peripheral/it8613e` | SIO `0x2e/0x2f`；HWM base `0xA30` |
| 风扇 index/data | 同上 | `0xA35/0xA36` |
| 来电启动配置 | IT8613E LDN `0x04`，寄存器 `0xf4` | 默认／关闭 `0x80`，开启 `0xa0` |
| xHCI | `xhci`，USB bus `xhci.0` | 根总线 `00:14.0`，模型 `qemu-xhci` |
| 只读 USB 介质 | `fnoscheck` | `fnoscheck-disk`，接在 `xhci.0` |
| CMOS RTC | `/machine` 的 `rtc-time`、`rtc-alarm` | 标准 CMOS index/data `0x70/0x71` |
| RTC PM1 状态／使能 | PM1 事件接口别名 | `0x1800/0x1802`，各 16 位，RTC 位为 bit10 |

按键所在 `0xFD6D0000..0xFD6D0fff` 页面作固定资源预留，真正可访问的 GPIO
寄存器仍只有 `0xFD6D0960..0xFD6D0961`，不能把整页当作 GPIO 寄存器阵列。
固定 I/O 窗口 `0xA30..0xA37`、`0x1800..0x1803` 不应被分给 PCI I/O BAR。

## 3. UEFI 完整启动示例

### 3.1 前提与隔离工作目录

以下宿主命令使用 Bash。先将 `/absolute/path/to/...` 替换为真实绝对路径。
`QEMU` 必须指向包含本机型及其依赖设备的构建；本文不代替构建过程，也不假定
系统安装的 QEMU 已有 `azw-me-pro-6`。

```bash
REPO=/absolute/path/to/qemu-source
QEMU=/absolute/path/to/build/qemu-system-x86_64
QEMU_IMG=/absolute/path/to/build/qemu-img
BASE_OS=/absolute/path/to/disposable-uefi-linux-base.qcow2
WORK=$(mktemp -d /tmp/azw-me-pro-6.XXXXXX)

"$QEMU" -machine help
printf '本次工作目录：%s\n' "$WORK"
```

`BASE_OS` 应为已经安装系统、能够用 UEFI 启动的 **qcow2** 测试镜像，且不可被其他
进程同时写入。下面创建写时复制层，不直接写入基底；不要把生产磁盘或宿主块设备
作为示例输入。空白的 `qemu-img create` 镜像不包含操作系统，不会自动变成可启动盘。

### 3.2 从仓库解包配对 OVMF

使用这对固件，用户自行解包到工作目录，不直接改仓库文件。

```bash
bzip2 -dc "$REPO/pc-bios/edk2-x86_64-code.fd.bz2" > "$WORK/OVMF_CODE.fd"
bzip2 -dc "$REPO/pc-bios/edk2-i386-vars.fd.bz2" > "$WORK/OVMF_VARS.fd"

"$QEMU_IMG" create -f qcow2 -F qcow2 -b "$BASE_OS" "$WORK/os-overlay.qcow2"
"$QEMU_IMG" create -f qcow2 "$WORK/data0.qcow2" 4G
"$QEMU_IMG" create -f qcow2 "$WORK/data1.qcow2" 4G
"$QEMU_IMG" create -f qcow2 "$WORK/hot-nvme.qcow2" 4G
"$QEMU_IMG" create -f qcow2 "$WORK/hot-sata.qcow2" 4G
```

这里 x86_64 code 与名称带 i386 的 vars 是指定配对，不要仅按文件名猜测并换成
其他版本、尺寸或 Secure Boot 变体。CODE 只读，VARS 使用本次 VM 专属的可写副本；
不同虚拟机不要同时使用同一 VARS 文件。重建 VARS 会丢失该副本中的 UEFI 启动项等设置。

### 3.3 一个 NVMe 系统盘、两个 SATA 数据盘、双监视器

在同一个 Bash 中设置公共参数，随后给出完整的进程启动命令。

```bash
COMMON=(
  -machine azw-me-pro-6,sata=off,usb=off
  -accel tcg
  -cpu max
  -smp 4
  -m 4096
  -drive "if=pflash,unit=0,format=raw,readonly=on,file=$WORK/OVMF_CODE.fd"
  -drive "if=pflash,unit=1,format=raw,file=$WORK/OVMF_VARS.fd"
  -rtc base=utc
  -display none
  -serial none
  -monitor "unix:$WORK/hmp.sock,server=on,wait=off"
  -qmp "unix:$WORK/qmp.sock,server=on,wait=off"
)

"$QEMU" "${COMMON[@]}" \
  -drive "if=none,id=os_backend,file=$WORK/os-overlay.qcow2,format=qcow2" \
  -device nvme,id=nvme_boot,drive=os_backend,bootindex=1 \
  -drive "if=none,id=data0_backend,file=$WORK/data0.qcow2,format=qcow2" \
  -device ide-hd,id=data_disk0,drive=data0_backend \
  -drive "if=none,id=data1_backend,file=$WORK/data1.qcow2,format=qcow2" \
  -device ide-hd,id=data_disk1,drive=data1_backend
```

这条命令的设备分配契约如下。

- `nvme_boot` 自动使用盘位 101，缺省 serial 为 `00:1c.0-01:00.0`。
- `data_disk0`、`data_disk1` 自动使用 SATA 1/2，serial 分别以 `P0/P1` 结尾。
- NVMe 102/103 和 SATA 3～6 留空，适合后面的热插拔示例。
- 没有网卡，也没有为 USB 介质添加后端；USB 设备由 machine 自带。
- TCG 不要求宿主提供 KVM。具备 KVM 条件时可把公共参数里的 `-accel tcg -cpu max`
  **替换**为 `-accel kvm -cpu host`，不要同时保留两组。
- `-display none -serial none` 是无界面配置，不会给用户提供客户机登录终端。
  需要安装系统或交互时，可将 `-display none` 换为当前构建支持的图形显示后端；
  或使用 `-serial stdio`，并在客户机配置串口控制台。HMP/QMP 仍使用独立 socket。
- 可用同一组公共参数运行 `"$QEMU" "${COMMON[@]}"` 观察无盘平台；没有系统盘时
  进入 UEFI 界面或启动失败属于无启动介质的情况，不代表 machine 缺桥。

这些例子是**不同启动方案**。使用同一 `WORK` 前应正常结束上一台 VM，不要同时
复用 socket、VARS 或可写镜像。保留工作目录可保留本次镜像与 UEFI 设置。

### 3.4 连接 HMP 与 QMP

在另一终端设置第 3.1 节打印的真实工作目录，再连接。

```bash
WORK=/tmp/azw-me-pro-6.替换为实际后缀
socat -,rawer,echo=0 "UNIX-CONNECT:$WORK/hmp.sock"
```

另开一个终端连接 QMP。

```bash
WORK=/tmp/azw-me-pro-6.替换为实际后缀
socat - "UNIX-CONNECT:$WORK/qmp.sock"
```

QMP 每次新连接都先接收欢迎消息，再发送能力协商并等成功响应。

```json
{"execute":"qmp_capabilities","id":"capabilities"}
```

后文所有 QMP 命令都假定当前连接已完成握手。JSON 中的文件路径不会展开 `$WORK`
或 `~`，必须替换为运行 QEMU 的宿主实际可读的绝对路径。QMP 不应暴露到不可信网络；
其监视器权限足以修改虚拟机及所用的宿主文件。

## 4. SMBIOS 与可选网卡

### 4.1 SMBIOS Type 1 默认值

| 字段 | 默认值 |
| --- | --- |
| manufacturer | `AZW` |
| product | `ME Pro-6` |
| version | `V1.0` |
| serial | `TESTcf30dd1180414a188f6d3dd4d73a7540` |
| sku | `G01` |
| family | `Default string` |
| uuid | `cf30dd11-8041-4a18-8f6d-3dd4d73a7540` |

只覆盖指定字段，未指定字段保留机型默认值。例如采用第 3 节的变量与公共参数，
以下是另一条完整启动命令，仅更改 Type 1 serial。

```bash
"$QEMU" "${COMMON[@]}" \
  -smbios type=1,serial=LAB-MEP6-0001 \
  -drive "if=none,id=os_backend,file=$WORK/os-overlay.qcow2,format=qcow2" \
  -device nvme,id=nvme_boot,drive=os_backend,bootindex=1
```

需要时可追加 `-uuid 11111111-2222-4333-8444-555555555555` 覆盖 UUID；
也可在标准 `-smbios type=1,...` 参数中设置相应字段，避免同时给出相互冲突的 UUID。
保留目标身份时，产品名应写成 `ME Pro-6`，不要改成 `ME Pro 6`；空格与连字符
不是同一字符。依赖目标身份识别的客户机还应保留 `AZW`、`V1.0` 和以 W/G/B
开头的 SKU。磁盘 serial 与 SMBIOS serial 是两个独立接口。

客户机 Linux 中查看实际固件提供的表，而不只是检查 monitor 属性。

```sh
sudo dmidecode --type 1
```

### 4.2 显式网卡示例

采用现有 `e1000e` 模型，放入第一个空网卡位置；这不是 RTL8126 或 I226-V 仿真。
以下命令仍使用第 3 节的变量与公共参数。**`-netdev user` 依赖构建时启用 libslirp**，
不是所有 QEMU 构建都提供该后端。可用 `"$QEMU" -netdev help` 查看当前可用的网络后端；
未启用 libslirp 时，应换用启用该依赖的构建，或将 `user` 换成当前构建支持且已经
正确配置的其他后端（如 Linux 上的 TAP）。

```bash
"$QEMU" "${COMMON[@]}" \
  -drive "if=none,id=os_backend,file=$WORK/os-overlay.qcow2,format=qcow2" \
  -device nvme,id=nvme_boot,drive=os_backend,bootindex=1 \
  -netdev user,id=net0 \
  -device e1000e,id=nic0,netdev=net0,bus=pcie.00.1c.2,addr=0.0,mac=52:54:00:06:00:01
```

第二块卡可用另一组 `-netdev ... ,id=net1`，
并把 `e1000e` 接到 `bus=pcie.00.1c.3,addr=0.0`，使用不同的设备 ID 和 MAC。
需要固定位置时优先写 `-netdev` 加 `-device`，不要依赖隐式网卡的自动放置。

## 5. 启动磁盘：自动分配与显式连接

### 5.1 共同规则

1. 机型补全针对 `nvme` 控制器以及 `ide-hd`、`ide-cd` 前端。
   `nvme-ns` 不作为独立 PCIe 控制器再分配；其他存储模型不套用此自动策略。
2. `-blockdev` 或 `-drive if=none` 只创建后端，不推断它是 SATA 还是 NVMe，
   不会单独触发自动挂盘。前端类型由 `-device` 或 `device_add` 明确指定。
3. 未指定 `bus` 时，按第 2.2 节从实际空闲槽位选位置。已挂其他设备的槽位、
   仍在等待拔出完成的槽位均不算空闲；失败的添加不永久占号。
4. 显式 `bus` 不会被替换。`addr`、`unit` 和 `serial` 保留用户值，由平台与标准设备规则校验。
   位置不存在、类型不匹配、位置已占用或参数非法均报错，**不回退到另一个槽位**。
5. 只写 `addr` 而不写 `bus`，仍先按自动池选择 bus，再校验该 addr。
   要选定某个 NVMe 盘位，应显式给出 `bus`，通常使用 `addr=0.0`。固定八桥下的
   NVMe 只能使用 PCI slot 0，非零 slot（例如 `addr=1.0`）直接报错；function 0～7
   仍受 PCI 多功能设备规则与地址占用检查约束，不能把其他 function 当作新增自动槽位。
6. 自动池满时直接拒绝，不落到根总线、网卡位置或新增控制器。
7. 启动时先接入 legacy drive，再处理全部 keyval `-device`，最后处理全部 JSON
   `-device`；各组内部保持声明顺序。两种 `-device` 格式混用时，不按命令行交错顺序创建。
   热插拔按命令顺序接入。后处理的显式位置不会抢占先前自动分配的磁盘。
8. 需要固定位置的前端应在同一参数格式内优先声明，并避免 legacy drive 与它占同一个端口。
   JSON 前端需要的位置也不能被先处理的 keyval 前端占用。`bootindex` 控制启动优先级，
   不改变磁盘分配顺序，也不绕过三 NVMe／六 SATA 的容量。

短 bus ID 推荐直接使用表中名称，例如 `bus=asm0.1`。标准 qdev 设备树路径也可指定，
例如 `bus=pcie.00.1d.1/asm0/asm0.1` 指向同一个 SATA 子 bus；可用 HMP `info qtree`
核对设备与 bus 层次。`bus=` 不接受 `/machine/peripheral/...` 形式的 QOM 路径。
`qom-get` 查询 `parent_bus` 返回的也是 QOM 路径，不可直接照抄为 `bus=` 参数。

### 5.2 legacy `-drive`：六 SATA 端口

`-drive file=...,format=qcow2` 使用机型默认磁盘接口，接入六 SATA 端口。
需要确定映射时使用 `if=ide,index=0..5`；虽然参数名是 `ide`，实际连接的是
ASM1062 的 SATA/AHCI 端口，客户机仍使用 AHCI。

| legacy index | SATA bus | 自动创建的前端 ID |
| --- | --- | --- |
| `0` | `asm0.0` | `sata0` |
| `1` | `asm0.1` | `sata1` |
| `2` | `asm1.0` | `sata2` |
| `3` | `asm1.1` | `sata3` |
| `4` | `asm2.0` | `sata4` |
| `5` | `asm2.1` | `sata5` |

例如把系统盘放在 SATA 1，数据盘放在 SATA 4，其他端口留空。

```bash
"$QEMU" "${COMMON[@]}" \
  -drive "if=ide,index=0,id=legacy_os,file=$WORK/os-overlay.qcow2,format=qcow2" \
  -drive "if=ide,index=3,id=legacy_data,file=$WORK/data0.qcow2,format=qcow2"
```

这里 `legacy_os`、`legacy_data` 是后端 ID，不是 `device_del` 使用的前端 ID；
后者分别是 `sata0`、`sata3`。省略 index 时由标准 legacy drive 规则分配可用索引，
不额外扫描后声明的 CLI 前端。超过 `index=5` 会被拒绝。

**本版本不再支持在 `-drive` 中传入 `serial`、`bootindex` 这类前端选项。**
legacy 自动创建的磁盘使用机型缺省串号；需要自定义串号或启动优先级时，
改用下一节的 `if=none` 加 `-device`，或 `-blockdev` 加 `-device`。
不能把前端参数混进后端参数字符串。

### 5.3 `if=none` 加 `-device`：显式位置、自定义串号及 bootindex

下面系统盘指定到 NVMe 102，数据盘指定到 SATA 4。

```bash
"$QEMU" "${COMMON[@]}" \
  -drive "if=none,id=os_backend,file=$WORK/os-overlay.qcow2,format=qcow2" \
  -device nvme,id=nvme_boot,drive=os_backend,bus=pcie.00.1c.6,addr=0.0,serial=LAB-NVME-102,bootindex=1 \
  -drive "if=none,id=data_backend,file=$WORK/data0.qcow2,format=qcow2" \
  -device ide-hd,id=data_disk,drive=data_backend,bus=asm1.1,unit=0,serial=LAB-SATA-004,bootindex=2
```

去掉前端的 `serial`，就按选定位置生成缺省串号；去掉前端的 `bus`，就按自动池选位置。
`serial` 属于前端，`id=data_backend`、`id=data_disk` 分别标识后端与前端。
现代 `-device` 的前端 ID 完全由用户选择，不会自动改成 `sata0..5`。

显式选择的三个 NVMe 桥是自动池，但缺省串号补全还能识别固定八桥中的显式连接。
例如在原本预留网卡的 `pcie.00.1c.2` 后显式挂 `nvme` 时，会根据该固定桥生成
`00:1c.2-02:00.0`；这占用了网卡位置，**不会**扩展三槽自动池。

### 5.4 `-blockdev` 加 `-device`：区分文件节点与镜像节点

以下示例改用显式 block node，系统盘自动分配，数据盘固定到 SATA 6。

```bash
"$QEMU" "${COMMON[@]}" \
  -blockdev "driver=file,node-name=os_file,filename=$WORK/os-overlay.qcow2" \
  -blockdev driver=qcow2,node-name=os_node,file=os_file \
  -device nvme,id=nvme_boot,drive=os_node,bootindex=1 \
  -blockdev "driver=file,node-name=data_file,filename=$WORK/data0.qcow2" \
  -blockdev driver=qcow2,node-name=data_node,file=data_file \
  -device ide-hd,id=data_disk,drive=data_node,bus=asm2.1,unit=0,serial=LAB-SATA-006
```

`drive=` 指向镜像格式节点，不应直接把 qcow2 容器当 raw 使用。更换为 raw 镜像时，
相应的格式节点应写 `driver=raw`。所有节点名和设备 ID 都必须避免冲突。

JSON 形式的 CLI `-device` 使用同样策略。例如可把上例的数据盘前端行替换为以下参数。

```sh
-device '{"driver":"ide-hd","id":"data_disk","drive":"data_node","bus":"asm2.1","unit":0,"serial":"LAB-SATA-006"}'
```

CLI keyval 示例和 JSON 示例是二选一，不可把同一前端创建两次。

### 5.5 池外连接与顺序冲突

显式接到扩展拓扑、根总线或不属于固定六 SATA 端口的控制器时，不自动迁回池内。
若机型无法为该位置确定预期 BDF，必须显式提供 serial；新控制器或扩展桥也要由用户创建。

例如在启动时额外增加一个测试用 Root Port，并在其后挂一块 NVMe；这不是标准三槽布局。

```bash
"$QEMU" "${COMMON[@]}" \
  -device pcie-root-port,id=extra_rp,bus=pcie.0,addr=0x10,chassis=20,slot=20 \
  -drive "if=none,id=extra_backend,file=$WORK/data0.qcow2,format=qcow2" \
  -device nvme,id=extra_nvme,drive=extra_backend,bus=extra_rp,addr=0.0,serial=LAB-EXTRA-001
```

添加扩展桥会改变固件的枚举条件，不能继续把表中的预期 secondary bus 当作实际保证。

以下为容易误解的声明顺序，假定两个后端都已存在。

```sh
-device ide-hd,id=first,drive=backend_a \
-device ide-hd,id=second,drive=backend_b,bus=asm0.0,unit=0
```

若起初 SATA 池为空，第一行已经占 `asm0.0`，第二行失败，不会把第一块挪到 `asm0.1`。
另外，即使把 legacy `-drive if=ide,index=0,...` 写在上述第一行后面，legacy 前端
仍在 CLI 前端之前创建并占据 `asm0.0`。

## 6. 运行时热插拔

### 6.1 操作条件与生命周期

- 下面以 `nvme` 和 `ide-hd` 为例；八桥和三个 ASM1062 已由 machine 创建，
  不要重新创建它们。NVMe 控制器本身则由 `device_add nvme` 创建。
- 镜像必须事先存在，`blockdev-add` 不替用户创建 qcow2 文件。
- 第 3.3 节启动后有两个 NVMe 和四个 SATA 空位；其他启动方案需先确认目标位置空闲。
- QMP 每条命令都应等待成功响应，再发依赖它的下一条；用请求 `id` 区分响应与异步事件。
- 客户机要有 NVMe、AHCI 及相应 PCIe 热插拔支持。设备添加成功后，仍需在客户机确认
  枚举、驱动绑定和实际磁盘 I/O；设备名 `/dev/sdX`、`/dev/nvmeXnY` 不保证固定。
- 拔盘前在客户机停止使用该盘、卸载文件系统、完成缓存写回，并按需要从 RAID、LVM
  或存储池移除。不要拿运行中的系统盘演示拔出。
- `device_del` 成功响应不等于可立即复用位置。必须收到匹配的 `DEVICE_DELETED`；
  pending unplug 仍占位，QEMU 端删除完成后自动池才会重新使用该位置。
- `DEVICE_DELETED` 不保证客户机已完成驱动清理。复用同一位置前，还应确认客户机中
  对应旧块设备已经消失；SATA 的清理可能晚于该事件，并出现旧设备的命令收尾错误。
  应按设备实际状态等待，不用固定延时替代完成判断。
- NVMe 经 PCIe 插槽拔出通常需要客户机配合。ASM1062 的硬盘拔出为 surprise removal，
  不代替客户机卸载和缓存刷新，即使删除很快完成也必须遵守上述数据保护步骤。
- `DEVICE_DELETED` 可能出现在命令响应之前或之后。应从发出请求起持续处理事件，
  而不是只在响应后读取一条消息。HMP 发起的删除也通过已连接的 QMP 报告事件。

### 6.2 QMP：NVMe 自动分配、拔出、释放后端

先在已握手的 QMP 连接上逐条发送以下请求。示例路径要替换为第 3 节的实际文件。

```json
{"execute":"blockdev-add","arguments":{"driver":"file","node-name":"hot_nvme_file","filename":"/path/to/azw-work/hot-nvme.qcow2"},"id":"nvme-file-add"}
{"execute":"blockdev-add","arguments":{"driver":"qcow2","node-name":"hot_nvme_node","file":"hot_nvme_file"},"id":"nvme-node-add"}
{"execute":"device_add","arguments":{"driver":"nvme","id":"hot_nvme","drive":"hot_nvme_node"},"id":"nvme-add"}
{"execute":"qom-get","arguments":{"path":"/machine/peripheral/hot_nvme","property":"serial"},"id":"nvme-serial"}
```

采用第 3.3 节的占用状态且期间未加其他设备时，新控制器使用 NVMe 102，缺省串号为
`00:1c.6-04:00.0`。如果要明确放到 NVMe 103，**将上面的 `device_add` 请求替换为**
以下请求，而不是同时发送两次。

```json
{"execute":"device_add","arguments":{"driver":"nvme","id":"hot_nvme","drive":"hot_nvme_node","bus":"pcie.00.1d.0","addr":"0.0","serial":"HOT-NVME-103"},"id":"nvme-add-explicit"}
```

显式例子也可以省略 `serial` 来取得盘位 103 的缺省值。
在客户机完成停止使用、卸载等准备后请求删除前端。

```json
{"execute":"device_del","arguments":{"id":"hot_nvme"},"id":"nvme-del"}
```

持续读取 QMP，等待 `data.device` 为 `hot_nvme` 的 `DEVICE_DELETED`。
下面仅列出用于匹配的事件字段；完整事件还带时间戳等信息。

```json
{"event":"DEVICE_DELETED","data":{"device":"hot_nvme","path":"/machine/peripheral/hot_nvme"}}
```

确认事件后，按先格式节点、后文件节点的依赖顺序删除后端。

```json
{"execute":"blockdev-del","arguments":{"node-name":"hot_nvme_node"},"id":"nvme-node-del"}
{"execute":"blockdev-del","arguments":{"node-name":"hot_nvme_file"},"id":"nvme-file-del"}
```

重新插入可重复本节流程；槽位由当时的实际占用状态决定。若希望重新使用同一位置，
应再次显式指定 bus；“复用空槽”不等于永远绑定上一次的磁盘 ID。

### 6.3 QMP：SATA 自动分配与指定控制器端口

```json
{"execute":"blockdev-add","arguments":{"driver":"file","node-name":"hot_sata_file","filename":"/path/to/azw-work/hot-sata.qcow2"},"id":"sata-file-add"}
{"execute":"blockdev-add","arguments":{"driver":"qcow2","node-name":"hot_sata_node","file":"hot_sata_file"},"id":"sata-node-add"}
{"execute":"device_add","arguments":{"driver":"ide-hd","id":"hot_sata","drive":"hot_sata_node"},"id":"sata-add"}
{"execute":"qom-get","arguments":{"path":"/machine/peripheral/hot_sata","property":"serial"},"id":"sata-serial"}
```

在第 3.3 节的初始占用状态下，自动使用 SATA 3，即 `asm1.0`，缺省串号
`00:1d.2-07:00.0-P0`。要选第二控制器第二端口，即 SATA 4，把上述 `device_add`
替换为以下请求。

```json
{"execute":"device_add","arguments":{"driver":"ide-hd","id":"hot_sata","drive":"hot_sata_node","bus":"asm1.1","unit":0,"serial":"HOT-SATA-004"},"id":"sata-add-explicit"}
```

客户机停止使用该盘后，删除前端。

```json
{"execute":"device_del","arguments":{"id":"hot_sata"},"id":"sata-del"}
```

等待匹配的删除完成事件。

```json
{"event":"DEVICE_DELETED","data":{"device":"hot_sata","path":"/machine/peripheral/hot_sata"}}
```

随后释放两个命名节点。

```json
{"execute":"blockdev-del","arguments":{"node-name":"hot_sata_node"},"id":"sata-node-del"}
{"execute":"blockdev-del","arguments":{"node-name":"hot_sata_file"},"id":"sata-file-del"}
```

如果只删除前端而暂时保留这两个节点，也可用同一个 `drive=hot_sata_node` 重新添加，
但前端旧 ID 和位置必须已经释放，且客户机中的旧块设备应已消失。添加前端失败时，
已创建的命名 block node 不会自动全部回滚，应按引用关系自行清理。
不要把仍在使用的节点交给 `blockdev-del`。

### 6.4 HMP：`drive_add`、`device_add`、`device_del`

HMP 参数是 keyval，不是 JSON。示例中的文件路径仍是 QEMU 宿主路径。
先保持另一个已握手的 QMP 连接，以便收到删除完成事件。

```text
(qemu) drive_add 0 if=none,id=hmp_backend,file=/path/to/azw-work/hot-sata.qcow2,format=qcow2
(qemu) device_add ide-hd,id=hmp_disk,drive=hmp_backend
(qemu) info qtree
(qemu) info block
```

`drive_add` 的 `0` 是这条 HMP 命令要求的地址占位；在 `if=none` 下不选择 SATA
控制器。真正位置由 `device_add` 决定。要显式选择 SATA 6，将上面的添加前端行替换为
以下命令。

```text
(qemu) device_add ide-hd,id=hmp_disk,drive=hmp_backend,bus=asm2.1,unit=0,serial=HMP-SATA-006
```

NVMe 也使用同一套补全策略。例如改用一个独立且未被占用的后端。

```text
(qemu) drive_add 0 if=none,id=hmp_nvme_backend,file=/path/to/azw-work/hot-nvme.qcow2,format=qcow2
(qemu) device_add nvme,id=hmp_nvme,drive=hmp_nvme_backend
```

需要固定位置时，前端行可改为以下形式。

```text
(qemu) device_add nvme,id=hmp_nvme,drive=hmp_nvme_backend,bus=pcie.00.1d.0,addr=0.0,serial=HMP-NVME-103
```

准备好拔盘后使用前端 ID，而不是 backend ID。

```text
(qemu) device_del hmp_disk
(qemu) device_del hmp_nvme
```

分别等到 QMP 中匹配的 `DEVICE_DELETED`，再检查 HMP `info block`。
这里的 legacy `drive_add if=none` 后端遵循标准自动删除语义，可能已经随前端移除；
只有仍存在且已脱离前端的后端才需要 `drive_del hmp_backend` 或
`drive_del hmp_nvme_backend`。不要在前端仍连接时用 `drive_del` 代替热拔盘，
那可能直接使客户机 I/O 失败。

若后端来自第 6.2/6.3 节的 QMP `blockdev-add`，也可用 HMP
`device_add ... ,drive=hot_sata_node` 添加前端；删除后仍按 QMP 的
`blockdev-del` 两节点顺序清理。命名节点不会因为切换了前端命令通道就自动释放。

### 6.5 满池、错误位置和恢复

下表中的 `err_node` 假定是已准备好、尚未连接的镜像节点，`bad_disk` 未被使用。
每个错误例子独立执行，不要因缺少后端而把另一种错误误认为位置检查结果。

| 条件 | 请求片段 | 契约 |
| --- | --- | --- |
| 三个 NVMe 自动槽均占用 | `device_add nvme,id=bad_disk,drive=err_node` | `azw-me-pro-6 has no free NVMe slot` |
| 六 SATA 端口均占用 | `device_add ide-hd,id=bad_disk,drive=err_node` | `azw-me-pro-6 has no free SATA slot` |
| 指向不存在的 bus | `device_add ide-hd,id=bad_disk,drive=err_node,bus=asm9.0` | 找不到 bus，拒绝，不改接其他口 |
| 指向已占用的 `asm0.0` | `device_add ide-hd,id=bad_disk,drive=err_node,bus=asm0.0,unit=0` | 拒绝，不使用下一个空口 |
| 将 `unit=1` 当第二 SATA 口 | `device_add ide-hd,id=bad_disk,drive=err_node,bus=asm2.0,unit=1` | 非法 unit，拒绝 |
| 固定桥下 NVMe 指定非零 slot | `device_add nvme,id=bad_disk,drive=err_node,bus=pcie.00.1d.0,addr=1.0` | 该 Root Port 只提供 slot 0，直接拒绝，不改为 `addr=0.0` |
| NVMe 显式地址非法 | `device_add nvme,id=bad_disk,drive=err_node,bus=pcie.00.1d.0,addr=20.0` | PCI slot 超出范围，拒绝；keyval 地址按十六进制解释 |
| 扩展桥后缺少 serial | `device_add nvme,id=bad_disk,drive=err_node,bus=extra_rp` | 若 `extra_rp` 存在，要求显式 serial |
| 固定基础设备热拔出 | `device_del asm0` | 拒绝删除固定控制器 |

QMP 形式同样适用，例如不存在的 bus。

```json
{"execute":"device_add","arguments":{"driver":"ide-hd","id":"bad_disk","drive":"err_node","bus":"asm9.0","unit":0},"id":"bad-location"}
```

修正失败参数后可以重试；机型不会因为一次失败就消耗一个永久槽号。
但已成功创建的后端、尚未完成的拔出、重复设备 ID 和已被其他前端使用的后端
仍是独立约束。出现 `DEVICE_UNPLUG_GUEST_ERROR` 或一直没有完成事件时，
先查客户机热插拔驱动和设备使用状态，不要继续删除被引用的后端来强行释放。

## 7. 复位按钮 GPIO/MMIO

### 7.1 固定寄存器语义

已有通用命令的完整说明见 [GPIO 文档](gpio.md)。本机型自动创建的实例等价配置如下；
这是配置说明，**不要在启动命令中重复添加**。

```text
gpio,id=PORT_RESET_PRESS_SECONDS,addr=0xfd6d0960,size=2,gpio-mask=0x2,write-mask=0x700,default=0x102
```

| 位／范围 | 含义与写权限 |
| --- | --- |
| bit1 | 低有效按钮输入；`1` 未按下，`0` 按下；monitor 注入可改，客户机不能改 |
| bits8～10 | 客户机配置位；初始化为 bit8=1、bit9/10=0；monitor 的 GPIO 注入不改它们 |
| 其他位 | 不在这两个写掩码内，保持模型当前值 |

`gpio-mask=0x0002` 与 `write-mask=0x0700` 独立，不要求存在子集关系。
默认完整值 `0x0102` 是仿真初值，不是完整真实芯片上电值的声明。
机型提供的是 `/dev/mem` 可见 MMIO，不是 `/dev/gpiochip*`、GPIO ioctl 或 Linux input
按键设备。按下是否触发重置、长按操作等由客户机软件决定，GPIO 命令本身不会重启 VM。

### 7.2 HMP：读值、按下、释放和定时恢复

```text
(qemu) xp /1hx 0xFD6D0960
(qemu) gpio PORT_RESET_PRESS_SECONDS 0
(qemu) xp /1hx 0xFD6D0960
(qemu) gpio PORT_RESET_PRESS_SECONDS 0x2
(qemu) xp /1hx 0xFD6D0960
(qemu) gpio PORT_RESET_PRESS_SECONDS 0 5
```

在配置位未被客户机改动时，前三次读数依次应为 `0x0102`、`0x0100`、`0x0102`。
最后一条模拟按下 5 个**虚拟秒**，到期只恢复 bit1 到默认未按下状态。
VM 暂停期间不会消耗这段时间。省略 duration 会取消旧计时并持续保持本次输入；
新的定时命令替换旧计时，reset 则恢复整个 `0x0102` 并取消计时。

`value` 是原始位模式，高电平应写 `0x2`，不是 `1`。HMP duration 为正整数秒，
不能写 `5s` 或 `0`；不是“写入按压秒数寄存器”。

### 7.3 QMP

```json
{"execute":"gpio","arguments":{"id":"PORT_RESET_PRESS_SECONDS","value":0,"duration":5},"id":"button-press"}
```

要立即释放并取消旧计时，发送以下请求。

```json
{"execute":"gpio","arguments":{"id":"PORT_RESET_PRESS_SECONDS","value":2},"id":"button-release"}
```

JSON 数值使用十进制整数，不使用 `0x2` 或字符串 `"0x2"`。
通过 QMP 读物理值可调用现有 HMP 命令，不需要新增 GPIO 查询接口。

```json
{"execute":"human-monitor-command","arguments":{"command-line":"xp /1hx 0xFD6D0960"},"id":"button-read"}
```

### 7.4 客户机直接 MMIO

在专用 Linux 测试客户机内，若有 BusyBox `devmem` 且内核允许访问该物理范围，
可用 16 位访问读取和执行配置位 RMW。

```sh
sudo busybox devmem 0xFD6D0960 16
old=$(sudo busybox devmem 0xFD6D0960 16)
new=$(( (old & ~0x0600) | 0x0100 ))
sudo busybox devmem 0xFD6D0960 16 "$new"
sudo busybox devmem 0xFD6D0960 16
```

上述 RMW 清 bit9/10、置 bit8，并保留其他读到的位；客户机写入最终只能影响
`write-mask` 选中的位，不能伪造按钮输入。访问不能跨过 2 字节窗口，不能用
32 位写代替这里的 16 位写。不要在相同地址再放一个 `trim-mmio` 或拆出两个重叠实例。

若 `/dev/mem` 被内核权限、lockdown 或访问策略拒绝，应在合适的隔离客户机中
提供合法访问条件，不把 HMP 可读直接当成客户机访问已经验证。

## 8. IT8613E：直接风扇访问与来电启动

### 8.1 端口、通道和默认计数

实例为 `/machine/peripheral/it8613e`。本机型已经设置 `hwm-addr=0xA30`，
所以使用 `0xA35/0xA36`，不要套用独立设备常见的 `0x295/0x296`。
SIO 配置端口仍是 `0x2e/0x2f`，设备标识为 `0x8613`。

默认是固定 RPM 模式，按**芯片通道号**区分。

| 通道 | 板上含义 | 默认 RPM | tach 低字节寄存器 | tach 高字节寄存器 | 预期 raw |
| --- | --- | --- | --- | --- | --- |
| fan2 | CPU 风扇 | `222` | `0x0e` | `0x19` | `0x0be0` |
| fan3 | 其他通道 | `333` | `0x0f` | `0x1a` | `0x07eb` |
| fan4 | SYS 风扇 | `444` | `0x80` | `0x81` | `0x05f0` |
| fan5 | 其他通道 | `555` | `0x82` | `0x83` | `0x04c0` |

读取时，向 index 端口写寄存器号，再从 data 端口读一个字节；组合高低字节后换算。

```text
raw = (high << 8) | low
RPM = 1350000 / (raw * 2)
```

对这些默认值采用整数除法，分别反算为 222、333、444、555 RPM。
一般非零 RPM 的编码是 `clamp(1350000 / (RPM * 2), 1, 0xfffe)`，整数向下取整；
零转速编码为 `0xffff`。其他 RPM 可能有量化或限幅误差，不承诺任意设置精确往返。
风扇寄存器读数不是电机、温度或完整风扇闭环仿真。

### 8.2 客户机直接 I/O 读取完整例子

以下一次性 Python 命令只在**专用 x86 Linux 客户机内**执行，要求 `/dev/port`
可用及相应 I/O 权限。不要在宿主运行；也不要与 `it87` 等驱动或其他直接 I/O
程序并发操作同一 index/data 端口。示例保存 bank 选择并临时选 bank 0，结束后恢复。

```sh
sudo python3 - <<'PY'
import os

fd = os.open('/dev/port', os.O_RDWR)
def outb(port, value):
    if os.pwrite(fd, bytes([value]), port) != 1:
        raise OSError('short port write')
def inb(port):
    value = os.pread(fd, 1, port)
    if len(value) != 1:
        raise OSError('short port read')
    return value[0]
def reg_read(reg):
    outb(0xA35, reg)
    return inb(0xA36)
def reg_write(reg, value):
    outb(0xA35, reg)
    outb(0xA36, value)

old_index = None
old_bank = None
try:
    old_index = inb(0xA35)
    old_bank = reg_read(0x06)
    reg_write(0x06, old_bank & 0x1f)
    for name, lo_reg, hi_reg in (
        ('fan2/CPU', 0x0e, 0x19),
        ('fan3',     0x0f, 0x1a),
        ('fan4/SYS', 0x80, 0x81),
        ('fan5',     0x82, 0x83),
    ):
        high = reg_read(hi_reg)
        low = reg_read(lo_reg)
        raw = (high << 8) | low
        rpm = 0 if raw in (0, 0xffff) else 1350000 // (raw * 2)
        print(f'{name}: raw=0x{raw:04x}, rpm={rpm}')
finally:
    try:
        if old_bank is not None:
            reg_write(0x06, old_bank)
        if old_index is not None:
            outb(0xA35, old_index)
    finally:
        os.close(fd)
PY
```

是否符合本机型风扇接口，应以这些直接寄存器和换算结果判断，不能只用 `sensors`、
`hwmon/sysfs` 或 QOM 属性读数代替。QOM 显示的是配置转速，不等于已经读到了正确的 I/O 地址。

### 8.3 HMP/QMP 运行时修改转速

```text
(qemu) qom-get /machine/peripheral/it8613e fan2-rpm
(qemu) qom-get /machine/peripheral/it8613e fan4-rpm
(qemu) qom-set /machine/peripheral/it8613e fan2-rpm 1200
(qemu) qom-set /machine/peripheral/it8613e fan4-rpm 900
```

QMP 等价操作如下。

```json
{"execute":"qom-get","arguments":{"path":"/machine/peripheral/it8613e","property":"fan2-rpm"},"id":"cpu-fan-read"}
{"execute":"qom-set","arguments":{"path":"/machine/peripheral/it8613e","property":"fan2-rpm","value":1200},"id":"cpu-fan-set"}
{"execute":"qom-set","arguments":{"path":"/machine/peripheral/it8613e","property":"fan4-rpm","value":900},"id":"sys-fan-set"}
```

`fan2`～`fan5` 是对应 `fan*-rpm` 的短别名。修改后需要再次从客户机读取 tach，
检查目标软件是否使用预期通道。恢复本机型四路初始配置可执行以下 HMP 命令。

```text
(qemu) qom-set /machine/peripheral/it8613e fan2-rpm 222
(qemu) qom-set /machine/peripheral/it8613e fan3-rpm 333
(qemu) qom-set /machine/peripheral/it8613e fan4-rpm 444
(qemu) qom-set /machine/peripheral/it8613e fan5-rpm 555
```

固定 RPM 模式不等于锁定所有客户机寄存器写入；需要锁定测试值时可使用现有
`fan2-lock` 等属性。不能在 realized 后用 `qom-set` 改 `sio-addr`、`hwm-addr`
或 `pme-f4-default` 来重新布置本机型端口。

### 8.4 来电启动配置：LDN4 的 `0xf4`

- 向 `0x2e` 依次写 `0x87,0x01,0x55,0x55` 进入 SIO 配置模式。
- 通过 index `0x07`、data `0x04` 选择 LDN4。
- 选择 index `0xf4`，从 `0x2f` 读回或写入配置：关闭 `0x80`，开启 `0xa0`。
- 退出配置模式为 index `0x02`、data `0x02`。
- 本机型 `pme-f4-default=0x80`，设备 reset 后恢复关闭；这是配置状态，不是
  `/dev/mem` 的另一处开关，不应写到按钮 MMIO。

下面的客户机一次性命令默认只读。把 `REQUEST = None` 改为 `0xa0` 可开启，
改为 `0x80` 可关闭。与风扇例子一样，要求隔离客户机及独占端口访问。

```sh
sudo python3 - <<'PY'
import os

REQUEST = None  # 只读；开启用 0xa0，关闭用 0x80
fd = os.open('/dev/port', os.O_RDWR)
def outb(port, value):
    if os.pwrite(fd, bytes([value]), port) != 1:
        raise OSError('short port write')
def inb(port):
    value = os.pread(fd, 1, port)
    if len(value) != 1:
        raise OSError('short port read')
    return value[0]
def sio_read(reg):
    outb(0x2e, reg)
    return inb(0x2f)
def sio_write(reg, value):
    outb(0x2e, reg)
    outb(0x2f, value)

old_ldn = None
try:
    for value in (0x87, 0x01, 0x55, 0x55):
        outb(0x2e, value)
    old_ldn = sio_read(0x07)
    sio_write(0x07, 0x04)
    print(f'before: 0x{sio_read(0xf4):02x}')
    if REQUEST is not None:
        sio_write(0xf4, REQUEST)
    print(f'after:  0x{sio_read(0xf4):02x}')
finally:
    try:
        if old_ldn is not None:
            sio_write(0x07, old_ldn)
        sio_write(0x02, 0x02)
    finally:
        os.close(fd)
PY
```

配置值可写可读不表示已经具备真实来电启动能力；不要用物理断电或进程退出后自动重启
作为这个接口的含义。

## 9. RTC 当前时间与日期闹钟

### 9.1 客户机可见配置接口

| 接口 | 位／寄存器 | 含义 |
| --- | --- | --- |
| CMOS `0x70/0x71` | index `0x01` | 闹钟秒 |
| 同上 | index `0x03` | 闹钟分 |
| 同上 | index `0x05` | 闹钟时 |
| 同上 | index `0x0b` bit5 | AIE，CMOS 闹钟中断使能 |
| 同上 | index `0x0b` bit2 | DM：1 为二进制，0 为 BCD |
| 同上 | index `0x0b` bit1 | 1 为 24 小时制，0 为 12 小时制 |
| 同上 | index `0x0d` 低6位 | 闹钟日；0 表示不限定日，高2位保留 |
| I/O `0x1800`，16 位 | PM1_STS bit10 | RTC 状态，写 1 清除该位 |
| I/O `0x1802`，16 位 | PM1_EN bit10 | RTC PM1 事件使能 |

`0x1800..0x1803` 是已有 ICH9 PM1 事件寄存器的别名，不是第二套独立状态；
原 ICH9 接口与这里读写的是同一状态。修改 PM1_EN 时应 RMW 保留其他使能位；
PM1_STS 是写一清除，不能把读回的全部已置位状态原样写回而误清其他事件。

例如已有客户机应用在 **BCD、24 小时制** 下配置每月 15 日 06:30:00，可使用
下表中的寄存器值；配置过程中先关闭相关使能，保持编码模式一致，设置完成后再启用。

| 配置项 | 原始写值／操作 |
| --- | --- |
| 秒 index `0x01` | `0x00` |
| 分 index `0x03` | `0x30` |
| 时 index `0x05` | `0x06` |
| 日 index `0x0d` 低6位 | `0x15` |
| CMOS AIE | 对 `0x0b` RMW 置 bit5 |
| PM1 RTC 使能 | 对 `0x1802` 做 16 位 RMW 置 `0x0400` |

二进制模式的同一日期应写日十进制 `15`、分十进制 `30` 等，不是 BCD 原始字节。
不要为了设置闹钟直接整字节覆盖 REG_B，破坏当前时间编码或其他中断配置。
这些配置和查询不承诺按日期匹配唤醒，也不把 CMOS 的 AIE 与 PM1 使能合并成一个开关。

### 9.2 HMP 查询

```text
(qemu) qom-get /machine rtc-time
(qemu) qom-get /machine rtc-alarm
```

这两个都是**只读 QOM 属性**，不是新增 HMP 命令；不能用 `qom-set` 设置它们。
当前时间和闹钟由客户机标准 RTC/CMOS I/O 配置，monitor 查询仿真的 RTC 状态，
不是执行宿主的 `date` 命令。

### 9.3 QMP 查询当前时间：保留 `tm_*` 字段

```json
{"execute":"qom-get","arguments":{"path":"/machine","property":"rtc-time"},"id":"rtc-now"}
```

`rtc-time` 沿用既有 RTC `date` 接口。以下示例表示 2026-09-30 12:34:56；
实际返回值随虚拟 RTC 状态变化。

```json
{"return":{"tm_year":126,"tm_mon":8,"tm_mday":30,"tm_hour":12,"tm_min":34,"tm_sec":56},"id":"rtc-now"}
```

| 字段 | 换算 |
| --- | --- |
| `tm_year` | 公历年 = `tm_year + 1900` |
| `tm_mon` | 公历月 = `tm_mon + 1`，原值范围 0～11 |
| `tm_mday` | 月内日，1～31 |
| `tm_hour` | 时，0～23 |
| `tm_min` | 分 |
| `tm_sec` | 秒 |

不要按 `year/month/day` 这样的自造键解析。本机型没有另行注册一套当前时间格式。
RTC 的起始时基和推进方式仍遵循标准 `-rtc` 配置及客户机对 RTC 的更改，
例如本文使用 `-rtc base=utc`。

### 9.4 QMP 查询闹钟：`rtc-alarm` 字段

```json
{"execute":"qom-get","arguments":{"path":"/machine","property":"rtc-alarm"},"id":"rtc-alarm"}
```

顶层字段如下，名称中的连字符不可换成下划线。

| 字段 | 含义 |
| --- | --- |
| `enabled` | CMOS REG_B 的 AIE 是否置位 |
| `pm1-enabled` | PM1_EN 的 RTC bit10 是否置位，与 AIE 独立 |
| `binary` | 当前 CMOS 编码是否为二进制 |
| `hour-24` | 当前是否为 24 小时制 |
| `day` | 月内日分量，不包含年或月 |
| `hour` | 时分量，具体有效值统一解码为 0～23 |
| `minute` | 分分量 |
| `second` | 秒分量 |

每个时间分量都是对象。

| 分量键 | 含义 |
| --- | --- |
| `raw` | 原始完整寄存器字节，以 JSON 十进制数返回；日的 raw 也包含高状态位 |
| `wildcard` | 是否表示该分量不限定 |
| `valid` | 当前编码是否合法；合法通配也为 `true` |
| `value` | 仅当 `valid=true` 且 `wildcard=false` 时存在，表示解码后的具体数值 |

沿用第 9.1 节 BCD 示例，假定 REG_D 高位为 `0x80` 且两个使能均开启，则响应结构
可为下面这样。`raw=149` 即 `0x95`，低6位 `0x15` 解码为 15 日。

```json
{
  "return": {
    "enabled": true,
    "pm1-enabled": true,
    "binary": false,
    "hour-24": true,
    "day": {"raw": 149, "wildcard": false, "valid": true, "value": 15},
    "hour": {"raw": 6, "wildcard": false, "valid": true, "value": 6},
    "minute": {"raw": 48, "wildcard": false, "valid": true, "value": 30},
    "second": {"raw": 0, "wildcard": false, "valid": true, "value": 0}
  },
  "id": "rtc-alarm"
}
```

编码和边界规则如下。

- 秒／分／时的高两位均为 1 时表示通配，例如 `raw=192`（`0xc0`）。
  此时 `wildcard=true,valid=true`，没有 `value`。
- 日只解释低6位，低6位为 0 表示不限定日。例如日 `raw=128`（`0x80`）是通配，
  不是 0 日或 128 日。
- 具体秒／分应在 0～59，24 小时制的时在 0～23，日在 1～31。
  不合法 BCD 或越界值返回 `valid=false`，不会以 `value=0` 伪装有效值。
- 12 小时制下用 bit7 表示 PM，低7位的具体时必须是 1～12，再转成 24 小时值。
  BCD `0x12` 为午夜 0 时，`0x92` 为中午 12 时，`0x86` 为 18 时；
  转换仍先遵守通配识别规则。
- 日期只包含月内日，不返回或推测年、月，也不根据未提供的月份判断 29/30/31 日。
- 关闭 `enabled` 或 `pm1-enabled` 不会把保留的闹钟配置隐藏掉；应分别检查配置与使能。
- 查询不通过读取会清 IRQ 状态的 REG_C 获取数据，重复查询不清除 IRQ 或 PM1 状态。

客户端应先判断 `wildcard` 和 `valid` 再读取可选的 `value`，不要假设四个分量都有
具体数值，更不要把一次可读回配置解释为实际定时开机已经实现。

## 10. 观察与排查清单

### 10.1 客户机观察

在能登录的 Linux 客户机内，可以使用以下命令检查公开可见信息。

```sh
sudo dmidecode --type 1
lspci -nn
lspci -t
lsblk -o NAME,TYPE,SIZE,MODEL,SERIAL
lsusb
```

检查八桥是否保留、三个 `1b21:1062` 控制器的位置、实际 secondary bus 与串号标签的
对应关系、只读 USB 设备以及用户显式添加的网卡。没有网卡是默认设计，
USB `fnoscheck` 只需枚举观察，不需执行介质内容。查看系统日志、驱动绑定和
临时数据盘实际 I/O，才能进一步判断热插拔是否在客户机内可用。

GPIO 使用第 7 节的 16 位 MMIO 访问，风扇使用第 8 节的直接 I/O；不要用仅 monitor
读值替代客户机访问。RTC 同时核对当前时间、保留的闹钟配置及两个独立使能位。

### 10.2 QEMU 侧观察

```text
(qemu) info pci
(qemu) info qtree
(qemu) info block
(qemu) qom-list /machine/peripheral
(qemu) qom-get /machine rtc-time
(qemu) qom-get /machine rtc-alarm
```

QMP 可查询命名后端，辅助区分前端是否已删除与节点是否仍存在。

```json
{"execute":"query-block","id":"block-backends"}
{"execute":"query-named-block-nodes","arguments":{"flat":true},"id":"block-nodes"}
```

### 10.3 常见问题

| 现象 | 首先核对 |
| --- | --- |
| 找不到 machine 或设备类型 | 当前执行文件是否为包含新机型和依赖设备的构建 |
| 设置 `sata=off` 后仍有 SATA | 这是内置 ASM1062，off 只关闭继承的 ICH9 SATA |
| 设置 `usb=off` 后仍有 USB | 这是固定 xHCI 与只读介质，不是旧 EHCI/UHCI |
| `-netdev user` 报后端不可用 | 当前构建是否启用 libslirp；用 `-netdev help` 查看支持的后端 |
| 加一块 NVMe 却到了第二槽 | 第一槽是否被系统盘或其他设备占用；系统盘不排除 |
| 只有后端没有磁盘 | `if=none`／`-blockdev` 需要配套前端 |
| 自定义串号不生效或 drive 参数报错 | 串号和 bootindex 应放在 `-device`，不是 `-drive` |
| 显式位置报错而旁边有空位 | 显式位置不会回退；核对 bus、addr、unit 及声明顺序 |
| 删除后马上加回失败或客户机仍报旧设备错误 | 是否已收到对应 `DEVICE_DELETED`、客户机旧块设备是否消失，后端与 ID 是否已释放 |
| monitor 加盘成功但客户机不可用 | 客户机驱动、PCIe 热插拔处理、固件资源分配及实际 I/O |
| 客户机 BDF 与串号不同 | 串号使用预期固定拓扑；核对固件配对和是否加了额外桥 |
| GPIO 高电平写成 1 仍像按下 | bit1 的原始高电平为 2，value 不是右移后的逻辑值 |
| GPIO 定时恢复没发生 | 虚拟机是否暂停，计时是否被后续命令取消或替换 |
| 风扇读到错误地址或值 | HWM index/data 是 `0xA35/0xA36`，CPU=fan2，SYS=fan4 |
| RTC 日期差 1900 年或月份差 1 | 按 `tm_year + 1900`、`tm_mon + 1` 换算 |
| 闹钟缺少 `value` | 可能是通配或非法编码，先看 `wildcard`、`valid` |
| 来电／日期配置可读但没有自动开机 | 本接口不承诺真实来电启动、日期匹配唤醒或进程外启动 |
