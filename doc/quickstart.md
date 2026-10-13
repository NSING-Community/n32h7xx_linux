# 快速上手

本文走一遍首次使用：编译镜像 → USB 刷机 → 登录 → 运行一个应用。
主机工具先按 [环境搭建](setup.md) 安装，源码按 [README](../README.md#获取源码)
拉取。下面的主机命令均在仓库根目录的 WSL / Linux 终端执行。

## 1. 接线

| 接口 | 本流程用途 |
| --- | --- |
| 电源 | 给板子供电 |
| USB1（PA11 / PA12） | bootloader MSC 刷机；Linux 启动后 ADB 登录 |
| NSLink / CMSIS-DAP | 可选；串口日志、调试及片内 bootloader 烧写 |
| 网线 | 可选；SSH、SCP 和网络 OTA |

本流程要求板子片内已有本工程 bootloader；尚未安装或需要更换时，先按
[bootloader 烧写说明](tools.md#烧写-bootloader) 操作。

## 2. 编译镜像

```sh
make
```

完成后使用 `out/dist/spi-nor-boot.img`，它包含 dtb、kernel 和 rootfs，保留可写
`system` 分区。构建目标及产物说明见 [README](../README.md#构建)。

## 3. USB 刷机

按住 KEY1（PG3）复位或上电，让 bootloader 进入 MSC 模式。Windows PowerShell
中用 `usbipd list` 找到 `19f5:5780`，首次管理员共享后 attach 到 WSL：

```powershell
usbipd bind --busid <MSC 的 busid>     # 首次，管理员 PowerShell
usbipd attach --wsl --busid <MSC 的 busid>
```

在仓库根目录的 WSL 终端写入：

```sh
sudo tools/h787ctl msc-write out/dist/spi-nor-boot.img
```

确认写入和读回校验成功，Windows 侧 `usbipd detach --busid <MSC 的 busid>`，
松开 KEY1，再复位或重新上电。其他进入 MSC 的方法、单分区写入和探针烧写见
[主机工具与烧写](tools.md)。

## 4. 登录并检查启动

Linux 启动后 USB1 重新枚举为 ADB。按 [环境搭建的 USB 共享步骤](setup.md#2-windows-侧安装-usbipd)
共享并 attach `1d6b:0105`（它与 MSC 是不同设备），然后在 WSL 中运行：

```sh
adb devices
adb shell
```

ADB shell 默认目录为 `/system`。在板子 shell 中检查：

```sh
uname -a
cat /proc/mtd
ifconfig eth0
df
exit
```

应看到四个 MTD 分区、只读 `/` 和可写 `/system`。也可以通过 115200 8N1 串口
登录；接网线时可用 `ssh root@<板子IP>`，IP 从串口、ADB 的 `ifconfig eth0` 或
路由器 DHCP 租约查询。

开发配置默认 root 空密码，SSH/Telnet 同时启用，USB ADB 免认证。
修改登录配置应在构建树编辑 `rootfs/etc/defaults.conf` 和 `rootfs/etc/shadow`，
重新打包并更新 rootfs；板上的 `/etc` 为只读，不能持久修改。ADB 服务控制及
USB Host/Device 切换见 [USB 文档](usb.md)。

## 5. 运行第一个应用

回到仓库根目录的主机终端，只编译并部署 `hello`：

```sh
make -C sdk/example hello
adb push sdk/example/hello /system/bin/hello
adb shell /system/bin/hello
```

程序输出后返回，完成从构建到板上执行的流程。工具链、SCP 部署、共享库、LED
及网络示例集中在 [SDK 文档](sdk.md)，这里不再逐个展开。

## 下一步

- 定制内核：[kernel-config.md](kernel-config.md)
- 网络更新 kernel / dtb / rootfs：[ota-update.md](ota-update.md)
- 应用与开机自启：[sdk.md](sdk.md)
- USB 功能与角色切换：[usb.md](usb.md)
