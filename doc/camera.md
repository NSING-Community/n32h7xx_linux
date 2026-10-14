# OV5640 摄像头

N32H787 HMI 当前使用 DVP1 接口，OV5640 经 I2C4 配置，Linux 通过 V4L2 的
`/dev/video0` 提供采集。设备支持 JPEG、GREY、YUYV 和 RGB565 四种格式，
各格式实际可用的分辨率不一样，见下面"分辨率支持"。

## 分辨率支持

传感器只有固件里那 10 个离散模式，请求别的尺寸会由传感器吸附到最近的模式，
实际拿到的尺寸以驱动返回的 `width`/`height` 为准：

`160x120 176x144 320x240 640x480 720x480 720x576 1024x768 1280x720 1920x1080 2592x1944`

| 格式 | 交付分辨率 | 每帧字节数 |
|---|---|---|
| GREY | 160×120、176×144、320×240、640×480、720×480、720×576 | `width × height` |
| YUYV | 160×120、176×144、320×240 | `width × height × 2` |
| RGB565 | 160×120、176×144、320×240 | `width × height × 2` |
| JPEG | 全部 10 档，最高 2592×1944 | 压缩后大小可变 |

表里的每一档都在真实场景下跑过多轮（每档几百帧、交错采样）。决定稳定性的不是
总像素数，而是**每行字节数**：

- 每行 ≤ 720 B（GREY 到 720×576、YUYV/RGB565 到 320×240）不会成片丢帧；
- 每行 ≥ 1280 B（GREY 1024×768 及以上、YUYV/RGB565 640×480 及以上）开始成片
  丢帧，坏帧率几十个百分点，这些档位不列入交付；
- JPEG 走压缩通路，每行只有几百字节，不受这条限制，10 档全部可用。

### 坏帧

这条通路任何分辨率都不是 100%。驱动把 FIFO 溢出、帧被截断、压缩流顶到缓冲区
上限的帧标成 `V4L2_BUF_FLAG_ERROR`（内核日志里的 `short raw frame` /
`capture flagged`）。**应用必须检查这个标志**，未压缩格式还要核对 `bytesused`
是否等于 `width × height × bpp / 8`：长度固定，短了就是坏帧。JPEG 每帧长度
可变，但写得出的文件一定以 `FF D9` 结尾。

实测交付表内的档位偶发丢一帧（千分之几，与分辨率大小无关）：交付清单 22 个
组合各跑 150 帧（共 3300 帧）只有 GREY 640×480、GREY 720×576 各丢 1 帧；另一轮
GREY 320×240 连跑 400 帧丢 1 帧，丢的是流中间的第 6 个保存帧（38/240 行），
不是开流头几帧，所以多预热不能避开，只能按错误标志丢掉重取。

JPEG 两个最大档 1920×1080、2592×1944 的丢帧率略高（多轮合计约百分之一到几），
帧最大所以最先被 FIFO 截断：这类帧可能由驱动标成错误标志，也可能根本没写完
`FF D9`（`camera-matrix` 会报 `no JPEG EOI marker`）。两种都按坏帧重取一帧，
不要把这种帧当成图片内容去解析。

2592×1944 只支持 JPEG；GREY 请求该尺寸会被驱动拒绝。采集尺寸超出上表时
`camera-matrix` 会打印实际拿到的尺寸，不要按请求的尺寸去解析文件。

## 构建和安装采集工具

在主机工程目录执行：

```sh
make -C sdk/example v4l2-capture
adb push sdk/example/v4l2-capture /system/bin/v4l2-capture
adb shell chmod 755 /system/bin/v4l2-capture
```

DVP 采集驱动和 OV5640 传感器已**内建在内核里**，开机时由设备树自动 probe，
`/dev/video0` 由内核创建（rootfs 的 camera 服务只负责确认设备是否起来了）。
先确认设备和支持的格式：

```sh
ls /dev/video0
/system/bin/v4l2-capture -l
```

## 采集

帧文件写到 SD 卡，避免占用 ramfs 内存：

```sh
mkdir -p /mnt/sd/camera
/system/bin/v4l2-capture -f JPEG -s 1920x1080 -n 10 -w 4 -q 1 \
    -c test_pattern=0 -o /mnt/sd/camera/frame.jpg
```

`-n 10` 保存十帧，文件名为 `frame.jpg.0` 到 `frame.jpg.9`；`-w 4` 跳过四帧预热，
`-q 1` 使用一个采集缓冲区。将 `test_pattern=0` 改为 `1` 可采集传感器测试图。
JPEG 每帧长度可变，每个文件是一张独立图片。驱动已经把帧尾的传输填充去掉了，
每个 `.jpg` 都以 `FF D9` 结尾。

未压缩格式使用同样的命令，将 `-f` 改为 `GREY`、`YUYV` 或 `RGB565`。
640×480 的 GREY 每帧为 307200 字节。这些文件是像素数据，没有图片文件头，
需要按对应格式和尺寸解析。
工具报告坏帧或返回非零时，应检查错误输出，不能把文件存在作为采集成功的依据。

## 全格式验证（camera-matrix）

`camera-matrix` 把驱动报告支持的每种格式 × 每种尺寸各抓一帧、校验后写盘，
用来确认某个组合在这块板子上到底能不能用。它不替代上面的单张采集，而是
一次跑完全部组合：

```sh
make -C sdk/example camera-matrix
adb push sdk/example/camera-matrix /system/bin/camera-matrix
adb shell /system/bin/camera-matrix -l                         # 只列出会跑哪些组合
adb shell /system/bin/camera-matrix -o /mnt/sd/camera-matrix   # 全部跑一遍
```

`-f` 和 `-s` 可以只挑部分格式/尺寸，例如 `-f GREY,JPEG -s 160x120,320x240`。
每一行打印 `ok` 表示该帧通过校验（JPEG 查 SOI/EOI，未压缩按
`bytesperline × height` 查长度），失败会打印原因；工具自身返回非零表示有
组合失败。帧文件写到 SD 卡，不要写 `/tmp`（ramfs，会吃光内存）。

## MJPEG

裸 MJPEG 就是按顺序拼接的独立 JPEG，没有 HTTP multipart 头。`v4l2-capture`
的 `-o -` 把每帧的字节按顺序写到 stdout（进度信息走 stderr），所以一条命令
就能采出一个 `.mjpeg` 文件；分辨率按上面的 JPEG 清单选：

```sh
mkdir -p /mnt/sd/camera
/mnt/sd/tools/v4l2-capture -f JPEG -s 640x480 -n 30 -w 4 -q 1 \
    -c test_pattern=0 -o - > /mnt/sd/camera/cam.mjpeg
```

文件里没有帧率信息，回放时自己给；真实帧率取决于是哪个分辨率，`v4l2-capture`
每帧打印的耗时就是实测值：

```sh
scp root@<板子IP>:/mnt/sd/camera/cam.mjpeg .
ffplay -f mjpeg -framerate 15 cam.mjpeg
ffmpeg -f mjpeg -framerate 15 -i cam.mjpeg -c:v libx264 -pix_fmt yuv420p cam.mp4
```

要单个图片文件时用 `-o <路径>`（不带 `-`）：那会写成 `路径.0`、`路径.1`……
每帧一个文件。`-o -` 也可以直接接管道送给下一个程序，不必先落盘。

## 板级配置

`kernel/arch/arm/boot/dts/nsing/n32h787-hmi.dts` 的 OV5640 节点通过
`ovti,max-dvp-pclk-hz = <96000000>` 给并口字节时钟设上限。驱动根据当前格式
和 PLL 实际频率自行分频，采集应用无需另写传感器时钟寄存器。

DVP 驱动和 OV5640 都内建在 zImage 里，改 DTS 只需更新 DTB，只有改驱动
或 defconfig 才需要重新烧 kernel。
