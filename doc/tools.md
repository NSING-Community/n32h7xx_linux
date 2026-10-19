# 主机工具与烧写

顶层 `tools/` 是主机侧工具集，用来编镜像、把镜像写进板子、以及看串口。
日常只需要一个入口 `tools/h787ctl`。

环境安装见 [setup.md](setup.md)，首次跑通见 [quickstart.md](quickstart.md)。
本文集中说明 USB MSC、片内 bootloader 烧写和主机调试工具；网络更新见
[ota-update.md](ota-update.md)。

| 文件 | 用途 |
| --- | --- |
| `tools/h787ctl` | 统一入口，下面是它调用的东西 |
| `tools/env.sh` | 路径和板卡常量，由 `h787ctl` 自动 source |
| `tools/spi_image.py` | 把 dtb + kernel + rootfs 组成 flash 镜像 |
| `tools/msc_image.py` | 往板子的 USB MSC 设备写镜像（带容量/型号/读回校验） |
| `tools/openocd_tcl.py` | OpenOCD Tcl-RPC：halt/reset、读寄存器、写 MSC 标志 |
| `tools/serial_console.py` | 串口 capture / send / expect |
| `tools/ssh-ota.sh` | 走 SSH 把分区镜像**管道**进板上的 `n32-flash ota -` 就地刷写（`make ota-rootfs` / `ota-kernel` / `ota-dtb` 调它）；设备上只留 32 KiB 缓冲，镜像多大都不占额外内存 |

## 先自检

```sh
cd ~/research/n32h7xx_linux
tools/h787ctl doctor
```

它会打印推导出的路径，检查 `make`/`python3`/`arm-none-eabi-gcc`/`openocd`
是否就位，列出 `/dev/serial/by-id/` 里的串口，并报告当前 MSC 设备和 OpenOCD
target 状态。环境搭建的完整步骤见 [setup.md](setup.md)。

## USB MSC 烧写

从仓库根目录执行 `make`，构建目标和产物布局见 [README](../README.md#产物)。
USB 更新使用 `out/dist/spi-nor-boot.img`：写入 `[0, 0x710000)` 的 dtb、kernel、
rootfs，保留 `/system`。`spi-nor-16M.img` 是整片镜像，包含擦空的 system 区域，
不能当作日常保留数据的升级镜像。

进入 MSC 模式任选一种：

- 按住 KEY1（PG3）上电或复位。
- 板子 Linux shell 中运行 `reboot msc`。
- 已连接探针并运行 OpenOCD 时，主机执行 `tools/h787ctl msc-enter`。

Windows / WSL 需要将 `19f5:5780` 共享并 attach 到 WSL，具体步骤见
[setup.md](setup.md#2-windows-侧安装-usbipd)。写入并校验：

```sh
sudo tools/h787ctl msc-write out/dist/spi-nor-boot.img
```

校验完成后在 Windows detach MSC，再松开 KEY1 并复位或重新上电。设备已经处于
MSC 模式且具备块设备写权限时，也可以用 `make flash-kernel` 重新构建并写入。

### 校验、预览与单分区写入

`msc-write` 检查设备型号 `N32H7xx Boot`、上报容量、未挂载状态和写入范围。
默认容量期望为 16 MiB，默认禁止覆盖 system；`--allow-system` 会允许覆盖其数据。
写入默认逐字节读回校验，内容一致的扇区会跳过，末尾按 4 KiB 补 `0xff`。

```sh
sudo tools/h787ctl msc-write --check-only out/dist/spi-nor-boot.img
sudo tools/h787ctl msc-write --dry-run out/dist/spi-nor-boot.img
sudo tools/h787ctl msc-write out/rootfs/rootfs.squashfs --offset 0x410000 --magic none
```

分区偏移统一见 [README 存储分区](../README.md#存储分区)。当前 bootloader 即使在
更大的 NOR 上也只上报 16 MiB，MSC 更新仍使用可容纳的 `spi-nor-boot.img`；整片
大容量镜像需要能访问完整 NOR 的烧写方式。仅当设备实际的上报容量改变时，才用
`--expect-bytes` 或 `H787_MSC_BYTES` 调整容量期望。

## 烧写 bootloader

bootloader 位于片内 flash，需要 CMSIS-DAP 探针。恢复工具来自独立的 NSLink
仓库，用 `H787_RECOVER_TOOL` 指定本机路径：

```sh
make flash-bootloader H787_RECOVER_TOOL=/path/to/ns-link/tools/h787_recover_flash.py
```

该目标构建后写入 `out/dist/bootloader.bin`。ELF、HEX、MAP 等调试产物位于
`out/bootloader/CM7/`。USB MSC 和网络 OTA 更新外部 NOR，不会更换片内 bootloader。

## 直接访问 DAP 和串口

`openocd_tcl.py` 走 OpenOCD 的 Tcl-RPC 口（默认 `127.0.0.1:6666`）：

```sh
tools/h787ctl openocd targets
tools/h787ctl openocd halt
tools/h787ctl openocd resume
tools/h787ctl openocd mdw 0x30015f00 2
tools/h787ctl openocd set-msc-flag
tools/h787ctl openocd reset-run
tools/h787ctl openocd recover-halt 700
```

`recover-halt` 用于目标无法正常 halt 时的复位恢复；执行后目标会停止，
需要继续运行时使用 `tools/h787ctl openocd resume`。

`serial_console.py` 直接用 Linux TTY，不需要 `pyserial`：

```sh
tools/h787ctl serial capture --seconds 15 -o /tmp/console.log
tools/h787ctl serial send 'uname -a'
tools/h787ctl serial expect 'login:' --timeout 20
```

串口使用 115200 8N1。同一时间只保留一个读者，避免多个终端同时读取同一设备。
串口权限和 Windows / WSL 设备归属见 [setup.md](setup.md#4-usb-权限)。

## 环境变量

`tools/env.sh` 由 `h787ctl` 自动 source，也可以在仓库根目录手动 source：

```sh
H787_ROOT=$PWD . ./tools/env.sh
```

常用的有 `H787_ROOT`、`H787_LINUX_ROOT`、`H787_SERIAL`、`H787_SERIAL_BAUD`、
`H787_OPENOCD_CFG`、`H787_OPENOCD_TCL_PORT`、`H787_DAP_SERIAL`。
