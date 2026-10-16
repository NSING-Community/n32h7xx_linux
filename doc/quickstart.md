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

开发配置默认 root 空密码，SSH 启用（Telnet 因明文传输且与 SSH 重复，已下线），USB ADB 免认证。
修改登录配置应在构建树编辑 `rootfs/etc/defaults.conf` 和 `rootfs/etc/shadow`，
重新打包并更新 rootfs；板上的 `/etc` 为只读，不能持久修改。ADB 服务控制及
USB Host/Device 切换见 [USB 文档](usb.md)。

SSH 主机密钥存放在 `/system/var/dropbear/`，只写一次、之后一直复用，所以重启
或者更新 kernel / dtb / rootfs 都不会再让客户端报 "host key changed"。密钥是
每块板子各自的，第一次用新 rootfs 启动时才会生成；主机上如果还留着旧记录，
删掉一次即可：

```sh
ssh-keygen -R <板子IP>
```

整片擦写的 `spi-nor-16M.img` 会连 system 分区一起擦掉，密钥随之重建，需要再
删一次记录；只更新 `spi-nor-boot.img` 则不受影响。`/system` 没有挂载时（分区
损坏、正在格式化）密钥回退到 ramfs，串口会打印
`REMOTE_HOSTKEY_VOLATILE`，此时每次重启都会变。

密钥稳定解决的是 "REMOTE HOST IDENTIFICATION HAS CHANGED"。板子 MAC 由芯片 UCID
派生，同一块板子每次启动都一样，DHCP 拿回同一租约，所以日常重启和刷机都不再
需要重新认证主机。只有地址真的换了（换网段、路由器改了租约表）才会再问一次
"are you sure you want to continue connecting"，那是 SSH 的正常行为，与密钥无关；
地址要写死可以在路由器上做 DHCP 保留，或把 `defaults.conf` 的 `NETWORK_MODE`
改成 `static` 并填好同网段的 `IPV4_ADDRESS`、`IPV4_GATEWAY`、`DNS1`。

### 板级默认配置

开机行为由 `rootfs/etc/defaults.conf` 决定，改完重新打包 rootfs 并更新分区才生效：

| 配置项 | 默认 | 说明 |
| --- | --- | --- |
| `HOSTNAME` | `n32h787` | 主机名 |
| `NETWORK_MODE` | `dhcp` | `dhcp` / `static` / `off` |
| `NETWORK_IPV6` | `slaac` | `slaac` / `dhcp6` / `off` |
| `NETWORK_IFACE` | `eth0` | 网口 |
| `IPV4_ADDRESS`、`IPV4_GATEWAY`、`DNS1` | 见文件 | 只在 `NETWORK_MODE=static` 时使用 |
| `REMOTE_LOGIN` | `ssh` | `off` / `ssh`（`telnet`、`both` 按 `ssh` 处理） |
| `USB_ADB` | `on` | USB1 的免认证调试 shell |
| `USB_ADB_UDC` | `40100000.usb` | ADB 绑定的控制器 |
| `CAMERA` | `on` | 开机确认摄像头就绪并生成 `/dev/video0`；驱动是模块时顺带加载 |

这个文件由 init 脚本用 `sed` 读取，值只是数据：不加引号、不留空格、不做变量
展开。关掉某项功能就改成 `off`，例如 `CAMERA=off` 之后不再检查摄像头。

## 5. 运行第一个应用

回到仓库根目录的主机终端，只编译并部署 `hello`：

```sh
make -C sdk/example hello
adb push sdk/example/hello /system/bin/hello
adb shell /system/bin/hello
```

程序输出后返回，完成从构建到板上执行的流程。工具链、SCP 部署、共享库、LED
及网络示例集中在 [SDK 文档](sdk.md)，摄像头见 [camera.md](camera.md)、音频见
[audio.md](audio.md)，这里不再逐个展开。

## 下一步

- 定制内核：[kernel-config.md](kernel-config.md)
- 网络更新 kernel / dtb / rootfs：[ota-update.md](ota-update.md)
- 应用与开机自启：[sdk.md](sdk.md)
- USB 功能与角色切换：[usb.md](usb.md)
