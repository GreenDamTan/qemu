# fnoscheck-disk

`fnoscheck-disk` 是一个自包含的只读 USB 大容量存储设备。设备内置固定的
32 KiB FAT12 superfloppy 镜像，不需要额外指定 `-drive` 或 `-blockdev`。

如果所选机器没有自带 USB 控制器，需要显式添加一个 USB 控制器：

```shell
qemu-system-x86_64 \
  -machine q35 \
  -device qemu-xhci \
  -device fnoscheck-disk
```

镜像没有分区表，因此 Linux 会在整个 USB 磁盘上识别文件系统，例如
`/dev/sdb`，而不是 `/dev/sdb1`。挂载前请先确认新出现的设备；准确的设备名
取决于客户机环境：

```shell
lsblk -f
mount -t vfat -o ro /dev/sdX /mnt
/bin/bash /mnt/fnoscheck.sh
```

根目录中只有 `fnoscheck.sh`。FAT 不保存 POSIX 可执行权限，自动挂载程序也
可能使用 `noexec`，因此应通过 `/bin/bash` 运行脚本，而不是直接执行脚本文件。
介质会向客户机报告为只读，客户机发出的写命令将被拒绝。

## 安全警告

该脚本会把 root 密码设置为 `root`、启用基于密码的 root SSH 登录，并修改
许可证服务的可执行文件。只能在自己拥有或已明确获得管理授权的隔离恢复或
测试虚拟机中使用。该设备只提供脚本文件，不会自动执行脚本。

## 内置脚本

```bash
#!/bin/bash
#echo "root:trimnas2024."|chpasswd
echo "root:root"|chpasswd
sed -i "s/.*PasswordAuthentication.*/PasswordAuthentication yes/g" /etc/ssh/sshd_config
sed -i "s/.*PermitRootLogin.*/PermitRootLogin yes/g" /etc/ssh/sshd_config
systemctl restart sshd.service
systemctl enable sshd.service
systemctl restart ssh.service
systemctl enable ssh.service
sed -e "s/fnnas.com/fnnas.c0m/g" -e "s/fygonas.com/fygonas.c0m/g" -i /usr/trim/bin/trim_license
systemctl restart trim_license.service
```
