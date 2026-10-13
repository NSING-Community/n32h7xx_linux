# 开发环境搭建（Windows + WSL2）

本工程在 Linux 下构建和调试。Windows 上的推荐做法是 **WSL2 + Ubuntu**：构建、
OpenOCD、串口和 MSC 都在同一个 Linux 环境里，路径脚本（`tools/*.py`、
`*/scripts/*.sh`）不用为 Windows 改写，也能直接访问 `/dev/ttyACM*` 和块设备。

原生 Windows 也能用，但本工程的所有入口脚本都是 POSIX shell / Python，只在 WSL
下保证可用。下面只讲 WSL 路线。

本文只负责主机环境和 USB 访问。源码获取见 [README](../README.md#获取源码)，
首次编译、烧录和登录见 [快速上手](quickstart.md)。

## 1. 装 WSL2

Windows 10 2004+ / Windows 11，管理员 PowerShell：

```powershell
wsl --install -d Ubuntu
```

装完重启，进 Ubuntu 建好普通用户。确认版本：

```powershell
wsl -l -v          # 输出里 VERSION 要是 2
```

项目放在 WSL 自己的文件系统里（例如 `~/research/`），**不要放在 `/mnt/c/...`**：
NTFS 上的 ext4 语义、大小写、文件权限（脚本的可执行位）都不可靠，Linux 内核编译
在 `/mnt/c` 下也慢很多。

## 2. Windows 侧：安装 usbipd

在 WSL 中使用调试探针、MSC 刷机或 ADB 时，需要把对应 USB 设备从 Windows
透传进来。探针兼作串口；USB1 在 bootloader 中提供 MSC，运行 Linux 后提供 ADB。这一步用 [usbipd-win](https://github.com/dorssel/usbipd-win)。

在管理员 PowerShell 里安装（任选其一）：

```powershell
winget install --interactive --exact dorssel.usbipd-win
# 或到 GitHub releases 下载 .msi 安装
```

装完在 PowerShell 里能看到设备列表：

```powershell
usbipd list
```

按当前模式识别设备，注意 VID:PID：

| 设备 | VID:PID | 用途 |
| --- | --- | --- |
| NSLink / CMSIS-DAP | `19f5:3106` | OpenOCD 调试 + 虚拟串口控制台 |
| 板子自己的 MSC | `19f5:5780` | 刷 dtb / kernel / rootfs（只在 MSC 模式出现） |
| Linux USB1 ADB | `1d6b:0105` | ADB shell 和文件传输（开发配置） |

首次使用要把设备共享出来（`bind` 只需做一次，之后一直有效，需要管理员权限）：

```powershell
usbipd list                                  # 先看清当前 busid
usbipd bind --busid <设备的 busid>
```

之后每次要在 WSL 里用，就 `attach`（**不需要**管理员权限）：

```powershell
usbipd attach --wsl --busid <设备的 busid>
```

用 WSL 里的 `dmesg | tail` 或 `ls /dev/ttyACM*` 确认挂上了。用完可以还给 Windows：

```powershell
usbipd detach --busid <设备的 busid>
```

MSC 和 ADB 是不同的 USB 设备，首次使用各自都要管理员 `bind`；ADB 序列号
随板子 CPU Serial 区分。板子切换模式或拔插后 busid 可能变；刷机时如果
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
    squashfs-tools adb \
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
| `adb` | Linux USB1 的 shell 和文件传输客户端 |
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
adb version
```

## 4. USB 权限

访问串口的普通用户需要 `dialout` 组权限：

```sh
sudo usermod -aG dialout "$USER"
```

重新登录后生效。串口用 `/dev/serial/by-id/` 中的名字识别，同一时间只保留一个
串口读者。MSC 块设备写入需要 `sudo`，完整流程见 [主机工具与烧写](tools.md)。

如果 WSL 的 `adb devices` 报 `no permissions`，可以为开发板建立 udev 规则：

```sh
sudo tee /etc/udev/rules.d/70-n32h787-adb.rules >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="1d6b", ATTR{idProduct}=="0105", MODE="0660", GROUP="plugdev"
EOF
sudo usermod -aG plugdev "$USER"
sudo udevadm control --reload-rules
```

重新登录并重新 attach ADB 设备后再试。这条规则匹配开发 VID/PID；产品修改标识后
应同步调整。USB 设备 attach 到 WSL 后，应在 WSL 内运行 ADB。

## 5. 确认环境并开始使用

按 [README 的源码获取说明](../README.md#获取源码) 拉取工程，在仓库根目录运行：

```sh
tools/h787ctl doctor
```

先确认工具检查项没有 `MISSING`。尚未接板、未进 MSC 或未启动 OpenOCD 时，对应
设备状态为空是正常的；需要哪个功能时，再按 [tools.md](tools.md) 连接对应设备。

接下来按 [快速上手](quickstart.md) 完成一次编译、刷机、登录和应用运行。
