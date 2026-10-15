# SDK 与应用程序构建

## 目录结构

```text
sdk/
├── toolchain/          交叉工具链（一个完整的 GCC prefix）
│   ├── bin/            arm-uclinuxfdpiceabi-gcc / -g++ / -as / -ld ...
│   ├── sysroot/        目标头文件和库（libc、libstdc++、libpthread）
│   ├── libexec/ lib/   GCC 内部程序和库
│   ├── arm-uclinuxfdpiceabi/  as/ld 的 triplet 目录
│   └── share/cmake/    CMake 工具链文件（给客户自己的工程用）
├── example/            应用示例源码，只带一个 Makefile
├── optional/           可选附加组件（预编译二进制），不参与构建，自行取用
└── optional-src/       上面这些组件的构建源码，同样不参与构建
```

工具链只有这一处：`sdk/toolchain/`，直接用全路径调用即可。`optional/` 与
`optional-src/` 只是随包提供的附加组件，`make` 不会引用它们，详见
[README 发布范围](../README.md#发布范围)。

## 示例一览

`sdk/example/` 里每个示例都是能独立读通的最小程序。`make -C sdk/example`
一次全部编出来，也可以只编一个（例如 `make -C sdk/example net-echo`）：

| 示例 | 演示 |
| --- | --- |
| `hello` | 最小 C 程序，动态链接 uClibc |
| `hello-cxx` | C++ 与 libstdc++ |
| `shared-app` + `libgreeting.so` | 自建共享库、`DT_RPATH` 找库 |
| `pthread-app` | uClibc pthread |
| `leds` | `/sys/class/gpio` 跑马灯（PB3 / PF10 / PI8） |
| `net-echo` | TCP echo 服务器：poll() 与线程两种模型、IPv6 双栈、背压 |
| `v4l2-capture` | V4L2 采集单帧 / MJPEG，见 [摄像头](camera.md) |
| `camera-matrix` | 摄像头全格式 x 分辨率验证，见 [摄像头](camera.md) |

板上的放音 / 录音本身用 rootfs 里的 TinyALSA，见 [音频](audio.md)。
声卡只收 16-bit 双声道、七档采样率，而 `tinyplay` 从 WAV 头取参数、又不做任何
转换，所以不是这个格式的 WAV 得在传上板之前转好。

## 可选附加组件

`sdk/optional/` 里是预编译好的附加程序，**不参与构建、不进任何镜像**，装不装、
装到哪儿都由使用者决定。`sdk/optional/install.sh` 把它们装到 `/system`：

```sh
sh install.sh                    # ssh，默认 192.168.1.85
BOARD=192.168.1.86 sh install.sh
USE=adb sh install.sh            # 走 adb
```

adb 路径直接推目录；往 `/system` 写比往 `/tmp` 慢一个数量级，见
[USB 文档的 adb 目录推送](usb.md#adb-的目录推送)。

| 组件 | 用途 | 依赖 |
| --- | --- | --- |
| [espeak-ng](../sdk/optional/espeak-ng/README.md) | 语音合成，板上的放音设备直接出声 | 无（GPL-3.0-or-later，见该目录 README） |
| [ns-flash](../sdk/optional/ns-flash/README.md) | 板子作 USB host，驱动外接 NSLink 探针烧写目标板 | 内核要有 USB host 与探针所用传输：bulk 探针走 usbfs，HID 探针要 `CONFIG_HIDRAW`（连同 `CONFIG_INPUT` / `CONFIG_HID_SUPPORT` / `CONFIG_HID` / `CONFIG_HID_GENERIC` / `CONFIG_USB_HID`） |

`ns-flash` 操作的是探针 SWD 那头接着的芯片，不是它自己跑着的这块板子。
`sdk/optional-src/` 放上面这些组件的构建源码，同样不参与构建。

## 工具链可以整体搬走

`toolchain/` 是一个自洽的 prefix：编译器用自己 executable 的位置反推
`sysroot`、`libexec`、`lib`，所以整个目录可以打包、移动到别的路径、甚至换机器，
不需要重新构建或改环境变量：

```sh
$ sdk/toolchain/bin/arm-uclinuxfdpiceabi-gcc -print-sysroot
/path/to/sdk/toolchain/bin/../sysroot
```

前提是 `bin/`、`lib/`、`libexec/`、`share/`、`sysroot/`、
`arm-uclinuxfdpiceabi/` 这些**要一起搬**，它们是同一个 prefix 的组成部分。

## 用 Make 编译

```sh
cd sdk/example
make
```

`sdk/example/Makefile` 里的工具链路径是**相对这个 Makefile 自己**算出来的，
不是相对当前目录，也不含任何绝对路径：

```make
example_dir := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
CROSS_COMPILE ?= $(example_dir)/../toolchain/bin/arm-uclinuxfdpiceabi-
```

所以下面几种调用都能工作：

```sh
cd sdk/example && make            # 在目录里
make -C sdk/example               # 从仓库根
make -f /abs/path/sdk/example/Makefile -C /abs/path/sdk/example
make -C /tmp/moved-sdk/example    # 整个 SDK 搬到别处之后
```

交叉前缀也可以覆盖，方便用自己的工具链：

```sh
make CROSS_COMPILE=/opt/other/bin/arm-uclinuxfdpiceabi-
```

主要变量：

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `CROSS_COMPILE` | 相对 `../toolchain/bin/` 推导 | 工具链前缀 |
| `CFLAGS` / `CXXFLAGS` | `-O2 -Wall -Wextra` | 编译选项 |
| `RUNTIME_LIBDIR` | `/system/lib` | 通过 `-rpath` 写进可执行文件 |
| `SYSROOT` | 空 | 置上时额外传 `--sysroot=` |

`clean` 删掉生成的 ELF 和 `.so`。

## 自己的工程用 CMake 或 Autotools

示例只用普通 `Makefile`，SDK 本身不绑定构建系统。换你自己习惯的构建系统时，
把工具链指到 `sdk/toolchain/` 就行，不需要额外安装。

CMake 带了工具链文件，它从自身位置推导 prefix，同样不含绝对路径：

```sh
cmake -S <你的工程> -B build \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/sdk/toolchain/share/cmake/n32h7xx.cmake" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

它设置了 `CMAKE_SYSTEM_NAME`、编译器、`CMAKE_SYSROOT` 和
`CMAKE_FIND_ROOT_PATH`，所以 `find_package` / `find_library` 只会在 sysroot 里找。
注意**不能 `try_run`**：构建主机是 x86_64，跑不了 ARM 程序。

Autotools 按常规交叉编译环境给 `--host` 和 `CC` 即可：

```sh
./configure --build=x86_64-pc-linux-gnu --host=arm-uclinuxfdpiceabi \
    --prefix=/usr \
    CC=$PWD/sdk/toolchain/bin/arm-uclinuxfdpiceabi-gcc
make && make install DESTDIR=/tmp/at-install
```

两种情况都要记得链接选项带 `-rpath /system/lib`（CMake 下是
`-DCMAKE_INSTALL_RPATH=/system/lib`），原因见下面"库放在哪"。

## 编译产物长什么样

```text
hello: ELF 32-bit LSB pie executable, ARM, EABI5 version 1,
       dynamically linked, interpreter /lib/ld-uClibc.so.0
```

硬浮点 ABI（`-mfloat-abi=hard`，`fpv5-d16`），动态链接到 uClibc。

## 库放在哪

设备上的动态库搜索路径：

| 路径 | 内容 |
| --- | --- |
| `/lib` | 系统库（`ld-uClibc.so.0`、`libc.so.0`） |
| `/usr/lib` | 同上 |
| `/system/lib` | 用户应用的库 |

这台设备的 uClibc **没有编译 `LDSO_CACHE_SUPPORT`**，所以既没有 `/etc/ld.so.conf`
也没有 `ld.so.cache`，`ld.so` 只会看 `/lib` 和 `/usr/lib`。因此应用自己的库必须
通过 `DT_RPATH` 指路——这也正是示例 Makefile 里默认带
`-Wl,-rpath,/system/lib`、CMake 里设 `CMAKE_INSTALL_RPATH` 的原因：不这样，
应用在 SSH 非登录命令或 init 脚本里（`/etc/profile` 没跑过、`LD_LIBRARY_PATH`
没设）就找不到 `/system/lib` 的库。

要临时试库的话，也可以手动指定：

```sh
LD_LIBRARY_PATH=/system/lib /system/bin/shared-app
```

## 部署到设备

在仓库根目录编译，应用放 `/system/bin`，自己的共享库放 `/system/lib`。
`/system` 是可写 JFFS2，更新 rootfs 时会保留其文件。USB ADB 部署不需要网线：

```sh
make -C sdk/example hello shared-app
adb push sdk/example/hello /system/bin/hello
adb push sdk/example/shared-app /system/bin/shared-app
adb push sdk/example/libgreeting.so /system/lib/libgreeting.so
adb shell /system/bin/hello
adb shell /system/bin/shared-app
```

联网时也可以用 SCP / SSH：

```sh
scp sdk/example/hello root@<板子IP>:/system/bin/
scp sdk/example/libgreeting.so root@<板子IP>:/system/lib/
ssh root@<板子IP> '/system/bin/hello'
```

普通应用和共享库直接传文件即可；更换 kernel、dtb 或整个 rootfs 时，按
[网络更新](ota-update.md) 或 [USB 烧写](tools.md) 操作。

### 开机自启

把自己的启动脚本放在可写的 `/system/bin/init.sh`，系统挂载 `/system` 后会调用：

```sh
#!/bin/sh
export PATH=/system/bin:/bin:/sbin
export LD_LIBRARY_PATH=/system/lib:/lib
/system/bin/my-app &
```

为脚本设置可执行权限。后台任务使用实际可执行程序，NOMMU 下的限制见本文末尾。

## 板载 LED 示例（/sys/class/gpio）

`leds.c` 是个跑马灯：三个用户 LED 接在 **PB3 / PF10 / PI8**，程序走经典
`/sys/class/gpio` 接口驱动它们。它不需要 libc 以外的任何库，也是看懂这块板
"从用户态碰硬件"最短的一条路。

```sh
make -C sdk/example leds
scp sdk/example/leds root@<板子IP>:/system/bin/
ssh root@<板子IP> '/system/bin/leds'          # 一直跑，Ctrl-C 停
ssh root@<板子IP> '/system/bin/leds 5 120'    # 5 轮，每步 120 ms
```

启动时会打印它算出来的编号，退出时（含 Ctrl-C）会关灯并 unexport 自己
export 过的线；如果那些线是别人 export 的，它只驱动、不动别人的状态。

### GPIO 编号不等于引脚名

gpiolib 在启动时给每个 bank 分配一个**基号**，本板是 512..672：一颗 16 线的
chip 一个端口，按 A..K 排。

| 端口 | chip label | base | 全局编号 |
| --- | --- | --- | --- |
| A | `GPIOA` | 512 | 512..527 |
| B | `GPIOB` | 528 | 528..543 |
| F | `GPIOF` | 592 | 592..607 |
| I | `GPIOI` | 640 | 640..655 |
| K | `GPIOK` | 672 | 672..687 |

所以 **PB3 = 528+3 = 531**、**PF10 = 592+10 = 602**、**PI8 = 640+8 = 648**。
基号是运行时分配的，不是固件常量，所以 `leds.c` 不写死，而是读
`/sys/class/gpio/gpiochipN/label` 找到 `GPIOB`/`GPIOF`/`GPIOI` 再算。

命令行手玩同一件事：

```sh
echo 531 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio531/direction
echo 1   > /sys/class/gpio/gpio531/value        # 点亮
echo 0   > /sys/class/gpio/gpio531/value        # 熄灭
echo 531 > /sys/class/gpio/unexport
```

`/sys/class/gpio` 和 `/dev/gpiochipN` 两套接口在本板都开着：前者是经典 ABI，
不需要任何用户态库，脚本里最好用；后者是现在的推荐接口，需要 libgpiod 或自己
写 ioctl。

### 用 gpio 脚本操作引脚

基号是运行时分配的，把 `531` 写进脚本就会在别次开机上变错。板上的 `/bin/gpio`
按 `gpiochipN/label` 反查，所以脚本里写引脚名即可：

```sh
gpio list                 # 各 bank 的 label / base / 线数
gpio num PB3              # 531
gpio export PB3           # 占用（可重复执行）
gpio dir PB3 out          # in | out
gpio set PB3 1            # 拉高（未 export 会自动 export 并设为 out）
gpio get PB3              # 0 或 1
gpio edge PB3 rising      # none | rising | falling | both
gpio info PB3             # direction / value / active_low / edge
gpio unexport PB3         # 释放
```

`gpio` 带 `#` 注释的头就是它自己的帮助，`gpio help` 打印的即本文这一段之前的
那个列表。`set` / `dir` 会按需 export，但**不会**替你 unexport——脚本结束前
自己 `gpio unexport`，否则那根线会一直被占用到重启（`gpio info` 会显示别的进程
不认得的占用状态）。三个用户 LED 是 **PB3 / PF10 / PI8**，试引脚就从它们开始。

注意两点：`gpio edge` 只让 `/sys/class/gpio/gpioN/value` 可以被 `poll()`，
**本板没有 GPIO 中断**（pinctrl 驱动没有 `irq_chip`），所以它不是真的外部中断源；
另外本板 busybox **没有** `gpio` applet，`/bin/gpio` 是板上唯一一个，不用担心
被哪个 applet 顶掉。

### 智能体 / 逻辑脚本常要的东西

板上 rootfs 带着脚本常用的一整套 busybox applet，不需要再装别的东西：`awk`、
`sed`、`grep`、`find`（`-maxdepth` / `-type` / `-name` / `-size` / `-exec … \;`
和 `-exec … +` 都在）、`xargs`、`expr`、`seq`、`cut`、`tr`、`sort`、`uniq`、
`comm`、`diff`、`cmp`、`tee`、`md5sum`、`base64`、`patch`、`strings`、`od`、
`hexdump`、`xxd`、`nc`、`nslookup`；后台与进程管理是 `nohup`、`setsid`、
`pgrep`、`pkill`、`flock`、`crond`。

几条在这个 no-MMU 环境里会影响脚本写法的：

- **后台要用 `nohup` 或 `setsid`**，不要靠 `&` 之后就登出。`nohup cmd
  >log 2>&1 &` 在 ssh 会话断开后仍然活着；`setsid` 还能把进程从会话里摘出来。
- **`timeout` 本树已修**（`3 s` 就能截断 `sleep 8`）。但它的退出码是 **143**
  （被 SIGTERM 杀掉的目标进程的终止状态），不是 GNU coreutils 的 124 —— 脚本
  判超时不要写 `[ $? -eq 124 ]`。
- **写文件避开 `/tmp`、`/run`、`/var/log`**：它们是 ramfs，写进去就是吃 RAM，
  没有配额，写爆时内核 OOM 掉的是**你正在写的那个进程**而不是返回 `ENOSPC`。
  长期落盘用 `/system`（JFFS2），抓帧一类的大产物用 `/mnt/sd`。
- **`fork()` 不存在**（no-MMU），要并行就用线程或直接 exec 另一个程序。
  后台只能后台**真正的可执行文件**：`nohup /system/bin/my-app >log 2>&1 &` 可以，
  `( ... ) &` / `{ ...; } &` 这种复合后台不行——规则见下面
  [无 MMU 环境的坑](#无-mmu-环境的坑)。管道和命令替换是安全的。
- **`PATH` 取决于是怎么登进来的**：非交互 ssh 是
  `/usr/sbin:/usr/bin:/sbin:/bin`，**没有 `/system/bin`**；串口登录 shell 读
  `/etc/profile` 才有。所以自己放在 `/system/bin` 的程序，从脚本或 ssh 单条命令
  调用时要写全路径，别依赖 `PATH`。同理 `LD_LIBRARY_PATH=/system/lib` 只在串口
  登录 shell 里默认给（见 [库放在哪](#库放在哪)），其它场合靠二进制自带的
  `-Wl,-rpath,/system/lib`。
- 跑长任务前先看一眼内存：`free` 与 `cat /proc/buddyinfo`。

## 网络示例（TCP echo 服务器）

`net-echo.c` 是个 TCP echo 服务器：监听一个端口，把收到的字节原样发回。它是
从"板子在网上"到"别人的程序能连上我的程序"最短的一条路，在 PC 上用 `nc` 就能
验证来回。

```sh
make -C sdk/example net-echo
scp sdk/example/net-echo root@<板子IP>:/system/bin/
ssh root@<板子IP> '/system/bin/net-echo 2323'     # 前台跑，Ctrl-C 停
```

然后在 PC 上：

```sh
nc <板子IP> 2323
hello            # 板子把 "hello" 原样发回来
Ctrl-D           # 断开
```

参数：

```sh
net-echo              # 0.0.0.0:2323，默认：一个 poll() 循环伺候所有连接
net-echo 5000         # 换端口
net-echo -t           # 一连接一线程（下面解释两者的区别）
net-echo -6 5000      # IPv6 双栈 + 换端口；选项和端口可以任意组合
```

`-6` 不是"只收 IPv6"，而是**双栈**：一个监听 socket 同时接受 IPv6 和 IPv4
（`IPV6_V6ONLY` 关掉），IPv4 客户端在日志里显示成 `[::ffff:192.168.1.2]` 这种
IPv4-mapped 形式。

### 两种服务方式：poll() 还是线程

同一个程序给了两种写法，默认和 `-t`，值得对着看：

| | 默认（`poll()`） | `-t`（一连接一线程） |
| --- | --- | --- |
| 进程/线程数 | 恒定 1 个线程 | 1 + 连接数个线程 |
| 每连接开销 | 一个缓冲（16 KB） | 一个线程栈（64 KB） |
| 慢客户端 | 只影响它自己那条连接 | 只阻塞它自己那个线程 |
| 代码 | 一个事件循环 | 每连接一个阻塞式 recv/send 循环 |

两种模式都能运行。本系统没有常规 `fork()` 所需的独立地址空间，不能直接照搬
一连接一进程的 `fork()` 服务模型。线程共享地址空间，uClibc 的 pthread 可用于
这里的并发处理；每个连接需要独立线程栈。

`-t` 的线程栈是**显式设成 64 KB** 的（`THREAD_STACK`）。默认栈可能是几 MB，
乘以 16 个连接就超过这块板的 RAM 了；worker 只做 recv/send，64 KB 足够。

### 背压：慢客户端不拖垮别人，也不被粗暴断开

两种模式都用 TCP 背压，行为一致：

- 默认模式下 socket 是非阻塞的，每个连接有一个 16 KB 发送缓冲。**只要这个连接
  还有没发完的数据，就只向它要 `POLLOUT`，不要 `POLLIN`**——即暂时不读它。
  对端读得慢，压力就落回对端自己的发送缓冲，最终阻塞在对端的 `send()` 上，
  而不是在板子上堆内存，也不是把连接掐掉。
- `-t` 模式下 worker 用的是普通阻塞 `send()`，天然就是这个行为。

实测（板子上，PC 连接只发不收）：慢客户端能推 60 万字节后自己阻塞住，同时另一个
客户端 echo 延迟仍是毫秒级；慢客户端之后重新开始读，数据照样在。

配套的注意点：`SIGPIPE` 被忽略，对端中途断开写不进去时靠 `send()` 返回错误处理，
而不是让进程被信号打死；`SIGINT`/`SIGTERM` 只置一个标志，循环退出后关掉所有
socket 再返回（`-t` 模式下会等 worker 自己收尾，实测 3 个空闲连接下 0.2 秒退干净）。

## 无 MMU 环境的坑

设备跑的是 no-MMU ARMv7-M Linux。写启动脚本时有一条硬性规则：

**后台任务只能是真正的可执行文件。**

```sh
/system/bin/my-app &         # 可以：子进程立刻 execve，父进程恢复
( ...; ... ) &               # 不行：不能使用常规 fork 后台模型
{ ...; ...; } &              # 不行，同上
```

NOMMU 上常规 `fork()` 不可用，启动子程序通常使用 `vfork()` 或
`posix_spawn()`：子进程在 `execve()` 或退出之前与父进程共享内存，父进程等待。
本系统的 shell 复合后台任务无法按常规 fork 模型执行，可能阻塞启动流程，
因此应把工作写成独立程序或脚本并执行。

普通命令替换 `x=$(外部命令)` 是安全的（子进程立刻 exec）。
