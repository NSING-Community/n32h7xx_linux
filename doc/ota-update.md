# 通过网络更新分区（SSH OTA）

本文只说明运行中的设备如何通过 SSH 更新 rootfs、kernel 或 dtb。
普通应用和库部署见 [SDK](sdk.md#部署到设备)，USB MSC 恢复及片内 bootloader
烧写见 [主机工具与烧写](tools.md)。分区布局统一见 [README](../README.md#存储分区)。

## 前提

设备已启动，网络可达，SSH 能登录。主机命令在仓库根目录执行：

```sh
ssh root@<板子IP> 'uname -a; cat /proc/mtd'
```

确认设备身份及 `dtb`、`kernel`、`rootfs`、`system` 四个分区。镜像必须与板型匹配。
设备需有 `/sbin/n32-flash` 或 `/system/bin/n32-flash`；发布 rootfs 已包含它。
旧系统缺少该工具时，从匹配的 rootfs 文件树把这个文件复制到 `/system/bin` 即可。

## 使用 make 更新

| 更新内容 | 命令 | 构建输入 |
| --- | --- | --- |
| rootfs | `make ota-rootfs H787_BOARD=root@<板子IP>` | 当前 rootfs 文件树及构建配置 |
| kernel | `make ota-kernel H787_BOARD=root@<板子IP>` | 内核 zImage |
| dtb | `make ota-dtb H787_BOARD=root@<板子IP>` | 当前板型 DTB |

`H787_BOARD` 没有默认值，用串口、ADB 的 `ifconfig eth0` 或 DHCP 租约确认当前 IP。
想让板子每次开机都用同一个地址，可以在 `rootfs/etc/defaults.conf` 里把
`NETWORK_MODE` 改成 `static`（配置项见 [快速上手](quickstart.md#板级默认配置)）。
目标先构建对应镜像，再通过 SSH 管道送入 `n32-flash ota -`，逐块写入和读回校验，
最后重启。流式传输使用 32 KiB 镜像缓冲，不在板上暂存整份镜像。

root 密码为空、SSH 无法交互提示时，在主机准备 askpass 脚本：

```sh
printf '#!/bin/sh\necho ""\n' >/tmp/askpass.sh
chmod +x /tmp/askpass.sh
H787_SSH_ASKPASS=/tmp/askpass.sh make ota-rootfs H787_BOARD=root@<板子IP>
```

脚本由 `tools/ssh-ota.sh` 实现。设备输出的写入、读回校验结果应成功；重启时 SSH
连接会断开，之后脚本等待 IP 可达。IP 可达只确认网络恢复，仍应重新登录检查
`uname -a`、挂载状态以及更新后的程序或文件。

rootfs 更新保留 `/system`；kernel 和 dtb 更新也不会改写 rootfs、system 或片内
bootloader。每条命令默认分别重启。如果 kernel 与 DTB 必须一起更新，可以在构建
完成后按下一节的 `--no-reboot` 写两份，再统一重启。

## 手动更新已构建的镜像

不希望更新目标重新构建时，可以直接用脚本写指定产物并明确传入设备地址：

```sh
bash tools/ssh-ota.sh out/kernel/zImage kernel root@<板子IP> --no-reboot
bash tools/ssh-ota.sh out/kernel/n32h787-hmi.dtb dtb root@<板子IP> --no-reboot
ssh root@<板子IP> reboot
```

空密码时同样为前两条命令设置 `H787_SSH_ASKPASS`；最后的 SSH 可交互执行。
`--no-reboot` 只推迟重启，新 kernel / DTB 要重启才会加载。

### 直接使用管道

以 rootfs 为例，`--size` 使用本次文件的实际长度，避免复制旧示例中的固定数值：

```sh
image=out/rootfs/rootfs.squashfs
bytes=$(wc -c < "$image")
cat "$image" | ssh root@<板子IP> "n32-flash ota - rootfs --size $bytes"
```

`-` 表示标准输入。也可以在板上从服务器流式下载：

```sh
wget -O - http://<服务器>/rootfs.squashfs | n32-flash ota - rootfs
```

### 先暂存再写入

```sh
scp out/rootfs/rootfs.squashfs root@<板子IP>:/tmp/new-rootfs.squashfs
ssh root@<板子IP> 'n32-flash ota /tmp/new-rootfs.squashfs rootfs'
```

`/tmp` 是无配额的 ramfs，文件占用物理内存。先检查 `/proc/meminfo` 的
`MemAvailable`，为运行中的服务和写入程序留出余量；内存吃紧时使用管道。
写入耗时取决于镜像大小和 NOR 的擦除、编程速度，进度及读回结果由工具输出。

## 写入检查与中断恢复

`n32-flash` 在更新过程中执行以下检查：

- **内核标记的只读分区拒绝**。当前四个分区都可写，所以这条平时不会触发；如果
  以后哪个分区节点加了 `read-only`，`open(O_RDWR)` 会返回 `EACCES`，`n32-flash`
  会直接说明是该标志导致的。
- **挂载中的文件系统拒绝**。`/` 和 `/system` 都会被拦。
  就地更新 rootfs 必须显式加 `--force`，这正是 `ota` 子命令在做的事。
- **镜像不能放在要被擦的分区上**。`ota` 检查镜像源与目标分区是否相同，是同一个
  就拒绝，防止擦除源文件。使用标准输入 `-` 可避免这个问题。
- **镜像在 RAM 文件系统上时会提示**。镜像位于 `/tmp`（ramfs）等吃物理内存又没有
  配额的地方时，`n32-flash` 会打印镜像占用和当前可用内存，见前面的暂存说明。
  这项仅作提示。
- **镜像类型校验**。rootfs 使用 SquashFS、kernel 使用 zImage、dtb 使用 FDT。
  流式写入在首次擦除前检查镜像头部，类型不匹配会拒绝。
- **`--size` 截断检查**（只在 `ota -` 时生效）。流长度和预期不符会报错退出。
  注意它发生在擦除**之后**：管道方式没法在擦之前先验完整份镜像，这是"给文件
  路径时可以顺带校验、走管道时依赖 SSH 自身完整性"这个差别的来源。

写一半掉电的恢复办法：按住 `KEY1`（`PG3`）同时复位，设备进 USB MSC 模式，
从主机重写分区即可。

## 命令速查

设备侧：

```sh
n32-flash info
n32-flash write IMAGE PARTITION [--offset N] [--force] [--no-erase]
n32-flash erase PARTITION [--offset N] [--length N] [--force]
n32-flash ota IMAGE [PARTITION] [--no-reboot] [--size N]
n32-flash ota - PARTITION [--no-reboot] [--size N]
```

更新中断后，用 [USB MSC 流程](tools.md#usb-msc-烧写) 恢复
`out/dist/spi-nor-boot.img`。它覆盖 dtb、kernel、rootfs，保留 system；更换片内
bootloader 使用 [专门的烧写入口](tools.md#烧写-bootloader)。
