# 恢复 Intel HAXM 加速器维护记录

本文记录在当前 QEMU 树中恢复 Intel HAXM (`-accel hax`) 的背景、恢复来源、文件职责、构建系统入口、API 适配点、验证命令和后续移植排错方法。

这份文档面向后续维护者。重点不是介绍 HAXM 的使用方法，而是说明如何在较新的 QEMU 代码树中继续维护这份已经被上游删除的加速器代码。

## 目标和边界

本次恢复的目标：

- 让当前 QEMU 重新支持 `-accel hax`。
- 让 `--enable-hax` / `-Dhax=enabled` 能进入配置系统。
- 让 `x86_64-softmmu` 在 Windows MINGW64 环境下成功编译和链接。
- 让 `build/qemu-system-x86_64.exe -accel help` 能列出 `hax`。

本次恢复不保证：

- Intel HAXM 驱动在所有 Windows 版本上可安装或可运行。
- HAXM 能支持当前 QEMU 中新增的所有 CPU、设备、迁移或调试特性。
- HAXM 与 WHPX、Hyper-V、VBS、现代 Windows 虚拟化安全配置之间没有冲突。

HAXM 项目已经停止维护。运行时故障需要先区分是 QEMU 侧移植问题，还是 HAXM 驱动本身、宿主机系统、BIOS/VT-x、Hyper-V/VBS 状态导致的问题。

## 上游删除背景

上游 QEMU 在以下提交中删除了 HAX 加速器：

```text
commit b91b0fc1635544341b9d00d1addc8ddf48e5b389
Author: Philippe Mathieu-Daudé <philmd@linaro.org>
Subject: accel: Remove HAX accelerator
```

该提交删除了 `target/i386/hax/`、`include/sysemu/hax.h`、`accel/stubs/hax-stub.c`，并移除了 Meson、Kconfig、命令行帮助、文档等入口。

恢复时的基本原则：

- 以删除提交之前的 HAXM 代码为基础。
- 不重写 HAXM 后端核心逻辑。
- 只做当前 QEMU API 和目录结构所需的最小适配。
- 优先参考当前树中 WHPX、KVM、NVMM 等加速器的写法。

## 推荐恢复来源

可以从删除提交的父提交中恢复 HAXM 文件。曾使用过的恢复思路如下：

```sh
git show --full-index --binary b91b0fc163 \
  -- accel/stubs/hax-stub.c \
     include/sysemu/hax.h \
     target/i386/hax/hax-accel-ops.c \
     target/i386/hax/hax-accel-ops.h \
     target/i386/hax/hax-all.c \
     target/i386/hax/hax-i386.h \
     target/i386/hax/hax-interface.h \
     target/i386/hax/hax-mem.c \
     target/i386/hax/hax-posix.c \
     target/i386/hax/hax-posix.h \
     target/i386/hax/hax-windows.c \
     target/i386/hax/hax-windows.h \
     target/i386/hax/meson.build |
git apply --3way --reverse
```

注意事项：

- 上面命令会恢复旧路径 `include/sysemu/hax.h`。当前树应移动为 `include/system/hax.h`。
- 恢复后如果出现 staged/unstaged 混合状态，可以用 `git reset` 只清理索引状态，不会丢工作树内容。
- 不要用 `git checkout` 或 `git reset --hard` 清理工作树，避免误删用户已有改动。

## 恢复的 HAXM 文件

当前恢复后的文件清单：

```text
accel/stubs/hax-stub.c
include/system/hax.h
target/i386/hax/hax-accel-ops.c
target/i386/hax/hax-accel-ops.h
target/i386/hax/hax-all.c
target/i386/hax/hax-i386.h
target/i386/hax/hax-interface.h
target/i386/hax/hax-mem.c
target/i386/hax/hax-posix.c
target/i386/hax/hax-posix.h
target/i386/hax/hax-windows.c
target/i386/hax/hax-windows.h
target/i386/hax/meson.build
```

各文件职责：

| 文件 | 作用 |
| --- | --- |
| `include/system/hax.h` | 非 HAX 专用代码可见的 HAX 开关和 `hax_enabled()` 声明。 |
| `accel/stubs/hax-stub.c` | 未启用 HAX 时的 stub，提供 `hax_sync_vcpus()` 和 `hax_allowed`。 |
| `target/i386/hax/hax-all.c` | HAX 通用核心，包括初始化、vCPU 创建、运行循环处理、寄存器同步、MMIO/PIO 处理。 |
| `target/i386/hax/hax-mem.c` | HAX 内存映射、MemoryListener、RAM block notifier。 |
| `target/i386/hax/hax-accel-ops.c` | 当前 QEMU CPU 加速器线程接口注册。 |
| `target/i386/hax/hax-accel-ops.h` | HAX vCPU 线程和同步函数声明。 |
| `target/i386/hax/hax-i386.h` | HAX i386 侧内部结构和平台头汇合。 |
| `target/i386/hax/hax-interface.h` | HAXM 驱动接口结构体和 ioctl 编码。 |
| `target/i386/hax/hax-windows.c` | Windows 平台 HAXM 驱动访问、ioctl、vCPU run、寄存器/MSR/FPU 同步。 |
| `target/i386/hax/hax-windows.h` | Windows 平台类型和声明。 |
| `target/i386/hax/hax-posix.c` | Darwin/NetBSD 等 POSIX 平台驱动访问。 |
| `target/i386/hax/hax-posix.h` | POSIX 平台类型和声明。 |
| `target/i386/hax/meson.build` | HAX 源文件加入 i386 system target。 |

## 构建系统入口

恢复 HAXM 时需要维护以下入口：

```text
accel/Kconfig
accel/stubs/meson.build
docs/about/removed-features.rst
hw/i386/pc_q35.c
hw/intc/apic_common.c
include/exec/poison.h
include/system/hw_accel.h
meson.build
meson_options.txt
qemu-options.hx
scripts/meson-buildoptions.sh
system/vl.c
target/i386/meson.build
target/i386/hax/meson.build
```

### `meson_options.txt`

新增 feature option：

```meson
option('hax', type: 'feature', value: 'auto',
       description: 'HAX acceleration support')
```

维护点：

- 需要和其它加速器选项如 `kvm`、`whpx`、`hvf` 放在同一区域。
- 如果以后运行 `make update-buildoptions`，确认 `scripts/meson-buildoptions.sh` 中的 `--enable-hax` 没被丢失。

### `scripts/meson-buildoptions.sh`

新增帮助项：

```sh
printf "%s\n" '  hax             HAX acceleration support'
```

新增解析项：

```sh
--enable-hax) printf "%s" -Dhax=enabled ;;
--disable-hax) printf "%s" -Dhax=disabled ;;
```

维护点：

- `./configure --enable-hax` 最终要转成 `-Dhax=enabled`。
- 如果 `./configure --help | grep -i hax` 没有输出，先检查这里。

### `meson.build`

核心入口：

```meson
accelerator_targets += {
  'CONFIG_HAX': ['i386-softmmu', 'x86_64-softmmu'],
}
```

HAX 可用性逻辑：

```meson
if get_option('hax').allowed()
  if get_option('hax').enabled() or host_os in ['windows', 'darwin', 'netbsd']
    accelerators += 'CONFIG_HAX'
  endif
endif
```

显式启用但不可用时报错：

```meson
if 'CONFIG_HAX' not in accelerators and get_option('hax').enabled()
  error('HAX not available on this platform')
endif
```

摘要中显示：

```meson
summary_info += {'HAX support': config_all_accel.has_key('CONFIG_HAX')}
```

维护点：

- HAX 只支持 i386/x86_64 system emulator，不要加入 user target。
- Windows 下 `host_os == 'windows'` 时自动允许 HAX，和旧 QEMU 行为一致。
- 如果后续只希望显式启用 HAX，可以去掉 `host_os in [...]` 的 auto 行为，但要同步更新文档。

### `accel/Kconfig`

恢复：

```kconfig
config HAX
    bool
```

维护点：

- 位置通常和 `WHPX`、`NVMM`、`HVF` 等同级。
- 不需要在这里声明平台条件，平台条件由 Meson 控制。

### `target/i386/meson.build`

需要：

```meson
i386_ss.add(when: 'CONFIG_HAX', if_true: files('host-cpu.c'))
subdir('hax')
```

维护点：

- `host-cpu.c` 用于 x86 加速器的 host CPU model 支持。
- `subdir('hax')` 应和 `kvm`、`whpx`、`nvmm`、`hvf` 等 i386 加速器子目录放在一起。

### `target/i386/hax/meson.build`

当前写法：

```meson
i386_system_ss.add(when: 'CONFIG_HAX', if_true: files(
  'hax-all.c',
  'hax-mem.c',
  'hax-accel-ops.c',
))

if host_os == 'windows'
  i386_system_ss.add(when: 'CONFIG_HAX', if_true: files('hax-windows.c'))
else
  i386_system_ss.add(when: 'CONFIG_HAX', if_true: files('hax-posix.c'))
endif
```

重要：不要使用旧写法：

```meson
i386_system_ss.add(when: ['CONFIG_HAX', 'CONFIG_WIN32'], if_true: files('hax-windows.c'))
```

原因：

- `CONFIG_WIN32` 是 host config 符号，存在于 `config-host.h`。
- `i386_system_ss.add(when: ...)` 这里按 target config/source set 解析。
- 如果继续用旧条件，`hax-windows.c` 可能不会进入 `libqemu-x86_64-softmmu.a`，最终链接出现大量 `undefined reference to hax_*`。

### `include/system/hw_accel.h`

需要引入 HAX：

```c
#include "system/hax.h"
```

并把 `hax_enabled()` 纳入硬件加速器判断：

```c
return hvf_enabled()
    || hax_enabled()
    || kvm_enabled()
    || nvmm_enabled()
    || whpx_enabled();
```

维护点：

- 如果当前树新增了其它加速器，也要保持 `hwaccel_enabled()` 的语义正确。

### `include/exec/poison.h`

恢复：

```c
#pragma GCC poison CONFIG_HAX
```

维护点：

- 防止不该直接用 `CONFIG_HAX` 的地方误用。
- HAX 专用代码可在 per-target 环境使用该符号。

### `system/vl.c`

删除 HAX 前，board 初始化后有一段 HAX 专用同步：

```c
if (hax_enabled()) {
    /* FIXME: why isn't cpu_synchronize_all_post_init enough? */
    hax_sync_vcpus();
}
```

当前恢复时需要把它接回 `qemu_init_board()` 的 `realtime_init()` 之后，并使用当前头路径：

```c
#include "system/hax.h"
```

维护点：

- 这是旧 HAX 初始化路径的一部分，不是普通构建入口。
- 如果启动早期出现 CPU 状态不同步、BIOS 行为和 TCG/WHPX 明显不同，应确认这段没有漏掉。

### `hw/i386/pc_q35.c`

当前 q35 host bridge 用 `smm-ranges` 控制是否创建 `smram-region`。HAX 不支持真正的 SMM 执行，但 q35 的低端 legacy PCI/VGA 窗口通过 `smram-region` 路由：

```text
00000000000a0000-00000000000bffff (prio 1, i/o): vga-lowmem
```

如果 HAX 下直接让 `smm-ranges = x86_machine_is_smm_enabled(x86ms)`，q35 不会创建该窗口，`0xa0000-0xbffff` 会继续落到 `pc.ram`，SeaBIOS 文本写入 `0xb8000` 后图形窗口仍然黑屏。

当前处理：

```c
#include "system/hax.h"

smm_enabled = x86_machine_is_smm_enabled(x86ms);
smm_ranges = smm_enabled || hax_enabled();

object_property_set_bool(phb, PCI_HOST_PROP_SMM_RANGES,
                         smm_ranges, NULL);
...
qdev_prop_set_bit(lpc_dev, "smm-enabled", smm_enabled);
```

维护点：

- `smm-ranges` 对 HAX 打开，是为了保留 q35 low legacy PCI/VGA memory window。
- `smm-enabled` 仍不能因为 HAX 打开；HAX 没有完整 SMI/SMM 执行支持。
- 如果 `-accel hax -machine q35 -cpu qemu64 -m 512M` 黑屏但 guest 仍在跑，优先用 `info mtree -f` 检查 `0xa0000-0xbffff` 是否是 `vga-lowmem`，而不是 `pc.ram`。

### `qemu-options.hx`

把帮助文本中的加速器列表加回 `hax`：

```text
supported accelerators are kvm, xen, hax, hvf, nitro, nvmm, whpx, mshv or tcg
```

维护点：

- 这里是用户可见帮助。如果 `-accel help` 有 hax，但 `qemu-system-x86_64 -help` 文本没列 hax，优先检查该文件。

### `docs/about/removed-features.rst`

需要移除 HAXM 已删除小节：

```text
HAXM (``-accel hax``) (removed in 8.2)
```

维护点：

- 当前分支已经恢复 HAXM，不应继续在 removed-features 里声明 HAXM 被删除。
- 不建议把这段改成“已恢复”，因为 `removed-features.rst` 的语义是上游删除功能列表；直接移除更清晰。

## API 适配明细

旧 HAXM 代码来自 QEMU 8.1 附近。迁移到当前树时，主要差异如下。

| 旧写法 | 当前写法 | 说明 |
| --- | --- | --- |
| `#include "sysemu/..."` | `#include "system/..."` | 当前树大量 sysemu 头已迁到 system 命名空间。 |
| `include/sysemu/hax.h` | `include/system/hax.h` | 统一使用 system 路径。 |
| `#include "exec/address-spaces.h"` | `#include "system/address-spaces.h"` | address space 头文件迁移。 |
| `#include "hw/boards.h"` | `#include "hw/core/boards.h"` | MachineState/MachineClass 头路径变化。 |
| `cpu->env_ptr` | `cpu_env(cpu)` | 当前 CPUState 不再暴露 `env_ptr` 字段。 |
| `qemu_mutex_lock_iothread()` | `bql_lock()` | 当前 BQL API。 |
| `qemu_mutex_unlock_iothread()` | `bql_unlock()` | 当前 BQL API。 |
| `qemu_wait_io_event(cpu)` | `qemu_process_cpu_events(cpu)` + loop | 当前 KVM/WHPX/NVMM 等加速器线程模型。 |
| `cpu_physical_memory_rw()` | `address_space_rw()` | 合并读写 helper 已删除。 |
| `class_init(ObjectClass *, void *)` | `class_init(ObjectClass *, const void *)` | 当前 `TypeInfo.class_init` 签名。 |
| `init_machine(MachineState *)` | `init_machine(AccelState *, MachineState *)` | 当前 `AccelClass.init_machine` 签名。 |

### 头文件补充

`hax-all.c` 当前需要：

```c
#include "accel/accel-ops.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/i386/apic.h"
#include "qemu/thread.h"
```

原因：

- `accel/accel-ops.h` 提供完整 `AccelClass` 定义。
- `exec/cpu-common.h` 提供 `cpu_physical_memory_read/write()` 声明。
- `system/address-spaces.h` 提供 `address_space_memory` / `address_space_io`。
- `hw/core/boards.h` 提供 `MachineState` 完整定义。
- `hw/i386/apic.h` 提供 TPR access report 和 APIC base 同步所需接口。
- `qemu/thread.h` 提供 HAX CPUID/run 串行化使用的 `QemuMutex`。

`hax-mem.c` 当前需要：

```c
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/ramlist.h"
```

原因：

- `MemoryRegionSection`、`MemoryListener`、`memory_region_*()` 来自 `system/memory.h`。
- `RAMBlockNotifier`、`ram_block_notifier_add()` 来自 `system/ramlist.h`。

`hax-accel-ops.c` 当前需要：

```c
#include "accel/accel-cpu-ops.h"
```

原因：

- `AccelOpsClass`、`ACCEL_OPS_CLASS()`、`ACCEL_OPS_NAME()`、`TYPE_ACCEL_OPS` 定义在该头里。

### vCPU 线程循环适配

旧 HAX 线程形态类似：

```c
qemu_mutex_lock_iothread();
...
do {
    if (cpu_can_run(cpu)) {
        hax_smp_cpu_exec(cpu);
    }
    qemu_wait_io_event(cpu);
} while (!cpu->unplug || cpu_can_run(cpu));
```

当前应参考 WHPX/KVM 的形态：

```c
bql_lock();
...
do {
    qemu_process_cpu_events(cpu);

    if (cpu_can_run(cpu)) {
        r = hax_smp_cpu_exec(cpu);
        if (r == EXCP_DEBUG) {
            cpu_handle_guest_debug(cpu);
        }
    }
} while (!cpu->unplug || cpu_can_run(cpu));
...
bql_unlock();
```

`hax_smp_cpu_exec()` 内部进入 HAXM vCPU run 前仍需要释放 BQL，返回后重新持有 BQL：

```c
bql_unlock();
cpu_exec_start(cpu);
hax_ret = hax_vcpu_run(vcpu);
cpu_exec_end(cpu);
bql_lock();
```

维护点：

- 加速器线程启动后应持有 BQL。
- 进入宿主加速器执行 guest 前释放 BQL。
- 返回 QEMU 处理 MMIO/PIO/中断/状态同步前重新持有 BQL。
- 当前 `CPUState.thread` 和 `CPUState.halt_cond` 已由 CPU core 初始化，HAX 的 `create_vcpu_thread` 不能重新分配这两个对象；否则 `qemu_cpu_kick()` 和 vCPU 线程等待可能使用不同的同步对象。
- Windows 下旧 `qemu_wait_io_event()` 里的 `SleepEx(0, TRUE)` 需要移到 HAX vCPU loop 中，用来消费 `hax_kick_vcpu_thread()` 排队的 dummy APC。
- 如果后续 QEMU 的 CPU thread loop 再变化，优先对照 `accel/whpx/whpx-accel-ops.c` 和 `accel/kvm/kvm-accel-ops.c`。

### HAX SMP CPUID 适配

现象：

```text
-accel hax -machine q35 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=build/pc-bios/edk2-x86_64-code.fd \
  -m 2048 -smp 4,sockets=1,cores=4,threads=1 \
  -cpu Skylake-Client-noTSX-IBRS
```

1 或 2 vCPU 可以进入 OVMF/TianoCore，4 vCPU 或更多时图形窗口停在黑屏或固件占位画面。调试 OVMF MP mailbox 时可见 AP 已收到 INIT/SIPI，但后续卡在 MP 初始化或异常处理路径。

原因：

- HAXM 驱动内部只给 vCPU 0 分配 `guest_cpuid`，其它 vCPU 共享 vCPU 0 的 CPUID 缓冲。
- CPUID leaf 1 的 `EBX[31:24]` 包含当前 vCPU 的 initial APIC ID，QEMU 为每个 vCPU 生成的值不同。
- 多 vCPU 并发进入 HAXM 时，如果共享 CPUID 缓冲里保留了其它 vCPU 的 APIC ID/topology，固件 MP 初始化可能把 AP 识别错，进而挂在 OVMF 早期路径。

当前处理：

- 从 HAXM capability 中识别 `HAX_CAP_CPUID`。
- 实现 `HAX_VCPU_IOCTL_SET_CPUID` 的 Windows/POSIX 平台入口。
- 单 vCPU 时不调用 `SET_CPUID`，继续使用 HAXM 驱动默认 CPUID 路径。
- SMP 时只下发 CPUID leaf `1`，用于刷新当前 vCPU 的 APIC ID/topology。
- leaf `1` 的 family/model 和 feature bits 仍按 HAXM 默认策略保守处理：新 Intel family 6 model 回退到 `0x000106f1`，ECX/EDX 只保留旧 HAXM 默认支持的特性集合。
- 在每次进入 `hax_vcpu_run()` 前按当前 vCPU 刷新 CPUID，并用 `hax_cpuid_run_mutex` 把 `SET_CPUID` 和 `hax_vcpu_run()` 串行化，避免其它 vCPU 在本 vCPU 执行 CPUID 时改写共享缓冲。

维护点：

- 这会牺牲 HAXM SMP 并行度，但 HAXM 驱动的 CPUID 存储模型本身不是 per-vCPU；为了正确性需要接受这个限制。
- 不要把所有 QEMU CPUID leaf 都传给 HAXM。旧 HAXM 的默认 CPUID 路径会主动屏蔽部分 host/QEMU feature，并对较新的 Intel model 做回退；直接下发完整 QEMU CPUID 可能绕过这些保护。
- 尤其不要为了统一逻辑在 `-smp 1` 下也调用 `SET_CPUID`。曾经把 `Skylake-Client-noTSX-IBRS` 的完整 CPUID 下发给 HAXM 后，Linux guest 会进入 HAXM 不能可靠支持的 CPU/PMU 路径并触发 kernel panic。
- 如果 HAXM 驱动不支持 `HAX_CAP_CPUID`，当前代码保持旧行为；这种驱动下 SMP topology 仍可能不正确。
- 如果以后改动这段逻辑，至少要验证 OVMF q35 1、2、4 vCPU 都能显示 TianoCore/PXE 或进入 UEFI shell。
- 还要验证至少一个 Linux guest 的 `-smp 1` 启动路径，避免 CPUID 修复只覆盖固件阶段却破坏内核启动。

### fast MMIO 适配

旧代码使用已删除的 helper：

```c
cpu_physical_memory_rw(hft->gpa, &hft->value, hft->size, hft->direction);
```

当前替换为：

```c
address_space_rw(&address_space_memory, hft->gpa,
                 MEMTXATTRS_UNSPECIFIED, &hft->value, hft->size,
                 hft->direction);
```

`hft->direction` 在旧代码中作为读写方向布尔值使用。HAX API v4 的 direction 为 `2` 时表示 GPA 到 GPA2 的 MMIO move，这段逻辑仍保留：

```c
cpu_physical_memory_read(hft->gpa, &value, hft->size);
cpu_physical_memory_write(hft->gpa2, &value, hft->size);
```

维护点：

- 如果后续 `address_space_rw()` 签名变化，参考 WHPX/HVF 的 PIO/MMIO 处理。
- `MEMTXATTRS_UNSPECIFIED` 是当前普通内存访问的默认属性。

### ROMD 内存映射适配

旧 HAX 代码会忽略 ROMD region：

```text
Ignoring ROMD region 0x00000000ffc84000->0x0000000100000000
Ignoring ROMD region 0x00000000000e0000->0x0000000000100000
```

当前默认固件代码可能以 ROMD region 形式出现在 guest 物理地址空间中。如果 HAX 不映射这些区域，guest vCPU 可能无法正常取指，随后触发 HAX state-change exit。

当前处理：

- `hax_process_section()` 接受 `memory_region_is_ram(mr)` 或 `memory_region_is_romd(mr)`。
- 对 `memory_region_is_rom(section->mr)` 或 `memory_region_is_romd(section->mr)` 的区域设置 `HAX_RAM_INFO_ROM`。

维护点：

- HAXM ioctl 已有 `HAX_RAM_INFO_ROM` 只读映射标志，不需要改驱动 ABI。
- 这类 ROMD 映射方式和当前 HVF/KVM 对 ROMD 的处理方向一致。

### HAX state-change exit 适配

旧 HAX 代码在 `HAX_EXIT_STATECHANGE` 中打印：

```text
VCPU shutdown request
```

旧 HAX 实现把 `HAX_EXIT_STATECHANGE` 当作 guest shutdown 处理。当前移植应保留这个用户可见行为和日志，同时补上当前 vCPU loop 需要的返回条件：

```c
fprintf(stdout, "VCPU shutdown request\n");
qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
hax_vcpu_sync_state(env, 0);
cpu->exception_index = EXCP_INTERRUPT;
ret = 1;
```

维护点：

- 发起 shutdown 请求后要设置 `cpu->exception_index = EXCP_INTERRUPT`，让 `hax_smp_cpu_exec()` 返回外层 vCPU loop。
- 不要删除 `VCPU shutdown request` 日志；它对判断 guest 是否触发 state-change exit 有用。
- 不要只设置局部 `ret = 1` 后继续依赖 `cpu->exception_index` 的旧循环，否则容易重复进入 HAX run loop 或重复打印。
- `HAX_EXIT_UNKNOWN_VMEXIT` 和 default exit 路径同样应设置 `EXCP_INTERRUPT` 后返回外层，让系统请求被主循环处理。

### HAX HLT exit 状态同步

现象：

```text
-accel hax -machine q35 -cpu qemu64 -m 512M
```

图形窗口一直黑屏；TCG 使用相同机器参数可以正常进入 BIOS。

通过 QMP/HMP `info registers` 采样可见：

- HAX 约 3 秒和 10 秒都停在 `EIP=000108bc`、`HLT=1`。
- TCG 约 3 秒也会到 `EIP=000108bc`、`HLT=1`，但约 10 秒后会推进到 `CS=f000 EIP=0000b757` 的 BIOS 等待点。

原因：

- HAX vCPU run 期间不会持续更新 QEMU 侧 `CPUArchState`。
- `HAX_EXIT_HLT` 返回后，如果不先同步真实 HAX 寄存器状态，外层 HLT 唤醒判断会继续使用旧的 `env->eflags`、`env->eip` 和段状态。
- 结果是 SeaBIOS 的 HLT 等待定时器中断后，QEMU/HAX 的 halt/unhalt 判断可能基于过期状态，guest 无法按预期推进。

处理：

```c
case HAX_EXIT_HLT:
    hax_vcpu_sync_state(env, 0);
    ...
```

维护点：

- 这段同步应放在 `target/i386/hax/hax-all.c` 的 `HAX_EXIT_HLT` 分支中，在检查 `CPU_INTERRUPT_HARD` / `CPU_INTERRUPT_NMI` 前执行。
- 修复后，`-accel hax -machine q35 -cpu qemu64 -m 512M -display none` 的寄存器采样应能从 `EIP=000108bc HLT=1` 推进到 `CS=f000 EIP=0000b757` 附近。
- 不要依赖 QEMU shadow `env->eip` 判断 HAX 当前真实执行位置，必要时先同步寄存器或通过 `info registers` 采样。

### KVM VAPIC option ROM 适配

旧 HAX 删除前，`hw/intc/apic_common.c` 中创建 `kvmvapic` option ROM 的条件包含：

```c
!hax_enabled()
```

当前树如果只恢复了 HAX 加速器文件，但没有恢复这条运行时保护，HAX 启动时也可能加载 KVM VAPIC option ROM。现象通常是：

- `-accel hax -machine q35 -cpu Haswell -m 512M` 图形窗口黑屏。
- TCG 使用同样机器参数可以正常进入 BIOS。
- 调试 vCPU 状态时，guest 可能停在包含 `kvm aPiC` 字符串的 option ROM 附近。

处理：

```c
#include "system/hax.h"

if (!vapic && s->vapic_control & VAPIC_ENABLE_MASK &&
        !hax_enabled() && current_machine->ram_size >= 1024 * 1024) {
    vapic = sysbus_create_simple("kvmvapic", -1, NULL);
}
```

维护点：

- 这是上游删除 HAX 前已有的 HAX 专用保护，不是新增策略。
- HAX 不应走 KVM VAPIC 辅助路径；恢复 HAX 时要同步恢复这处非 HAX 目录里的条件。
- 如果以后 `apic_common.c` 的 VAPIC 创建逻辑重构，仍要保留 HAX 排除条件或等价机制。

### q35 legacy VGA lowmem 适配

现象：

```text
-accel hax -machine q35 -cpu qemu64 -m 512M
```

guest 实际已经运行到 SeaBIOS 等待点，但图形窗口一直黑屏。QMP/HMP 采样可见：

- `info registers` 已推进到 `CS=f000 EIP=0000b757` 附近。
- `screendump` 全黑，像素数据全为 0。
- `pmemsave 0xb8000` 能在普通 RAM 中看到 `Boot failed` 等 SeaBIOS 文本。
- `info mtree -f` 中 `0xa0000-0xbffff` 显示为 `pc.ram`，而不是 `vga-lowmem`。

原因：

- q35 当前通过 `smm-ranges` 创建 `smram-region`。
- `smram-region` 在普通 CPU 视图中把 `0xa0000-0xbffff` 路由到 PCI address space，从而让 VGA 的 `vga-lowmem` 覆盖 RAM。
- HAX 不支持真正的 SMM，所以 `x86_machine_is_smm_enabled()` 对 HAX 返回 false。
- 如果直接把这个 false 传给 q35 host bridge 的 `smm-ranges`，q35 不创建 `smram-region`，legacy VGA memory window 就不会出现在系统 flatview 中。

处理：

```c
smm_enabled = x86_machine_is_smm_enabled(x86ms);
smm_ranges = smm_enabled || hax_enabled();

object_property_set_bool(phb, PCI_HOST_PROP_SMM_RANGES,
                         smm_ranges, NULL);
qdev_prop_set_bit(lpc_dev, "smm-enabled", smm_enabled);
```

维护点：

- 这是 q35 memory topology 适配，不是 VGA 设备修复。
- 不要把 `smm-enabled` 一起改成 `smm_ranges`；否则会把 HAX 不支持的 SMI/SMM 路径暴露给 guest。
- 修复后 `info mtree -f` 应出现 `00000000000a0000-00000000000bffff (prio 1, i/o): vga-lowmem`。
- 修复后 `screendump` 应不再全黑，`pmemsave 0xb8000` 也不应再在普通 RAM 中看到 SeaBIOS 文本。

### 删除提交外围项审计

上游删除提交 `b91b0fc1635544341b9d00d1addc8ddf48e5b389` 不只删除了 `target/i386/hax/`。恢复时至少要审计这些外围项：

```text
MAINTAINERS
docs/about/build-platforms.rst
docs/about/deprecated.rst
docs/about/index.rst
docs/about/removed-features.rst
docs/system/index.rst
docs/system/introduction.rst
hw/i386/pc_q35.c
hw/intc/apic_common.c
include/hw/core/cpu.h
include/system/hw_accel.h
system/cpus.c
system/vl.c
```

当前处理建议：

- `hw/i386/pc_q35.c`、`hw/intc/apic_common.c` 和 `system/vl.c` 是运行相关项，必须恢复或做等价适配。
- `system/cpus.c` 中旧的 Windows `SleepEx(0, TRUE)` 逻辑已经移到 HAX vCPU thread loop 中，位置在 `target/i386/hax/hax-accel-ops.c`。
- `include/hw/core/cpu.h` 的 `vcpu_dirty` 注释应保留 HAX，字段本身当前仍存在。
- `MAINTAINERS` 应补回 HAXM orphan 段，并把旧 `include/sysemu/hax.h` 路径改成当前 `include/system/hax.h`。
- 用户手册里的“supported accelerators”列表是否恢复 `hax` 要结合当前分支定位决定；HAXM 已停止维护，不建议把它重新描述成官方推荐能力。
- 当前树如果已经没有 `scripts/ci/org.centos/stream/8/x86_64/configure`，不需要恢复该旧 CI 配置项。

## Windows 构建验证

必须在 MINGW64 环境运行，例如 `C:\msys64\mingw64.exe`。不要使用普通 MSYS shell 代替，否则 PATH、编译器、Python、pkg-config 等环境可能不一致。

推荐命令：

```sh
cd /d/git/GreenDamTan/qemu
export CC="ccache gcc"
export CXX="ccache g++"
./configure --enable-gtk \
            --enable-sdl \
            --target-list=x86_64-softmmu \
            --disable-werror \
            --disable-capstone \
            --enable-hax \
            --prefix=/artifacts
meson compile -C build
```

配置摘要中应出现：

```text
Targets and accelerators
  HAX support                     : YES
  WHPX support                    : YES
  TCG support                     : YES
  target list                     : x86_64-softmmu
```

验证加速器注册：

```sh
build/qemu-system-x86_64.exe -accel help
```

期望输出包含：

```text
Accelerators supported in QEMU binary:
tcg
hax
whpx
```

本次恢复已通过完整 `meson compile -C build`，并确认 `-accel help` 输出包含 `hax`。

## 可选运行验证

如果宿主机安装了 HAXM 驱动，可以进一步尝试：

```sh
build/qemu-system-x86_64.exe \
  -accel hax \
  -machine q35 \
  -cpu host \
  -m 512M \
  -display none \
  -nodefaults \
  -S
```

如果只是验证 accelerator 初始化，也可以使用很小的 guest 配置。实际运行时如果出现 “No accelerator found” 或 HAXM 驱动打开失败，应先检查：

- HAXM 驱动是否安装。
- BIOS/UEFI 中 VT-x 是否启用。
- Hyper-V、Windows Hypervisor Platform、VBS、Credential Guard 是否占用了 VT-x。
- 当前 Windows 版本是否仍支持 HAXM 驱动。
- 是否以足够权限运行。

## 常见错误和处理

### `Unknown option: hax`

现象：

```text
ERROR: Unknown option: "hax"
```

检查：

- `meson_options.txt` 是否存在 `option('hax', ...)`。
- `scripts/meson-buildoptions.sh` 是否包含 `--enable-hax`。
- 如果是旧 build 目录缓存问题，重新运行 `./configure ... --enable-hax`。

### `-accel help` 不列出 hax

检查：

- 配置摘要中 `HAX support` 是否为 `YES`。
- `meson.build` 中 `CONFIG_HAX` 是否加入 `accelerators`。
- `accelerator_targets` 是否把 `CONFIG_HAX` 绑定到当前 target。
- 当前 target 是否是 `i386-softmmu` 或 `x86_64-softmmu`。

### 找不到 `exec/address-spaces.h`

现象：

```text
fatal error: exec/address-spaces.h: No such file or directory
```

处理：

```c
#include "system/address-spaces.h"
```

### 找不到 `hw/boards.h`

现象：

```text
fatal error: hw/boards.h: No such file or directory
```

处理：

```c
#include "hw/core/boards.h"
```

### `CPUState` 没有 `env_ptr`

现象：

```text
error: 'CPUState' has no member named 'env_ptr'
```

处理：

```c
CPUArchState *env = cpu_env(cpu);
```

### `AccelOpsClass` 不完整或 `ACCEL_OPS_CLASS` 未声明

现象：

```text
invalid use of incomplete typedef 'AccelOpsClass'
implicit declaration of function 'ACCEL_OPS_CLASS'
```

处理：

```c
#include "accel/accel-cpu-ops.h"
```

### `AccelClass` 不完整

现象：

```text
invalid use of incomplete typedef 'AccelClass'
```

处理：

```c
#include "accel/accel-ops.h"
```

### `init_machine` 函数指针类型不匹配

现象：

```text
assignment to 'int (*)(AccelState *, MachineState *)'
from incompatible pointer type 'int (*)(MachineState *)'
```

处理：

```c
static int hax_accel_init(AccelState *as, MachineState *ms)
```

### `MemoryRegionSection` 或 `MemoryListener` 不完整

现象：

```text
invalid use of incomplete typedef 'MemoryRegionSection'
variable 'hax_memory_listener' has initializer but incomplete type
```

处理：

```c
#include "system/memory.h"
```

### `RAMBlockNotifier` 不完整

现象：

```text
unknown type name 'RAMBlockNotifier'
storage size of 'hax_ram_notifier' isn't known
```

处理：

```c
#include "system/ramlist.h"
```

### 链接大量 `undefined reference to hax_*`

现象：

```text
undefined reference to `hax_mod_open'
undefined reference to `hax_vcpu_run'
undefined reference to `hax_sync_vcpu_state'
undefined reference to `hax_kick_vcpu_thread'
```

优先检查：

- Windows 下 `target/i386/hax/meson.build` 是否把 `hax-windows.c` 编进来。
- POSIX 下是否把 `hax-posix.c` 编进来。
- 不要用 `when: ['CONFIG_HAX', 'CONFIG_WIN32']` 判断平台文件。

可以用以下命令确认 `build.ninja` 是否包含平台文件：

```sh
grep -n "hax-windows\|hax-posix" build/build.ninja
```

### `qemu_mutex_lock_iothread` 或 `qemu_wait_io_event` 未声明

现象：

```text
implicit declaration of function 'qemu_mutex_lock_iothread'
implicit declaration of function 'qemu_wait_io_event'
```

处理：

- `qemu_mutex_lock_iothread()` 改成 `bql_lock()`。
- `qemu_mutex_unlock_iothread()` 改成 `bql_unlock()`。
- vCPU 主循环改为调用 `qemu_process_cpu_events(cpu)`。

参考：

```text
accel/whpx/whpx-accel-ops.c
accel/kvm/kvm-accel-ops.c
target/i386/nvmm/nvmm-accel-ops.c
```

### `cpus_register_accel` 断言 `ops->handle_interrupt`

现象：

```text
ERROR:../system/cpus.c:697:cpus_register_accel: assertion failed: (ops->handle_interrupt)
Bail out! ERROR:../system/cpus.c:697:cpus_register_accel: assertion failed: (ops->handle_interrupt)
```

原因：

- 当前 `AccelOpsClass` 要求 `create_vcpu_thread` 和 `handle_interrupt` 都必须存在。
- 旧 HAX 代码的 `hax_accel_ops_class_init()` 只注册了 vCPU 线程和 kick 回调，漏了中断处理回调。

处理：

```c
ops->handle_interrupt = generic_handle_interrupt;
```

维护点：

- 这行应放在 `target/i386/hax/hax-accel-ops.c` 的 `hax_accel_ops_class_init()` 中。
- WHPX、KVM、NVMM 等非 TCG 加速器也使用 `generic_handle_interrupt`。

### Linux guest 启动时 kernel panic

现象：

```text
-accel hax -cpu Skylake-Client-noTSX-IBRS -smp 1
```

固件阶段可以进入，但 Linux guest 启动内核后 panic。换回不改 HAX CPUID 的版本或使用 TCG 时，guest 可以继续启动。

原因：

- HAXM 驱动默认 CPUID 路径会主动屏蔽部分 CPU feature，并把较新的 Intel family 6 model 回退到旧 model，避免 guest 进入 HAXM 不能可靠模拟的 CPU/PMU 路径。
- 如果 QEMU 在 `-smp 1` 下也调用 `HAX_VCPU_IOCTL_SET_CPUID`，并把 `Skylake-Client-noTSX-IBRS` 的完整 CPUID leaf 下发给 HAXM，就会绕过 HAXM 的默认兼容性过滤。
- 这类问题和大小核 host 有一定相关性，但根因不是简单的 P/E core feature 交集，而是 HAXM 对较新 CPUID/PMU 组合支持不足。

处理：

- 单 vCPU 时不要调用 `SET_CPUID`，继续使用 HAXM 默认 CPUID。
- SMP 时只为修复 APIC ID/topology 下发 leaf `1`，不要下发完整 QEMU CPUID 表。
- leaf `1` 的 family/model 和 feature bits 要按 HAXM 默认策略保守过滤。

维护点：

- 不要为了统一代码路径把 `-smp 1` 也纳入 HAX CPUID 刷新逻辑。
- 修改 HAX CPUID 逻辑后，除了验证 OVMF 1、2、4 vCPU，还要至少验证一个 Linux guest 的单 vCPU 启动路径。

### `hax_start_vcpu_thread` 断言 `cpu->accel`

现象：

```text
ERROR:../target/i386/hax/hax-accel-ops.c:76:hax_start_vcpu_thread: assertion failed: (cpu->accel)
Bail out! ERROR:../target/i386/hax/hax-accel-ops.c:76:hax_start_vcpu_thread: assertion failed: (cpu->accel)
```

原因：

- 当前 HAX vCPU 私有状态是在 vCPU 线程里的 `hax_init_vcpu(cpu)` 中创建并赋给 `cpu->accel`。
- `hax_start_vcpu_thread()` 在主线程中调用 `qemu_thread_create()` 后立刻返回，不能假设子线程已经完成 `hax_init_vcpu(cpu)`。
- Windows 路径需要保存 `hThread` 给 `QueueUserAPC()` 使用，但这个保存动作必须等 `cpu->accel` 已经存在。

处理：

- 删除 `hax_start_vcpu_thread()` 中的 `assert(cpu->accel)`。
- 把 Windows 下的 `cpu->accel->hThread = qemu_thread_get_handle(cpu->thread);` 移到 `hax_cpu_thread_fn()` 中，放在 `hax_init_vcpu(cpu)` 之后、`cpu_thread_signal_created(cpu)` 之前。

维护点：

- `cpu_thread_signal_created(cpu)` 是主线程等待 vCPU 创建完成的同步点；在它之前填好 `hThread`，后续 kick vCPU 时才可靠。
- 不要在 `qemu_thread_create()` 返回后立即访问 HAX 的 `cpu->accel`，那是竞态。
- 不要在 `hax_start_vcpu_thread()` 里重新分配 `cpu->thread` 或 `cpu->halt_cond`；当前 CPU core 已经在 `hw/core/cpu-common.c` 中初始化它们，KVM/WHPX/NVMM 也直接复用这些对象。

## 后续向更新 QEMU 移植的检查清单

每次向更新 QEMU 移植 HAXM 时，建议按以下顺序检查。

### 1. 源码恢复

- `target/i386/hax/` 是否完整。
- `include/system/hax.h` 是否存在。
- `accel/stubs/hax-stub.c` 是否存在。
- 不要恢复旧路径 `include/sysemu/hax.h`。

### 2. 配置入口

- `meson_options.txt` 是否有 `hax`。
- `scripts/meson-buildoptions.sh` 是否有 `--enable-hax`。
- `./configure --help | grep -i hax` 是否能看到 HAX。
- `meson configure build | grep -i hax` 是否能看到 HAX option。

### 3. 加速器目标绑定

- `CONFIG_HAX` 是否加入 `accelerator_targets`。
- `CONFIG_HAX` 是否只绑定 x86 system target。
- 配置摘要中 `HAX support` 是否为 `YES`。

### 4. i386 target 接入

- `target/i386/meson.build` 是否有 `subdir('hax')`。
- `CONFIG_HAX` 是否加入 `host-cpu.c`。
- `target/i386/hax/meson.build` 是否选择了正确平台文件。

### 5. API 迁移

重点检查这些文件：

```text
include/accel/accel-cpu-ops.h
include/accel/accel-ops.h
include/hw/core/cpu.h
include/system/cpus.h
include/system/memory.h
include/system/ramlist.h
hw/i386/pc_q35.c
hw/intc/apic_common.c
system/cpus.c
system/vl.c
```

如果接口变化，优先参考当前树中的：

```text
accel/kvm/kvm-accel-ops.c
accel/whpx/whpx-accel-ops.c
target/i386/whpx/whpx-all.c
target/i386/nvmm/nvmm-accel-ops.c
target/i386/nvmm/nvmm-all.c
```

### 6. 编译验证

至少执行：

```sh
./configure --target-list=x86_64-softmmu --enable-hax --disable-werror
meson compile -C build
build/qemu-system-x86_64.exe -accel help
```

在本项目 Windows 构建中，使用完整 MINGW64 命令：

```sh
cd /d/git/GreenDamTan/qemu
export CC="ccache gcc"
export CXX="ccache g++"
./configure --enable-gtk \
            --enable-sdl \
            --target-list=x86_64-softmmu \
            --disable-werror \
            --disable-capstone \
            --enable-hax \
            --prefix=/artifacts
meson compile -C build
```

### 7. 文档同步

- `docs/about/removed-features.rst` 不应再说 HAXM removed。
- 本文档需要记录新的移植差异。
- 如果新增运行限制或已知问题，需要追加到“运行验证”或“常见错误”。

## 维护建议

- 不要大范围重构 HAXM 代码。先保证最小恢复可构建，再逐步处理运行问题。
- 不要为了消除警告而改变 HAXM ioctl 结构体布局；这些结构对应驱动 ABI。
- 不要把 `hax-windows.c` 和 `hax-posix.c` 同时编进同一个目标。
- 不要把 HAX 加入非 x86 target。
- 不要把 HAXM 当作替代 WHPX 的现代方案。Windows 上 WHPX 仍是更现实的默认选择。
- 如果遇到行为差异，优先比对上游删除前的 HAXM 实现，再比对当前 WHPX/KVM 的接口适配。
- 如果要提交补丁，建议把“恢复源码”和“当前 API 适配”分成不同提交，便于以后定位问题。

## 当前已验证状态

当前恢复已完成以下验证：

```sh
./configure --enable-gtk \
            --enable-sdl \
            --target-list=x86_64-softmmu \
            --disable-werror \
            --disable-capstone \
            --enable-hax \
            --prefix=/artifacts
meson compile -C build
build/qemu-system-x86_64.exe -accel help
```

结果：

- `meson compile -C build` 通过。
- `qemu-system-x86_64.exe` 和 `qemu-system-x86_64w.exe` 成功链接。
- `-accel help` 输出包含 `hax`。
- HAX q35 + OVMF + `Skylake-Client-noTSX-IBRS` 在 1、2、4 vCPU 下均可显示 TianoCore/PXE 画面。
- HAX q35 + SeaBIOS + `qemu64` 仍可显示，`info mtree -f` 中 `0xa0000-0xbffff` 仍为 `vga-lowmem`，没有回退成 `pc.ram`。
- HAX `adl-n` + fnOS/Linux + `Skylake-Client-noTSX-IBRS` + 1 vCPU 可启动到登录画面，未再触发 CPUID 相关 kernel panic。

尚未验证：

- 安装 HAXM 驱动后的真实 guest 启动。
- guest OS 内部的多 vCPU 压力、APIC、dirty logging、迁移等高级场景。
- Darwin/NetBSD POSIX HAX 路径。
