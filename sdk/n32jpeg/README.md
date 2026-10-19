# libn32jpeg

`libn32jpeg` 是 N32H7xx 硬件 JPEG 编码器的用户态接口。它只封装
`/dev/jpeg`，不在用户态复制整帧数据，也不依赖标准 `libjpeg`。

当前 ABI 已实现同步 GREY8 编码；RGB24 和 YUV422 仅保留格式编号，驱动会返回
`EOPNOTSUPP`。设备只允许一个进程打开，第二次打开返回 `EBUSY`。

## 编译

在 SDK 根目录执行：

```sh
make -C n32jpeg
```

生成 `libn32jpeg.so`、`libn32jpeg.a` 和 `n32jpeg-test`。交叉编译器默认使用
`../toolchain/bin/arm-uclinuxfdpiceabi-`，可通过 `CROSS_COMPILE` 覆盖。

## 使用

```c
#include "n32jpeg.h"

int fd = n32jpeg_open(NULL);
struct n32jpeg_info info;
n32jpeg_query(fd, &info);

void *input = n32jpeg_alloc(width * height);
void *output = n32jpeg_alloc(width * height + 4096);
__u32 size;
n32jpeg_encode_grey8(fd, input, width, height, 80, output,
                     width * height + 4096, &size);
n32jpeg_free(output);
n32jpeg_free(input);
close(fd);
```

输入是紧排的 8 位灰度帧。宽和高都必须是 8 的倍数，输入、输出地址也必须
8 字节对齐；其他尺寸由驱动返回 `EINVAL`。当前 no-MMU DMA 路径要求每个缓冲区
在一次分配中保持物理连续；`n32jpeg_alloc()` 会做对齐分配。输出缓冲区不足时
编码返回 `ENOSPC`。

板上快速验证：

```sh
./n32jpeg-test -s 1024x768 -q 80 -o /mnt/sd/n32jpeg-test.jpg
./n32jpeg-test -s 320x240 -q 80 -n 10 -o /mnt/sd/n32jpeg-frame.jpg
```

`-n 10` 在同一个设备文件和同一组缓冲区上连续编码 10 帧，文件名依次为
`n32jpeg-frame.jpg.0` 到 `.9`。程序会检查每帧的 SOI/EOI，并打印实际输出
字节数。摄像头的 GREY 预览和通用 RGB 压缩仍在后续工作中，不要用当前库处理
非 GREY8 输入。
