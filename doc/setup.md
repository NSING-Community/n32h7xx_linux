# 开发环境搭建（Windows + WSL2）

本工程在 Linux 下构建和调试。Windows 上的推荐做法是 **WSL2 + Ubuntu**：构建、
OpenOCD、串口和 MSC 都在同一个 Linux 环境里，路径脚本（`tools/*.py`、
`*/scripts/*.sh`）不用为 Windows 改写，也能直接访问 `/dev/ttyACM*` 和块设备。

原生 Windows 也能用，但本工程的所有入口脚本都是 POSIX shell / Python，只在 WSL
下保证可用。下面只讲 WSL 路线。

## 1. 装 WSL2

Windows 10 2004+ / Windows 11，管理员 PowerShell：

```powershell
wsl --install -d Ubuntu
```

装完重启，进 Ubuntu 建好普通用户。确认版本：

```sh
wsl -l -v          # 输出里 VERSION 要是 2
```

项目放在 WSL 自己的文件系统里（例如 `~/research/`），**不要放在 `/mnt/c/...`**：
NTFS 上的 ext4 语义、大小写、文件权限（脚本的可执行位）都不可靠，Linux 内核编译
在 `/mnt/c` 下也慢很多。

## 2. Windows 侧：安装 usbipd

板子的两个 USB 设备要从 Windows 透传给 WSL：调试探针（CMSIS-DAP，兼作串口）和
板子自己的 U 盘（MSC）。这一步用 [usbipd-win](https://github.com/dorssel/usbipd-win)。

在管理员 PowerShell 里安装（任选其一）：

```powershell
winget install --interactive --exact dorssel.usbipd-win
# 或到 GitHub releases 下载 .msi 安装
```

装完在 PowerShell 里能看到设备列表：

```powershell
usbipd list
```

会看到两类设备，注意 VID:PID：

| 设备 | VID:PID | 用途 |
| --- | --- | --- |
| NSLink / CMSIS-DAP | `19f5:3106` | OpenOCD 调试 + 虚拟串口控制台 |
| 板子自己的 MSC | `19f5:5780` | 刷 dtb / kernel / rootfs（只在 MSC 模式出现） |

首次使用要把设备共享出来（`bind` 只需做一次，之后一直有效，需要管理员权限）：

```powershell
usbipd list                                  # 先看清当前 busid
usbipd bind --busid <探针的 busid>
```

之后每次要在 WSL 里用，就 `attach`（**不需要**管理员权限）：

```powershell
usbipd attach --wsl --busid <探针的 busid>
```

用 WSL 里的 `dmesg | tail` 或 `ls /dev/ttyACM*` 确认挂上了。用完可以还给 Windows：

```powershell
usbipd detach --busid <探针的 busid>
```

板子进 MSC 模式时会重新枚举出一个新设备，插件/拔插之后 busid 可能变；刷机时如果
不想每次手动 `attach`，可以加 `--auto-attach` 让它自动挂上：

```powershell
usbipd attach --wsl --busid <MSC 的 busid> --auto-attach
```

注意 `busid` 会随插拔变化，一律以 `usbipd list` 的当前输出为准，别写死。

`attach` 之后设备就归 WSL 了，Windows 上原来的串口（设备管理器里的 COMx）会消失，
这是正常的。

## 3. WSL 侧：装编译与调试工具

```sh
sudo apt update
sudo apt install -y \
    build-essential git python3 \
    gcc-arm-none-eabi \
    openocd \
    squashfs-tools \
    flex bison bc libelf-dev libssl-dev cpio \
    device-tree-compiler \
    libncurses-dev
```

各自的作用：

| 包 | 用途 |
| --- | --- |
| `build-essential`, `flex`, `bison`, `bc`, `libelf-dev`, `libssl-dev`, `cpio`, `device-tree-compiler` | 编 Linux 内核和 dtb |
| `gcc-arm-none-eabi` | 编 CM7 bootloader |
| `openocd` | 调试探针、烧片内 flash、读内核日志（需要 0.12 或更新） |
| `squashfs-tools` | 打 SquashFS rootfs 镜像（提供 `mksquashfs`） |
| `libncurses-dev` | `make menuconfig` 的内核配置界面（纯编译不装也行） |
| `python3` | `tools/` 下所有脚本（只用标准库，不需要 pip 装东西） |

**根文件系统和应用的交叉工具链不用自己装**，仓库里自带了：

| 编译器 | 在哪 | 编什么 |
| --- | --- | --- |
| `arm-none-eabi-gcc` | 系统 apt 装的 | CM7 bootloader |
| `arm-n32-linux-kernel-gcc` | `sdk/toolchain/bin/` | Linux 内核（软浮点包装） |
| `arm-uclinuxfdpiceabi-gcc` | `sdk/toolchain/bin/` | 用户态应用（uClibc，FDPIC 动态链接） |

所以只需要额外装系统级的 `arm-none-eabi-gcc`，其余都在 `sdk/toolchain/` 里。

检查一遍：

```sh
arm-none-eabi-gcc --version
openocd --version
python3 --version
```

## 4. 拿到源码

本工程由几部分组成，`kernel/` 是独立的 Git 仓库，作为 **submodule** 引用
[NSING-Community/linux](https://github.com/NSING-Community/linux.git) 的
`n32h7xx` 分支（上游 Linux 加上 NSING 的改动）。完整取一份：

```sh
git clone --recurse-submodules https://github.com/NSING-Community/n32h7xx_linux.git \
    ~/research/n32h7xx_linux
cd ~/research/n32h7xx_linux
```

内核默认是 **depth 1** 的浅克隆（`.gitmodules` 里给 kernel 设了
`shallow = true`）：完整的 Linux 历史约 10 GiB，构建用不到。确实要完整历史：

```sh
git -C kernel fetch --unshallow
```

已经 clone 过、`kernel/` 是空目录的话（忘了 `--recurse-submodules`），补一次：

```sh
git submodule update --init
```

拉完主工程的新提交之后，也要让 kernel 跟上主工程记录的那个提交：

```sh
git pull && git submodule update --init
```

完整说明（默认深度、如何加深、改用 SSH）见根 `README.md` 的"获取源码"。

目录结构见根 `README.md`。

## 5. 编译

顶层 `make` 是统一入口，产物全在 `out/`：

```sh
cd ~/research/n32h7xx_linux

make              # 全部：bootloader + kernel + rootfs + 整片 SPI NOR 镜像

make help         # 看所有目标和可覆盖变量
```

单编某一块：

```sh
make bootloader   # out/bootloader/CM7/h787_linux_CM7.bin，同时拷成 out/dist/bootloader.bin
make kernel       # out/kernel/{zImage,n32h787-hmi.dtb}
make rootfs-squashfs   # out/rootfs/rootfs.squashfs
```

改内核配置（用 ncurses 菜单定制，改完 `make kernel` 会保留你的改动）：

```sh
make menuconfig
```

细节见 [kernel-config.md](kernel-config.md)。

编应用（SDK 自带工具链，路径自动推导）：

```sh
cd sdk/example && make
```

## 6. 连板子自检

先 `usbipd attach` 探针，再跑 doctor：

```sh
cd ~/research/n32h7xx_linux
tools/h787ctl doctor
```

它会打印所有路径、检查 `make`/`python3`/`arm-none-eabi-gcc`/`openocd` 是否就位、
列出 `/dev/serial/by-id/` 里的串口、当前 MSC 设备和 OpenOCD target 状态。
全部 OK 才算环境搭好。

探针的虚拟串口用 `/dev/serial/by-id/` 里的稳定名字访问，不要写死 `/dev/ttyACM0`：

```sh
ls /dev/serial/by-id/
tools/h787ctl serial capture --seconds 15 -o /tmp/h787-console.log
```

如果电脑上同时开着 Windows 的串口工具（或者 WSL 里的 `minicom`），两边会抢同一个
端口，表现是板子"掉字节"。**同一时间只留一个串口读者**。

`/dev/ttyACM*` 打不开就看看是不是权限问题，把自己加进 `dialout` 组再重新登录：

```sh
sudo usermod -aG dialout "$USER"
```

## 7. 烧写

三种更新方式，用的线不同：

| 更新内容 | 命令 | 线 |
| --- | --- | --- |
| rootfs / kernel / dtb（单分区） | `make ota-rootfs` / `ota-kernel` / `ota-dtb` | 网线 |
| dtb / kernel / rootfs（整片） | `make flash-kernel` | USB（板子自己的 MSC） |
| bootloader（片内 flash） | `make flash-bootloader` | CMSIS-DAP 探针 |

走 USB 刷内核时会用到 MSC，一条典型流程：

```sh
make flash-image                        # 生成 out/dist/spi-nor-boot.img（make 已做过）
tools/h787ctl msc-enter                 # 让板子进 MSC 模式（或按 KEY1 / 板上 reboot msc）
# Windows PowerShell：usbipd attach --wsl --busid <MSC 的 busid>
sudo tools/h787ctl msc-write out/dist/spi-nor-boot.img
# 写完 usbipd detach，再 h787ctl openocd reset-run 或断电重上电
```

写 MSC 要 `sudo`（WSL 普通用户没有块设备写权限）。

烧片内 flash 的恢复工具在单独的 NSLink 仓库，路径机器相关，用
`H787_RECOVER_TOOL=` 指过去：

```sh
make flash-bootloader H787_RECOVER_TOOL=/path/to/ns-link/tools/h787_recover_flash.py
```

分区的详细布局和每种更新的注意事项见 [ota-update.md](ota-update.md)。

## 8. 常用路径与环境变量

`tools/env.sh` 会自动推导这些；要覆盖就 `export` 之后再跑脚本：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `H787_SERIAL` | 空（自动找 `/dev/serial/by-id/*CMSIS-DAP*`） | 控制台串口；多个候选时必须显式指定 |
| `H787_SERIAL_BAUD` | `115200` | 波特率 |
| `H787_DAP_SERIAL` | 空 | CMSIS-DAP 序列号；留空让 OpenOCD 自己找唯一探针，插了多个才需要指定 |
| `H787_BOARD` | 空（必须指定） | `make ota-rootfs` 的 SSH 目标，如 `root@192.168.1.85` |
| `H787_OUT_DIR` | `<项目>/out` | 构建产物根 |
| `H787_OPENOCD_TCL_PORT` | `6666` | OpenOCD Tcl-RPC 端口 |

工具入口都在顶层 `tools/` 下，日常用 `tools/h787ctl`，详细命令见
[tools.md](tools.md)。

## 9. 常见问题

- **串口掉字节**：多半是同时开了两个串口读者（Windows 工具或 `minicom`）。同一
  时间只留一个，用 `tools/serial_console.py` 独占串口抓取。
- **OpenOCD 报 `partnum 0xfff` 或 halt 超时**：核停在 boot ROM 或异常状态。用
  `tools/h787ctl openocd recover-halt 700`（会先清 DP SELECT 再拉 NRST）。
  仍不行就断电重上电。
- **`AP0 still disabled` / `Target not examined yet`**：这类通常只有断电重上电
  才能恢复。别反复 reset。
- **找不到设备**：`usbipd list` 看 busid，重新 `attach`；设备重新枚举后 busid
  会变。
- **`make kernel` 报源码树不干净**：内核是 out-of-tree 构建，源树里不能有
  `.config`、`include/config/`、`arch/arm/include/generated`。跑一次
  `make -C kernel mrproper`。

## 10. 下一步

- 上电、进终端、编第一个应用：[quickstart.md](quickstart.md)
- 定制内核配置：[kernel-config.md](kernel-config.md)
- 网络更新分区：[ota-update.md](ota-update.md)
- 应用构建与 SDK 布局：[sdk.md](sdk.md)
