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
  `timeout`（no-MMU 上本树已修，退出码是 143 不是 124，见
  [SDK 文档](doc/sdk.md#智能体--逻辑脚本常要的东西)）
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
| `bootloader/` | CM7 bootloader |
| `kernel/` | Linux 内核，Git 子模块 |
| `rootfs/` | 根文件系统文件树 |
| `sdk/toolchain/` | 用户态交叉工具链 |
| `sdk/example/` | 应用示例及 Makefile |
| `sdk/optional/` | 可选组件的预编译程序和使用说明 |
| `sdk/optional-src/` | 可选开源组件的移植代码、补丁和构建说明 |
| `tools/` | 主机侧构建与烧写工具 |
| `doc/` | 用户文档 |
| `licenses/` | 许可证、版权声明和组件信息 |
| `out/` | 构建产物 |

## 可选组件

以下组件随 SDK 提供，**不参与默认构建，也不进入固件镜像**。

| 组件 | 用途 | 板上安装位置 |
| --- | --- | --- |
| [espeak-ng](sdk/optional/espeak-ng/README.md) | 英文、中文语音合成与播放 | `/system/espeak-ng/` |
| [ns-flash](sdk/optional/ns-flash/README.md) | 通过外接 NSLink 探针识别、烧写目标芯片 | `/system/bin/ns-flash` |

在电脑的项目根目录执行，选择一种安装方式：

```sh
# ADB：连接开发板 USB1
sh sdk/optional/install.sh

# SSH：连接网线，并填写板子的实际 IP
BOARD=<板子IP> USE=ssh sh sdk/optional/install.sh
```

在板上执行 `ifconfig eth0` 查看 IP。组件写入 `/system` 的 JFFS2 分区，
传输速度比写入内存慢，安装时请等待命令完成。

ns-flash 需要 USB Host；HID 探针还需要内核启用 `CONFIG_HIDRAW`。
组件的运行条件、参数和内存要求见各自说明。

espeak-ng 及其 pcaudiolib 音频层的移植代码、补丁和词典重建说明位于
[可选组件源码](sdk/optional-src/README.md)。完整对应源码还需包含上游源码、
数据源、链接依赖和构建材料，详见下文许可证说明。

## 文档导航

首次使用按 **环境搭建 → 获取源码 → 快速上手** 的顺序进行，之后按任务查阅。

| 文档 | 内容 |
| --- | --- |
| [环境搭建](doc/setup.md) | Windows / WSL2、依赖安装和 USB 共享 |
| [获取源码](#获取源码) | 克隆仓库及内核子模块 |
| [快速上手](doc/quickstart.md) | 编译、刷机、登录和运行示例 |
| [主机工具与烧写](doc/tools.md) | USB MSC、bootloader 烧写、串口和 OpenOCD |
| [网络 OTA](doc/ota-update.md) | 通过 SSH 更新 kernel / dtb / rootfs |
| [内核配置](doc/kernel-config.md) | 修改、保留、重置和导出配置 |
| [SDK 与应用开发](doc/sdk.md) | 工具链、库、部署、自启和应用示例 |
| [USB](doc/usb.md) | ADB、ACM、Host 存储及角色切换 |
| [摄像头](doc/camera.md) | OV5640 拍照和 MJPEG 采集 |
| [音频](doc/audio.md) | WM8978 录放音和 TinyALSA 工具 |

## Licensing

### 本项目

根目录 [LICENSE](LICENSE) 为 **BSD-3-Clause**，适用于采用该许可的本项目自有代码。
各文件及第三方组件原有的版权、许可和 NOTICE 保持有效。

Nations / NSING 为同一家公司；`sdk/example/` 为自有示例源码，其本地编译产物
不属于发行内容。可选 ns-flash 为公司自有闭源程序，随 SDK 提供不表示授予开源许可。

### 第三方组件

| 组件 | 许可证 |
| --- | --- |
| Linux 内核 | GPL-2.0-only，另遵循逐文件声明 |
| BusyBox | GPL-2.0-only |
| uClibc-ng | LGPL 及逐文件声明 |
| GCC 运行库 | GPL-3.0-or-later WITH GCC-exception-3.1 |
| espeak-ng / pcaudiolib | GPL-3.0-or-later；语音数据另保留其来源声明 |

完整组件、版本、来源和版权信息见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。SDK 工具链的许可证全文位于
`sdk/toolchain/licenses/`。

### 二进制发行

发行 SDK 或固件时，应提供与二进制版本对应的许可证材料，并按适用的 GPL/LGPL
方式交付完整对应源码，包括修改、配置及控制构建和安装的脚本。仅附上游链接或
移植补丁不足以替代完整源码；LGPL 还需满足适用的库替换、重链接和调试要求。

Linux 的 UAPI syscall 例外不免除内核本身的源码义务；GCC 运行库例外也不免除
运行库本身的源码义务。许可证与源码作为独立发行附件提供，不增加固件镜像体积。

审阅发行报告中的 WARNING/ERROR 后再发布。中文词典的 CC-CEDICT 历史导入版本
仍未确认，不能以 espeak-ng 的整体许可代替这项数据来源核查。
详细材料见[审计记录](licenses/AUDIT.md)、[发行工作流](licenses/README.md)
及[证据与遗留项](licenses/RESOLUTIONS.md)。
