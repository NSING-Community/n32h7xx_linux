# 快速上手

第一次用这台机器先把环境搭起来：[setup.md](setup.md)（Windows + WSL2、编译与
调试工具、usbipd 透传）。

## 接线

| 接口 | 用途 | 必需 |
| --- | --- | --- |
| 电源 | 供电 | 是 |
| 网线 | SSH、scp、OTA | 是（日常开发） |
| NSLink / CMSIS-DAP | 调 bootloader、烧片内 flash、看内核日志 | 只在换 bootloader 或调试时 |
| 串口（USART1，`PA9=TX`/`PA10=RX`，115200 8N1） | 控制台 | 可选，SSH 可用时不必 |
| USB（设备自己的 MSC） | 更新 dtb / kernel / rootfs 镜像 | 只在更新这三个分区时 |

## 上电

板子从外部 SPI NOR 启动：bootloader 读 `dtb` 和 `kernel` 两个分区，把内核放进
SDRAM 后跳过去，内核再挂载 `rootfs`（SquashFS，只读）和 `system`（JFFS2，可写）。

启动完可以在串口上看到内核的 printk；`rootfs/etc/inittab` 会在 `ttyS0` 上起一个
shell。看到 `N32H787:/ #` 提示符就说明系统已经起来了。

## 登录

设备通过 DHCP 拿地址，SSH 默认用户名 `root`：

```sh
ssh root@<板子IP>
```

地址是 DHCP 分配的，可能每次都变，用串口控制台或路由器查当前值。`telnet` 也开
着，用途和 SSH 一样。

> **默认凭据是空的。** 出厂 rootfs 里 `root` 的密码就是空字符串
> （`/etc/shadow` 里存的是空密码的 `$6$` 哈希），并且 `/etc/defaults.conf`
> 的 `REMOTE_LOGIN=both` 同时开了 telnet 和 ssh，其中 telnet 是明文的。
> 这是调试板的默认值，任何能连上 22 / 23 端口的人都会直接拿到 root。
>
> 配置是**只读**的：`/etc/defaults.conf` 和 `/etc/shadow` 都编译在 rootfs 里，
> 板上没有写它们的入口（rootfs 是只读 SquashFS，`/system` 只放程序和库），
> 所以 `passwd` 这类改动不会被保存。要改就改构建树、重新 `make` 再烧写：
>
> ```sh
> # 关掉 telnet
> sed -i 's/^REMOTE_LOGIN=.*/REMOTE_LOGIN=ssh/' rootfs/etc/defaults.conf
> # 设一个真密码（先用主机上的 openssl passwd -6 生成哈希）
> sed -i 's|^root:.*|root:<哈希>:19000:0:99999:7:::|' rootfs/etc/shadow
> make && make flash-image
> ```

登录后先看这几个：

```sh
uname -a
cat /proc/mtd            # 四个 flash 分区
ifconfig eth0
df                       # / (squashfs ro)、/system (jffs2 rw)、/tmp (ramfs)
```

## 编译一个应用

交叉工具链在 `sdk/toolchain/bin/`：

```sh
cd sdk/example
make                     # 直接编译示例，Makefile 自己找工具链
```

编译产物是 ARM EABI 动态链接的可执行文件，解释器 `/lib/ld-uClibc.so.0`。传到设备
上跑：

```sh
scp hello root@<板子IP>:/system/bin/
ssh root@<板子IP> '/system/bin/hello'
```

`sdk/example/leds.c` 是个更贴板子的例子：PB3 / PF10 / PI8 三个 LED 的跑马灯，
走 `/sys/class/gpio`，不依赖 libc 以外的库。

```sh
make -C sdk/example leds
scp sdk/example/leds root@<板子IP>:/system/bin/
ssh root@<板子IP> '/system/bin/leds 5 120'    # 5 轮，每步 120 ms
```

`sdk/example/net-echo.c` 是个 TCP echo 服务器，用来试"板子上的程序能被外面的
程序连上"：

```sh
make -C sdk/example net-echo
scp sdk/example/net-echo root@<板子IP>:/system/bin/
ssh root@<板子IP> '/system/bin/net-echo 2323'
# PC 上另开一个终端：
#   nc <板子IP> 2323     -> 输入的每行都会原样发回来
```

加 `-t` 就换成一连接一线程的实现，不加则是一个 `poll()` 循环伺候所有连接；两者
的区别和线程在这里为什么可用，见 [sdk.md](sdk.md#两种服务方式poll-还是线程)。

细节（自己的工程怎么接工具链、库放哪、为什么用 `/system/lib`）见 [sdk.md](sdk.md)。

## 下一步

- 开发环境搭建：[setup.md](setup.md)
- 更新设备上的分区：[ota-update.md](ota-update.md)
- 应用构建与 SDK 布局：[sdk.md](sdk.md)
