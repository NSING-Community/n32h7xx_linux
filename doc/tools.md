# 主机工具与烧写

顶层 `tools/` 是主机侧工具集，用来编镜像、把镜像写进板子、以及看串口。
日常只需要一个入口 `tools/h787ctl`。

本文只讲客户会用到的路径。

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

板子的基本情况：

- 启动介质：板上 SPI NOR flash，板子会把它当成 16 MiB 的 USB MSC 设备暴露出来。
- 控制台：NSLink 虚拟串口，115200 8N1。
- flash 布局：dtb 在 `0x000000`、kernel 在 `0x010000`、rootfs 在 `0x410000`、
  可写的 system 分区从 `0x710000` 到 flash 末尾。
- 进 MSC 的三条路：按住 `KEY1`（`PG3`）、用 DAP 写标志、或者在 Linux 里
  `reboot msc`。

## 编译和烧写

```sh
make                       # bootloader + kernel + rootfs + flash 镜像
```

最终产物在 `out/dist/`：

- `spi-nor-16M.img`：整片镜像，system 分区留空（首次启动由内核格式化）。
- `spi-nor-boot.img`：`0x710000` 以下的部分，走 USB 更新时写的就是它，所以用户的
  system 分区不会被动。名字里不带容量：固定分区都在 `0x710000` 结束，所以
  不管 NOR 是 16M 还是 32M，这个文件都是一样的；**只有** `spi-nor-<容量>.img`
  跟容量有关。
- `bootloader.bin`：CM7 bootloader。它烧在片内 flash，不在上面两个 SPI 镜像里，
  `make` 一并放进来，整个 dist 就是"要烧的东西都在这一份"。

| 更新内容 | 命令 | 线 |
| --- | --- | --- |
| dtb / kernel / rootfs | `make flash-kernel` | USB（板子自己的 MSC） |
| rootfs / kernel / dtb（单个分区） | `make ota-rootfs` / `ota-kernel` / `ota-dtb` | 网线 |
| bootloader（片内 flash） | `make flash-bootloader` | CMSIS-DAP 探针 |

走 USB 刷 dtb/kernel/rootfs 的典型流程：

```sh
make                                    # 或者只打镜像：make flash-image
tools/h787ctl msc-enter                 # 也可以按 KEY1，或在板子上 reboot msc
# Windows：usbipd attach --wsl --busid <MSC 的 busid>
sudo tools/h787ctl msc-write out/dist/spi-nor-boot.img
# 写完 usbipd detach，然后 h787ctl openocd reset-run 或断电重上电
```

在 WSL 里写块设备要 `sudo`。`msc-write` 会先核对设备型号必须是
`N32H7xx Boot`、容量正好 16 MiB、且没有挂载；写入范围一旦碰到 system 分区就
拒绝，除非显式给 `--allow-system`。板子换了比 16 MiB 大的 NOR 时，容量期望用
`--expect-bytes 32M` 或 `H787_MSC_BYTES` 跟着调（bootloader 目前仍按 16 MiB
上报，所以写得出的是 `spi-nor-boot.img` 那一块）。内容已经一致的扇区会跳过，载荷按
4 KiB 对齐补 `0xff`，写完默认读回校验：

```sh
tools/h787ctl msc-write --check-only out/dist/spi-nor-boot.img
tools/h787ctl msc-write --dry-run    out/dist/spi-nor-boot.img
tools/h787ctl msc-write out/rootfs/rootfs.squashfs --offset 0x410000 --magic none
```

`make flash-bootloader` 用的是片内 flash 恢复工具，它在单独的 NSLink 仓库里，
路径机器相关，用 `H787_RECOVER_TOOL=` 指过去：

```sh
make flash-bootloader H787_RECOVER_TOOL=/path/to/ns-link/tools/h787_recover_flash.py
```

它烧的就是 `out/dist/bootloader.bin`。构建中间产物（`.elf`/`.hex`/`.map`）在
`out/bootloader/CM7/`，要调试或手工烧就自己去那里拿。

## WSL 里找不到 MSC 设备

板子的 USB MSC（`19f5:5780`）和 CMSIS-DAP 探针（`19f5:3106`）是两个 USB 设备。
MSC 常常在 Windows 里是 `Shared` 但没有 attach 给 WSL，这时 `msc_image.py` 会报
`N32H7xx MSC device not found`：

```sh
powershell.exe -NoProfile -Command "usbipd list"
powershell.exe -NoProfile -Command "usbipd list"        # 先拿到当前 busid
powershell.exe -NoProfile -Command "usbipd attach --wsl --busid <busid>"
```

不要用 `pkill -f openocd`：`-f` 会匹配到调用者自己的命令行，把当前 shell 一起
杀掉。用 `pgrep -x openocd` 拿 PID 再 kill。

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

`recover-halt` 是核停在 boot ROM 或异常状态时的恢复路径：先清 DP SELECT，再让
NSLink 发 CMSIS-DAP 的 `DAP_ResetTarget`（`cmsis-dap cmd 0x0a`，PB0/NRST 拉低
20 ms），等待、examine、halt。延迟必须小于板子约 1.5 s 的恢复窗口。出现
`PARTNO 0xfff` 时用的就是这条命令。

Tcl-RPC 会把命令失败当成普通文本返回，所以 wrapper 会大小写不敏感地匹配已知的
reset/halt/读内存失败字样并返回非零，不会把失败报成成功。

`serial_console.py` 直接用 Linux TTY，不需要 `pyserial`：

```sh
tools/h787ctl serial capture --seconds 15 -o /tmp/console.log
tools/h787ctl serial send 'uname -a'
tools/h787ctl serial expect 'login:' --timeout 20
```

NSLink 虚拟串口在板子->主机方向会丢字节，长内核日志会花掉，但发往板子的命令是
正确的。丢字节在 WSL/usbipd 的 CDC-ACM 接收路径上，不在内核 USART 驱动里。
同一时间只留一个串口读者——Windows 终端和 WSL 里的 `minicom` 会抢同一个口。

## 环境变量

`tools/env.sh` 由 `h787ctl` 自动 source，也可以在仓库根目录手动 source：

```sh
H787_ROOT=$PWD . ./tools/env.sh
```

常用的有 `H787_ROOT`、`H787_LINUX_ROOT`、`H787_SERIAL`、`H787_SERIAL_BAUD`、
`H787_OPENOCD_CFG`、`H787_OPENOCD_TCL_PORT`、`H787_DAP_SERIAL`。
