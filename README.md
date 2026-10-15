# N32H7xx Linux 实验项目

本项目用于探索和验证 NSING N32H7xx/N32H787 上的 Linux，包含 bootloader、
内核、rootfs 和用户态 SDK。

**这是实验项目**，面向学习、开发与功能验证。功能和接口可能调整，尚未经过完整的
产品级稳定性、可靠性与安全验证；用于实际产品前，需要自行完成相应验证。

## 开始使用

首次使用请先阅读 [环境搭建](doc/setup.md)，配置 Windows / WSL2、构建依赖和
USB 工具；完成后按 [快速上手](doc/quickstart.md) 编译、烧录并登录开发板。

## 功能清单

当前功能如下。USB 各角色的使用方法见 [doc/usb.md](doc/usb.md)，音频见
[doc/audio.md](doc/audio.md)。

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
| GPIO / pinctrl | 11 个 bank、176 pin；`/sys/class/gpio` + `/dev/gpiochipN`，脚本用 `/bin/gpio` |
| Ethernet | NET1001A / JL1111 PHY，100 Mbps |
| SD/MMC | SD 卡（已测 128 GB SDXC） |
| SPI NOR | XSPI2，16 MiB，MTD 四分区 |
| RTC | LSE 时钟，无电池，开机 NTP 校时 |
| I2C4 | 用户态 `/dev/i2c-4`（`i2cdetect` / `i2cget` / `i2cset`） |
| 摄像头 | OV5640 / DVP1，V4L2；JPEG 最高 2592x1944，GREY / YUYV / RGB565，见 [摄像头使用](doc/camera.md) |
| 音频 | WM8978 codec（I2C + I2S），ALSA 卡 0；8000..48000 Hz 七档、双声道，录音与播放可同时开，见 [音频使用](doc/audio.md) |
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

- BusyBox 1.38（168 个 applet）：shell、`vi`、`top`、`ip`、`fdisk`、`flashcp`、
  `lsusb`、`hexdump`、`xxd`；脚本常用的 `awk` / `sed` / `grep` / `find`（含
  `-maxdepth` / `-type` / `-size` / `-exec`）/ `xargs` / `expr` / `seq` / `cut` /
  `sort` / `uniq` / `diff` / `md5sum` / `base64` / `patch` / `nc` / `nslookup`；
  后台与进程管理 `nohup` / `setsid` / `pgrep` / `pkill` / `flock` / `crond`；
  `timeout`（no-MMU 上本树已修，见下）
- `/bin/gpio`：`/sys/class/gpio` 脚本封装，按 `gpiochipN/label` 反查基号，
  用引脚名（`PB3`、`PF10`）而不是裸数字，见 [SDK 文档](doc/sdk.md#用-gpio-脚本操作引脚)
- ALSA：TinyALSA 的 `tinycap` / `tinyplay` / `tinymix` / `tinypcminfo`
- USB ADB：交互 shell、文件 push/pull；默认目录 `/system`，开机自动启动
- `n32-flash`：分区擦写 / 回读校验 / 网络 OTA

## 获取源码

`kernel/` 是 Git 子模块，以下命令会一并拉取：

```sh
git clone --recurse-submodules https://github.com/NSING-Community/n32h7xx_linux.git \
    ~/research/n32h7xx_linux
cd ~/research/n32h7xx_linux
```

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

`H787_BOARD` 没有默认值，必须指定。板子走 DHCP，MAC 由芯片 ID 派生、每块板固定，
所以地址在同一网络里基本不变，但镜像里不写死，用板上的 `ifconfig eth0` 读。

USB MSC 的接线、进入方式和读回校验见 [主机工具与烧写](doc/tools.md)，
SSH 更新过程见 [网络 OTA](doc/ota-update.md)。

更换更大的 NOR 时使用 `make FLASH_SIZE=32M`，`system` 占用新增容量；16 MiB
是默认最小档。当前 bootloader 的 MSC 按 16 MiB 上报，大容量板仍可通过 MSC
更新 `spi-nor-boot.img`，整片大容量镜像需要能访问完整 NOR 的烧写方式。

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
| `sdk/optional/` | 可选附加组件（预编译二进制），默认不编译、不进任何镜像，自行取用 |
| `sdk/optional-src/` | 上面这些组件的构建源码，供 GPL 对应源码要求，同样不参与构建 |
| `doc/` | 用户文档 |
| `out/` | 全部构建产物 |

## 文档导航

首次使用按 **环境搭建 → 获取源码 → 快速上手** 的顺序进行，之后按任务查阅专题。

| 文档 | 内容 |
| --- | --- |
| [环境搭建](doc/setup.md) | Windows / WSL2、依赖安装、USB 共享及权限 |
| [获取源码](#获取源码) | 克隆、内核子模块及源码更新 |
| [快速上手](doc/quickstart.md) | 首次编译、USB 刷机、登录、板级默认配置及运行 hello |
| [主机工具与烧写](doc/tools.md) | MSC 更新、bootloader 烧写、串口和 OpenOCD |
| [网络 OTA](doc/ota-update.md) | 通过 SSH 更新 kernel / dtb / rootfs |
| [内核配置](doc/kernel-config.md) | menuconfig、配置保留、重置和导出 |
| [SDK 与应用开发](doc/sdk.md) | 工具链、库、部署、自启、LED 和网络示例 |
| [USB](doc/usb.md) | ADB、ACM、Host 存储及 Device/Host 切换 |
| [摄像头](doc/camera.md) | OV5640 采集格式、MJPEG 和板级时钟配置 |
| [音频](doc/audio.md) | WM8978 和 TinyALSA 工具 |

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

`sdk/optional/` 是可选附加组件，只是随包提供、不参与默认构建：`make` 与
`make flash-image` 都不引用它，镜像里也不含它的内容。目前有两个：
[espeak-ng](sdk/optional/espeak-ng/README.md)（GPL-3.0-or-later，内含同许可的
pcaudiolib 音频输出层），按 GPL 要求单独发放；以及
[ns-flash](sdk/optional/ns-flash/README.md)，板子作 USB host 驱动外接 NSLink
探针烧写目标板用的命令行工具（需要内核打开 USB host 和对应传输：bulk 探针走
usbfs，HID 探针走 `CONFIG_HIDRAW`）。
`sdk/optional/install.sh` 是把这些组件装到 `/system` 的一键脚本：

```sh
sh install.sh                    # ssh，默认 192.168.1.85
BOARD=192.168.1.86 sh install.sh
USE=adb sh install.sh            # 走 adb
```

目录名在 `/system` 下保持不变，所以 espeak-ng 装完就是 `/system/espeak-ng/`，
拷过去的 273 个文件与下载件逐字节相同。ns-flash 是单个文件而不是目录，直接进
`/system/bin/`。两条路径都不需要 `mkdir` 或 `chmod`，可执行位随文件一起过去。
写 `/system` 是写 SPI NOR 上的 JFFS2，比写 `/tmp` 慢一个数量级，装 espeak-ng
等半分钟属正常，见 [USB 文档](doc/usb.md#adb-的目录推送)。

`sdk/optional-src/` 是这些组件的构建源码：espeak-ng 的移植 glue（Makefile、
`config.h`、`compat/`，见 [sdk/optional-src/espeak-ng/](sdk/optional-src/espeak-ng/)）
和 pcaudiolib 的源码（新写的 TinyALSA 后端 `tinyalsa.c` 及三处改动的补丁，见
[sdk/optional-src/pcaudiolib/](sdk/optional-src/pcaudiolib/)）。上游 espeak-ng 未做
任何修改；pcaudiolib 是另一个 GPL-3 工程，为了接到板上的 TinyALSA 而打了补丁，
所以补丁本身也必须随源码发出。二进制发行还需按适用的 GPLv3 第 6 节方式提供完整
对应源码、数据源和构建材料；仅指向上游仓库或只附移植 glue 不足以证明已履行义务。
它同样不被 `make` 引用，也不进任何镜像。

## Licensing

根目录 [LICENSE](LICENSE) 保持 **BSD-3-Clause**，适用于本工程自有、采用该许可的
构建工具、脚本等材料；它不覆盖 Linux 或第三方组件原有版权和许可证，也不撤销
文件已有的其他授权。Nations / NSING 是同一家公司，自有代码保留原声明；可选
`ns-flash` 是公司自有闭源二进制，不因放入 SDK 而授予开源许可。
`sdk/example/` 是公司自写的示例源码；本地编译出的程序、对象和库不发布，也不作为
发行二进制纳入许可证审计清单。

Linux 内核遵循自身 `COPYING` 和逐文件许可，整体为 **GPL-2.0-only**。
UAPI 的 `Linux-syscall-note` 不免除分发内核二进制时的源码义务。BusyBox 为
GPL-2.0-only；uClibc-ng 遵循 LGPL 及逐文件声明；GCC 运行库采用
GPL-3.0-or-later WITH GCC-exception-3.1，运行库例外不免除运行库本身的源码义务。
完整组件、版本、来源、声明和未知项见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

许可证材料作为 SDK、固件的独立发行附件提供，不添加到 rootfs、SquashFS、
Bootloader 或 SPI NOR 镜像。SDK 现有许可证全文位于
`sdk/toolchain/licenses/`；组件特有版权和 NOTICE 同样保留。

发行 SDK 或固件时，将许可证包与经核实的完整对应源码放在同一发布页，明确关联
二进制版本，并按适用许可证提供下载或其他有效交付方式。GPL/LGPL 源码应包含实际
修改、配置和控制构建/安装的脚本；LGPL 还需满足适用的替换、重链接和调试要求。
只提供上游链接或移植补丁，不能替代完整对应源码的交付。
报告中的 WARNING/ERROR 不得当作已确认信息，源码快照也不自动证明它与二进制对应。
详见 [审计记录](licenses/AUDIT.md) 和 [发行工作流](licenses/README.md)。

已核实的构建来源、uClibc 源码版本修正和中文词典重建记录见
[证据与遗留项](licenses/RESOLUTIONS.md)。报告会校验已审阅输入和源码摘要；后续更换
二进制或修改源码时须重新核实。中文词典上游的 CC-CEDICT 历史导入版本尚未查明，
仍明确标为 WARNING，不能用 espeak-ng 的整体许可证代替这项数据来源核查。
