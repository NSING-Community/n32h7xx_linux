# N32H7xx Linux 主工作区

NSING N32H7xx/N32H787 的 Linux、bootloader、rootfs 和用户态 SDK 统一入口。

## 功能清单

以下都在 N32H787 HMI 实板上跑通。USB 使用及角色切换方法见
[doc/usb.md](doc/usb.md)。

**系统**

- N32H787 上的 ARMv7-M NOMMU Linux，CM7 单核（CM4 保持复位）
- CM7 bootloader：从 SPI NOR 启动内核，自身可作 U 盘刷机
- 32 MiB SDRAM、I/D-cache、CM7 600 MHz、SDRAM 133 MHz
- 高精度定时 / tickless idle / 抢占内核，SysTick + BTIM1
- 用户态 fault 只杀进程不挂整机，FPU/VFP
- FDPIC/FLAT 用户态，`devtmpfs` / `proc` / `sysfs`
- 内核配置客制化：`make menuconfig`，见 [doc/kernel-config.md](doc/kernel-config.md)

**驱动**

| 驱动 | 说明 |
| --- | --- |
| USART1 | 控制台 `ttyS0` |
| GPIO / pinctrl | 11 个 bank、176 pin；`/sys/class/gpio` + `/dev/gpiochipN` |
| Ethernet | NET1001A / JL1111 PHY，100 Mbps |
| SD/MMC | SD 卡（已测 128 GB SDXC） |
| SPI NOR | XSPI2，16 MiB，MTD 四分区 |
| RTC | LSE 时钟，无电池，开机 NTP 校时 |
| USB1 / USB2 | DWC2 HS Host / Device；USB1 默认 ADB，USB2 默认 Host；支持 DTB 配置及运行时角色切换，USB2 VBUS 自动控制 |

**文件系统**

- SquashFS 只读根 `/`
- JFFS2 可写 `/system`
- VFAT / exFAT（SD 卡、U 盘）
- ramfs（`/tmp`、`/run`、`/var/log`）

**网络与远程登录**

- IPv4：DHCP 或静态；IPv6：双协议栈，SLAAC 或 DHCPv6
- SSH（dropbear）、SCP、SFTP、Telnet，开发板默认 root 免密
- NTP 自动校时
- 插卡 / 插盘自动挂载（mdev）

**用户态**

- BusyBox 1.38（shell、`vi`、`top`、`ip`、`fdisk`、`flashcp` 等）
- USB ADB：交互 shell、文件 push/pull；默认目录 `/system`，开机自动启动
- `n32-flash`：分区擦写 / 回读校验 / 网络 OTA

## 获取源码

`kernel/` 是 git submodule，其余目录都在本仓库。

```sh
git clone --recurse-submodules https://github.com/NSING-Community/n32h7xx_linux.git \
    ~/research/n32h7xx_linux
cd ~/research/n32h7xx_linux
```

上面一条命令已经带上了 `kernel/`，不需要再单独 init。漏了 `--recurse-submodules`、
或者从压缩包解出来时 `kernel/` 会是空的，补一次：

```sh
git submodule update --init
```

更新主工程后，让 kernel 跟上记录的提交：

```sh
git pull --recurse-submodules
```

仓库公开，HTTPS 匿名即可拉；要走 SSH：

```sh
git config submodule.kernel.url git@github.com:NSING-Community/linux.git
git submodule sync && git submodule update --init
```

需要完整内核历史（默认 depth 1，约 250 MB），或让 kernel 跟分支最新：

```sh
git -C kernel fetch --unshallow
git submodule update --remote kernel
```

kernel 分支 `n32h7xx`，仓库
[NSING-Community/linux](https://github.com/NSING-Community/linux.git)。

## 构建

```sh
make              # 全部：bootloader + kernel + rootfs + 整片 SPI NOR 镜像
make bootloader   # CM7 bootloader
make kernel       # zImage + <BOARD>.dtb
make menuconfig   # 改内核配置（改完 make kernel 保留）
make rootfs       # 打包 SquashFS rootfs
make flash-image  # 组装 SPI NOR 镜像
make help         # 全部目标和可覆盖变量
```

内核配置在 `out/kernel/build/.config`；`kernel-nconfig` / `kernel-oldconfig` /
`kernel-savedefconfig` / `kernel-defconfig` 见 [doc/kernel-config.md](doc/kernel-config.md)。

## 产物

全部集中在 `out/`：

```text
out/dist/spi-nor-16M.img        # 整片 SPI NOR 镜像 + .sha256
out/dist/spi-nor-boot.img       # dtb+kernel+rootfs（0x710000 以下，不动 system）
out/dist/bootloader.bin         # CM7 bootloader（烧片内 flash）
out/kernel/{zImage,n32h787-hmi.dtb,System.map,config,SHA256SUMS}
out/rootfs/rootfs.squashfs      # 只读 rootfs
out/bootloader/CM7/             # bootloader 构建产物
out/recover-<时间戳>/           # make flash-bootloader 的烧写备份
```

## 分区与更新

设备上有两块存储器：片内 flash（约 2 MiB，放 bootloader，只能探针烧）和外部
SPI NOR（16 MiB，放 dtb / kernel / rootfs / system）。

外部 SPI NOR 固定四分区：

| 分区 | 偏移 | 大小 | 挂载点 | 运行时能否写 |
| --- | --- | --- | --- | --- |
| `dtb` | `0x000000` | 64 KiB | 不挂载 | 是（重启生效） |
| `kernel` | `0x010000` | 4 MiB | 不挂载 | 是（重启生效） |
| `rootfs` | `0x410000` | 3 MiB | `/`（SquashFS，只读） | 是（整分区） |
| `system` | `0x710000` | 其余（≥8 MiB） | `/system`（JFFS2，读写） | 是 |

kernel 命令行：

```text
earlycon console=ttyS0,115200 root=/dev/mtdblock2 rootfstype=squashfs ro rootwait
```

想更新什么、用什么方式、要插什么线：

| 想更新 | 用什么 | 要插什么线 |
| --- | --- | --- |
| 应用程序、库 | `scp` 到 `/system/bin`、`/system/lib` | 网线 |
| dtb / kernel / rootfs | `make ota-*` | 网线 |
| dtb + kernel + rootfs 一起 | `make flash-kernel`（USB MSC） | USB |
| bootloader | `make flash-bootloader` | CMSIS-DAP |

```sh
H787_BOARD=root@<板子IP> make ota-rootfs   # 更新 rootfs
H787_BOARD=root@<板子IP> make ota-kernel   # 更新 kernel
H787_BOARD=root@<板子IP> make ota-dtb      # 更新 dtb
make flash-kernel                          # USB MSC 写 dtb+kernel+rootfs
make flash-bootloader                      # 探针写 CM7 bootloader
```

`H787_BOARD` 没有默认值，必须指定（板子走 DHCP，地址会变）。

进 USB MSC：上电/复位时按住 KEY1（`PG3`，低有效），调试器写请求标志
`0x30015f00`（magic `0x544f4f42` 加它的取反），或 Linux 里 `reboot msc`。

换更大的 NOR：`make FLASH_SIZE=32M`，`system` 自动吃掉多出来的空间；16 MiB 是
最小档。大于 16 MiB 的板子走 MSC 时按 16 MiB 上报容量，写整片要用
`--expect-bytes`（或 `H787_MSC_BYTES`）对上。

完整说明见 [doc/ota-update.md](doc/ota-update.md)、[doc/tools.md](doc/tools.md)。

## 常用变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `JOBS` | `nproc` | 并行 job 数 |
| `BOOT_STORAGE` | `spi` | 改成 `sdmmc` 切到 SDMMC 后端 |
| `BOARD` | `n32h787-hmi` | 板名，决定 dtb 文件名（`<BOARD>.dtb`） |
| `DEFCONFIG` | `nsing_n32h7xx_defconfig` | 内核配置种子 |
| `CROSS_COMPILE` | 空（退回 `arm-none-eabi-`） | 内核交叉前缀；SDK 提供 `arm-n32-linux-kernel-` |
| `ROOTFS_TREE` | `rootfs` | rootfs 输入目录 |
| `OUT_DIR` | `out` | 所有构建产物的输出根 |
| `FLASH_SIZE` | `16M` | SPI NOR 容量；`system` = `FLASH_SIZE - 0x710000` |
| `MIN_SYSTEM` | `8M` | system 分区下限 |
| `H787_BOARD` | 无（必须指定） | `make ota-*` 的网络目标 |
| `H787_RECOVER_TOOL` | 无 | 烧片内 flash 用的 NSLink 恢复工具路径 |
| `H787_DAP_SERIAL` | 空 | 只连一个 CMSIS-DAP 时可留空 |

## 目录

| 路径 | 内容 |
| --- | --- |
| `Makefile` | 顶层构建入口 |
| `bootloader/` | CM7 bootloader 固件；可 `make -C bootloader` 单独编 |
| `tools/` | 主机侧工具（客户用），见 [doc/tools.md](doc/tools.md) |
| `kernel/` | 内核，git submodule |
| `rootfs/` | 交付 rootfs 文件树，只打包不编译 |
| `sdk/toolchain/` | 交叉工具链，见 [doc/sdk.md](doc/sdk.md) |
| `sdk/example/` | 应用示例 + Makefile（工具链路径相对推导） |
| `doc/` | 用户文档 |
| `out/` | 全部构建产物 |

用 SDK 编译应用：

```sh
cd sdk/example && make
sdk/toolchain/bin/arm-uclinuxfdpiceabi-gcc -O2 sdk/example/hello.c -o /tmp/hello
```

## 文档

`doc/setup.md` 搭 Windows + WSL2 开发环境、`doc/quickstart.md` 上手、
`doc/kernel-config.md` 内核配置、`doc/ota-update.md` 网络更新分区、
`doc/sdk.md` 应用构建、`doc/tools.md` 主机工具与烧写、`doc/usb.md` USB 角色切换及 ADB。

## 发布范围

```text
本工程    https://github.com/NSING-Community/n32h7xx_linux
kernel    https://github.com/NSING-Community/linux   (n32h7xx 分支)
```

本工程发布的目录：

```text
.gitmodules  LICENSE  Makefile  README.md  doc/  tools/  bootloader/  kernel/  rootfs/  sdk/
```

`kernel/` 以 gitlink + `.gitmodules` 引用，不复制源码。

## 许可

本工程采用 BSD 3-Clause License，见 [LICENSE](LICENSE)。`kernel/` 内的内核按其自身许可发布。
