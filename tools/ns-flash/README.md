# ns-flash 命令行烧录

`ns-flash` 通过 NSLink 探针（CMSIS-DAP）烧写目标板，不依赖 OpenOCD。
片内 Flash 和外部 SPI NOR 共用一个可执行文件；外部 SPI NOR 由独立的
`.nsalgo` 算法包选择器件与引脚。

| 文件 | 用途 |
| --- | --- |
| `ns-flash-linux` | Linux / WSL x86-64 可执行文件 |
| `ns-flash-win.exe` | Windows x86-64 可执行文件 |
| `n32h787-xspi2.nsalgo` | N32H787 / XSPI2 / W25Q128（16 MiB）外部 SPI NOR 算法 |
| `ns-link.bin` | NS-Link 调试器固件；用于网页在线升级或 ROM DFU 恢复 |

版本：`0.1.6`。`ns-flash-linux` 没有动态库依赖，可直接运行。

探针只按 VID/PID 识别。固件 revision（`bcdDevice`）、接口类型和端点编号
与内置表不完全一致时，工具会优先选择最接近的接口继续连接，并采用探针
自报的 packet size；不会因为 1.02/1.06 的 revision 差异拒绝设备。

## 准备工作

1. 将 NSLink 探针接到电脑；WSL 下先用 `usbipd` 把 CMSIS-DAP v2 设备
   attach 进去（`usbipd list` 查看 BUSID，`usbipd attach --wsl --busid <BUSID>`）。
2. 确认工具能看到探针并记住索引：

```sh
chmod +x tools/ns-flash/ns-flash-linux
tools/ns-flash/ns-flash-linux --list-dap
```

只有一个探针时不需要 `--dap`。有多个时，用 `--list-dap` 输出的从 0 开始的
索引加 `--dap <index>`；索引不是 USB 总线号，重新插拔后要重新确认。

以下命令都在仓库根目录执行。

## 烧写片内 Flash

用于写入 `bin/bootloader.bin`：

```sh
# 只识别芯片，不写入
tools/ns-flash/ns-flash-linux --detect

# 自动识别芯片并烧写片内 Flash
tools/ns-flash/ns-flash-linux bin/bootloader.bin

# 多探针、指定 SWD 频率
tools/ns-flash/ns-flash-linux --dap 1 --speed 1000 bin/bootloader.bin
```

芯片型号默认从目标读取，通常不用写 `--target`。需要跳过识别时才显式指定，
例如 `--target n32h78x_i_2m`。

## 烧写外部 SPI NOR

外部 SPI NOR 的镜像放在位置参数，`--algorithm` 只接受本地 `.nsalgo` 文件
（命令行不支持算法 URL）：

```sh
# 查看算法支持的型号与接线，不连接探针
tools/ns-flash/ns-flash-linux \
    --algorithm tools/ns-flash/n32h787-xspi2.nsalgo --targets

# 识别 CPU 和外部 Flash，不擦写
tools/ns-flash/ns-flash-linux \
    --algorithm tools/ns-flash/n32h787-xspi2.nsalgo --detect

# 烧写外部 SPI NOR（默认不读回校验）
tools/ns-flash/ns-flash-linux \
    --algorithm tools/ns-flash/n32h787-xspi2.nsalgo bin/spi-nor-boot.img

# 烧写并整幅读回，逐字节比较
tools/ns-flash/ns-flash-linux \
    --algorithm tools/ns-flash/n32h787-xspi2.nsalgo --verify bin/spi-nor-boot.img
```

几点约定：

- 镜像从外部 Flash 偏移 0 写入，只擦除镜像覆盖的扇区。
- SPI 镜像通常较大，默认烧完即止，**不做读回校验**；需要确认结果时加
  `--verify`，代价是整幅镜像多读一遍。
- `bin/spi-nor-boot.img` 只覆盖 DTB、kernel 和 rootfs。Linux 启动还需要
  片内已有兼容的 bootloader，`system` 分区不受影响。
- 是否校验只看 `--verify`；`--nrst` 不改变这一点。片内 Flash 另按默认
  流程校验。

## 升级 NS-Link 调试器

### 新固件：网页在线升级

带在线升级功能的 NS-Link 可以直接在浏览器里更新。打开烧录页，连接调试器，
在“调试器支持在线升级”卡片点击“在线升级”；也可以直接访问 `?view=ota`。
升级走调试器现有的免驱接口，不需要安装驱动，也不需要本地服务。设备会在
写入完成并校验 CRC 后自动重启。

### 旧固件：一次性 ROM DFU

旧版 HID 固件和 1.06 之前的固件没有在线升级命令。网页检测到这类设备时只
显示下载与升级教程，不提供在线升级按钮；按下面的步骤用命令行升级一次后，
后续即可使用网页 OTA。

旧版 NS-Link 使用 N32G45x 的 ROM BOOT。把 `BOOT0=1`、`BOOT1=0`，
短接 J12 后重新插 USB，设备会以 `19F5:4537` 出现；升级完拔掉 J12
再重新插 USB，回到正常的 `19F5:3106` 探针。

N32H787 板子拉高 TP1 进入同一 ROM 模式。网页的 ROM 恢复页提供离线恢复包，
包内已经放好两个平台的命令行工具、`ns-link.bin` 和校验和：

<https://update.nsingtech.com/ns-flash/ns-link-dfu-recovery.zip>

Windows 解压后双击 `flash-windows.bat`；Linux 运行 `./flash-linux.sh`。
脚本会进入 ROM 模式检查、擦除、写入并做 CRC 校验。Windows 通过
`NationstechDFU.sys` 通信，不需要 WinUSB、Zadig 或 libusb。

下载页：<https://update.nsingtech.com/ns-flash/>

```sh
# 列出 ROM BOOT 接口并读取芯片信息
tools/ns-flash/ns-flash-linux --dfu-list
tools/ns-flash/ns-flash-linux --dfu-info

# 写入 NS-Link 固件；默认不校验
tools/ns-flash/ns-flash-linux --dfu tools/ns-flash/ns-link.bin

# 需要确认时，用 ROM 自带的 CRC 校验（不是整片回读）
tools/ns-flash/ns-flash-linux --dfu tools/ns-flash/ns-link.bin --verify
```

Windows 使用系统里的 `NationstechDFU.sys`，不需要装 libusb 或 WinUSB。
ROM BOOT 没有 `DFU_UPLOAD`，所以 `--verify` 只发一条 CRC 范围校验命令；
默认不校验，烧写速度不受影响。Linux 下直接走 `usbfs`，普通用户需要
对应的 udev 权限。

浏览器无法在 Windows 上抢占已被 `NationstechDFU.sys` 绑定的接口，所以
ROM DFU 这一步使用上面的命令行工具；升级完成后的新固件不需要这个驱动，
可以直接在网页内 OTA。

## 复位与 `--nrst`

烧录前工具会自动复位并停住 CPU，不需要连接 NRST 跳线。

只有应用固件破坏调试访问、软件复位无法恢复，或探针 NRST 已接到目标板时，
才加 `--nrst`：

```sh
tools/ns-flash/ns-flash-linux --nrst bin/bootloader.bin
tools/ns-flash/ns-flash-linux \
    --algorithm tools/ns-flash/n32h787-xspi2.nsalgo --nrst bin/spi-nor-boot.img
```

没有接 NRST 跳线却加 `--nrst`，复位会失败，工具在擦除前停止。

## 常用选项

| 选项 | 说明 |
| --- | --- |
| `--list-dap` | 列出所有探针接口并退出 |
| `--dap <index>` | 按 `--list-dap` 的索引选择探针 |
| `--detect` | 识别芯片后退出，不写入 |
| `--id` | 只读 IDCODE 后退出 |
| `--target <name>` | 显式指定芯片，跳过自动识别 |
| `--targets` | 列出内置芯片表后退出 |
| `--algorithm <file>` | 使用本地 `.nsalgo` 烧写外部 SPI NOR |
| `--speed <khz>` | SWD 时钟；默认 8000 kHz，旧版 HID 固件默认 2000 kHz |
| `--nrst` | 通过 NRST 复位并捕获停机 |
| `--verify` | 整幅读回并逐字节比较 |
| `--dfu <image>` | 通过 N32G45x ROM BOOT (`19F5:4537`) 写入片内 Flash |
| `--dfu-list` | 列出 ROM BOOT 接口并退出 |
| `--dfu-info` | 读取 ROM BOOT 型号、版本、UCID/UID/IDCODE 后退出 |
| `--dfu-address <addr>` | ROM BOOT 镜像起始地址，默认 `0x08000000` |
| `--dfu-index <n>` | 有多个 ROM BOOT 时选择其中一个 |
| `--no-reset` | DFU 写完后不发送 `SYS_RESET` |
| `--verbose` | 输出详细诊断信息 |
| `--quiet`, `-q` | 关闭进度行，保留报告和警告 |
| `--version` / `--help` | 版本 / 完整帮助 |

退出码：`0` 成功，`1` 目标或校验失败，`2` 参数错误，`3` 探针或传输失败，
`4` 内部错误。

## 浏览器烧录

不装命令行工具时，可用在线页面（Chrome / Edge）：

<https://update.nsingtech.com/ns-flash/index.html>

页面同样支持片内 Flash 和外部 SPI NOR，并能直接选用内置的
N32H787 XSPI2 算法。

## 许可

`ns-flash` 是公司自有的闭源程序，随 SDK 分发不授予开源许可。上述主机端
可执行文件使用 Zig 标准库构建，其 MIT 许可与归属见同目录的
`THIRD_PARTY_NOTICES.txt` 和 `Zig-MIT.txt`。
