# N32H787 USB

两路控制器复用 Linux DWC2，支持 Host 和 Device。默认 USB1 为 Device，USB2 为
Host。设备树决定默认角色；启用 `usb-role-switch` 时也可以在运行中切换。

| 控制器 | DM / DP | ID | Host 供电 |
| --- | --- | --- | --- |
| USB1（40100000） | PA11 / PA12 | PA10 留给 USART1 RX | 无受控供电开关 |
| USB2（40060000） | PB14 / PB15 | PB12；HMI V1.1 的 R147 标为 NC | PB4 高电平开启 |

HMI 板采用 Type-C 接口，不能把未连接的 ID 信号当成角色检测器。当前配置使用软件
切换。USB1 用作 Host 时需要合适的转接及外部供电；USB2 的 PB4 由内核 regulator
控制，应用不应再通过 GPIO sysfs 操作这根线。

## 切换角色

通过串口或 SSH 执行切换。USB1 从 Device 切到 Host 时，当前 ADB 连接会断开，
因此不要用 ADB 作为切换后继续操作的唯一通道。查看两路当前角色：

```sh
cat /sys/class/usb_role/40100000.usb-role-switch/role
cat /sys/class/usb_role/40060000.usb-role-switch/role
```

USB1 默认提供 ADB。切到 Host 前停止服务、断开与电脑的 USB 连接，再接 Host
转接线和外设；USB1 Host 需要外部 VBUS 供电：

```sh
/etc/init.d/adbd stop
echo host > /sys/class/usb_role/40100000.usb-role-switch/role
cat /sys/class/usb_role/40100000.usb-role-switch/role
```

恢复 USB1 Device 和 ADB，再连接电脑：

```sh
echo device > /sys/class/usb_role/40100000.usb-role-switch/role
/etc/init.d/adbd start
```

USB2 默认为 Host，PB4 供电随角色切换由内核自动控制。切到 Device 前先卸载
该端口上的存储设备并拔下外设；如果已有 gadget，切回 Host 前先解绑其 `UDC`：

```sh
echo device > /sys/class/usb_role/40060000.usb-role-switch/role
cat /sys/class/usb_role/40060000.usb-role-switch/role
# 创建并绑定 gadget 后，Device 才会向电脑提供具体功能。
# 用完后先解绑 gadget，再恢复 Host：
echo host > /sys/class/usb_role/40060000.usb-role-switch/role
```

可写 `host`、`device`、`none`。这里的 `none` 恢复设备树默认角色，不表示关闭
端口供电。运行时切换不修改 DTB，重启后恢复 USB1 Device、USB2 Host 的默认值。

要固定角色，在板级 DTS 的对应节点中设置 `dr_mode = "host";` 或
`dr_mode = "peripheral";`，删除 `usb-role-switch` 和
`role-switch-default-mode`。如果保留运行时切换，则使用 `dr_mode = "otg";`
并通过 `role-switch-default-mode` 设置 `host` 或 `peripheral`。

有真实 ID 接线的其他板型可以给 USB2 的 `pinctrl-0` 加上 `&usb2_id_pin`，给
`&usb2_phy` 增加 `nsing,external-id;`，并删除软件角色切换属性，让控制器按 ID
切换。不要在 HMI V1.1 的默认装配上直接启用它，也不要占用 USB1 的 PA10。

## Device：ADB

默认 rootfs 在 USB1 启动 microADB，连接电脑后可使用：

```sh
adb devices
adb shell
adb push file /system/file
adb pull /system/file
```

shell 默认进入 `/system`，交互登录加载 `/etc/profile`，使用与 SSH 相同的
彩色提示符及环境配置。支持交互终端及普通文件传输，使用 Apache-2.0 的
microADB；这是免认证的开发板 root 接口，未实现 Android 应用管理及 shell_v2。
设备序列号采用 CPU Serial，多块板连接时用 `adb -s <序列号>` 选择。
主机需要安装 ADB 并具备 USB 设备访问权限。WSL 使用前需通过 usbipd 共享设备；
转接给 WSL 后应在 WSL 内运行 ADB。

### 传输速度

`adb push` / `adb pull` 每帧都要一次 USB 往返，板上实测单帧固定开销约 5 ms，所以
较大的帧可以减少往返次数。默认帧载荷为 65544 字节，其中文件数据最多 64 KiB。
4 MiB 文件读写 ramfs `/tmp` 的中位数：

| 帧载荷 | push | pull |
| --- | --- | --- |
| 4 KiB | 1.4 MB/s | 1.4 MB/s |
| 16 KiB | 2.9 MB/s | 2.7 MB/s |
| 32 KiB | 3.6 MB/s | 2.8 MB/s |
| 65544（= 64 KiB + 8，当前） | 4.2 MB/s | 3.7 MB/s |

65544 不是调出来的数，它按 Android 默认来：`CONFIG_ADBD_SYNC_DATA_MAX` 取
`file_sync_protocol.h` 的 `SYNC_DATA_MAX`（64 KiB），加上 8 字节 sync DATA 头
就是帧载荷。主机侧 `file_sync_client.cpp` 把 `max` 写死成 `SYNC_DATA_MAX`
（注释 `// TODO: decide at runtime.`），设备在 CNXN banner 里通告更大的
`maxdata` 也不会被抬高；pull 收帧时 `msg.data.size > sc.max` 直接报
`msg.data.size too large` 并删掉半截文件。所以这个值就是上限，不要往上调。
再往上提速要改 USB 收发路径本身（单帧开销加上 gadget 数据通路约 6–7 MB/s），
不是配置层面的事。

### 主机侧只能有一个 adb server

adb 固定监听 `127.0.0.1:5037`。`.wslconfig` 用 `networkingMode=Mirrored` 时
WSL 与 Windows 共享环回，两边抢的是同一个端口：**先起来的那一边占住 5037，
另一边就再也起不了 daemon，`adb devices` 一直是空的**。而且这个占用在 Windows
`netstat` 和 WSL `ss` 里都可能看不到，很容易误判成"没东西占着"。

区分办法是让失败的一边前台跑一次 daemon，它会直接说出原因：

```sh
adb nodaemon server          # 报 cannot bind to 127.0.0.1:5037 ... (10048) 即是此冲突
```

Windows 侧要用 adb 就别在 WSL 里跑 adb（反过来说，需要把设备 `usbipd attach`
给 WSL 时，Windows 侧同时也用不了——设备和 server 都只能归一边）。要在 WSL 里
用 adb，最省事的是让它连 Windows 那个 daemon，双方共用；不行就给 WSL 单独一个
端口：

```sh
export ADB_SERVER_SOCKET=tcp:127.0.0.1:5038
```

WSL 侧残留的 server 清掉即可恢复 Windows：

```sh
pgrep -a adb && pkill -x adb
```

另外 WSL 发行版的 adb 版本可能和 Windows 的不是同一个（本机 34.0.5 对
37.0.1），共用 daemon 时以先起来的那个版本为准。

服务控制：

```sh
/etc/init.d/adbd status
/etc/init.d/adbd stop
/etc/init.d/adbd start
/etc/init.d/adbd restart
```

### adb 的目录推送

`adb push <目录> <目标目录>/` 和单文件一样可用，一个会话传完整个目录树。
往 `/system`（JFFS2，SPI NOR）写要比往 `/tmp`（ramfs）慢一个数量级：同样
3.7 MiB 的 13 个文件，`/system` 约 33 s，`/tmp` 约 1.2 s。慢的是 flash 写入，
不是 adb。`adb shell` 与 push 互不影响，可以同时用。

日志为 `/run/adbd.log`。关闭开机启动：把 rootfs 的 `/etc/defaults.conf` 中
`USB_ADB=on` 改为 `USB_ADB=off` 后重新打包。
`USB_ADB_UDC` 选择控制器，默认 `40100000.usb`。更换 gadget 或切到 Host 之前
先停止 ADB。配置使用示例 VID/PID，发布产品时应替换为自己的标识。

## Device：创建一个 ACM 串口

Device 角色提供 UDC，具体 USB 功能由 gadget 配置决定。默认内核包含 configfs
和 ACM 串口功能，可以在板上运行下面的示例。示例 VID/PID 仅用于本地测试，发布
产品时应使用自己的标识。

```sh
/etc/init.d/adbd stop
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
cd /sys/kernel/config/usb_gadget
mkdir serial
cd serial
echo 0x1d6b > idVendor
echo 0x0104 > idProduct
mkdir strings/0x409
echo n32h787-usb1 > strings/0x409/serialnumber
echo NSING > strings/0x409/manufacturer
echo 'N32H787 ACM' > strings/0x409/product
mkdir configs/c.1
mkdir functions/acm.usb0
ln -s functions/acm.usb0 configs/c.1/acm.usb0
echo device > /sys/class/usb_role/40100000.usb-role-switch/role
echo 40100000.usb > UDC
```

要在 USB2 创建同样的串口，把上述角色路径中的 `40100000` 改成 `40060000`，
绑定 `UDC` 时改写 `40060000.usb`；先将 USB2 切到 Device。

电脑连接 USB1 后出现串口，板端对应 `/dev/ttyGS0`。查看连接状态：

```sh
cat /sys/class/udc/40100000.usb/state
```

`configured` 表示主机已经配置了设备。解绑：

```sh
echo '' > /sys/kernel/config/usb_gadget/serial/UDC
```

这个配置保存在 configfs 的内存中，重启后需要重新创建。若要开机创建，可以将
命令放在自己的 `/system/bin/init.sh` 中。

## Host：存储设备

默认内核包含 USB Storage、SCSI 磁盘和 VFAT/exFAT。插入 USB2 后先用 `mount`、
`dmesg` 和 `/dev/sd*` 确认设备及挂载状态。rootfs 的 mdev 规则会尝试将受支持的
SD / USB 分区挂到 `/mnt/sd`；该位置已被其他存储占用时，可选其他目录手动挂载：

```sh
mkdir -p /mnt/usb
mount /dev/sda1 /mnt/usb
# 使用完成后
umount /mnt/usb
```

实际设备名和挂载点以枚举及 `mount` 输出为准。切换 Host 角色或拔盘前，先卸载
该设备的实际挂载点。

## Host：查看设备

安装了 BusyBox `lsusb` 时可以列出 Host 总线；也可以直接读取 sysfs：

```sh
lsusb
cat /sys/bus/usb/devices/*/uevent
```

每个 Host 控制器各有一个 root hub。默认 USB1 Device、USB2 Host 时只有 USB2
的 root hub；两路都设为 Host 才有两个。即使未插外设，Host 的 root hub 也会
出现。Device 控制器是 UDC，不提供 Host 总线。BusyBox `lsusb` 没有 `-t` / `-v`，
详细信息可直接查看 `/sys/bus/usb/devices/`。

## Host：CMSIS-DAP 探针

板子作 Host 接一个 NSLink 探针时，用的是 [ns-flash](sdk.md#可选附加组件) 这个
可选组件（`sdk/optional/ns-flash/`）。它让板子当主机去烧写探针 SWD 那头接着的
芯片，不是烧写自己。

NSLink 有两种固件，工具会按探针应答的接口自动选后端，但**内核两边都要有**：
bulk（CMSIS-DAP v2）走 usbfs，有 `CONFIG_USB` 加 Host 控制器就够；
HID（CMSIS-DAP v1）要 `CONFIG_HIDRAW`，即 `CONFIG_INPUT` + `CONFIG_HID_SUPPORT`
+ `CONFIG_HID` + `CONFIG_HID_GENERIC` + `CONFIG_HIDRAW` + `CONFIG_USB_HID`，
否则设备能枚举（`lsusb` 看得到 `19f5:3106`）、但没有 `/dev/hidrawN` 可开。
