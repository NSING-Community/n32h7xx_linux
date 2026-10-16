# 摄像头拍照（OV5640）

触发：用户要求拍照、抓帧、看摄像头画面、做视觉测试时读本技能。

- 设备 /dev/video0，驱动和传感器已内建；工具 `/system/bin/v4l2-capture`。
- 命令要用全路径（exec 的 PATH 不含 /system/bin）。
- 输出放 /mnt/sd；/tmp 是内存，别放大图。
读完本技能直接执行需要的命令，不要先做额外探测或信息收集。


## 常用命令

    /system/bin/v4l2-capture -l                                       # 列格式
    /system/bin/v4l2-capture -f JPEG -s 1024x768 -o /mnt/sd/cap.jpg   # 默认尺寸
    /system/bin/v4l2-capture -f JPEG -s 2592x1944 -o /mnt/sd/big.jpg  # 用户要高清
    /system/bin/v4l2-capture -f GREY -s 320x240  -o /mnt/sd/cap.pgm
    /system/bin/v4l2-capture -c test_pattern=1 -f JPEG -s 640x480 -o /mnt/sd/t.jpg

- 默认拍 1024x768：省内存、省图片 token，够看清；不要默认拍高清。
- 用户明确要高清/最大分辨率时才用 2592x1944（500 万像素）。
- `-c test_pattern=1` 输出内部彩条，无外部场景时用它自检。
- `-n N` 存 N 帧（文件名后加序号），`-w N` 预热丢帧（默认 2，等自动曝光稳定）。

## 支持的分辨率

传感器固定 10 档，请求其他尺寸会吸附到最近档，以驱动实际返回的尺寸为准：

    160x120   176x144   320x240   640x480   720x480
    720x576   1024x768  1280x720  1920x1080 2592x1944

| 格式 | 可用档位 | 每帧字节数 |
| --- | --- | --- |
| GREY | 最高 720x576 | w*h |
| YUYV / RGB565 | 最高 320x240 | w*h*2 |
| JPEG | 全部 10 档（实测可用） | 压缩，几十 KB 到约 1 MiB |

## 拍完必须校验

- 校验与拍摄并入同一条命令（`... && tail -c 2 ...`），不要单独占一轮。
- 看 stderr：出现 `BAD` / `short` 的帧是坏的，重拍。
- JPEG 必须以 FF D9 结尾：`tail -c 2 /mnt/sd/cap.jpg | xxd` 应为 `ff d9`。
- 未压缩：文件字节数应等于 w*h（GREY）或 w*h*2（YUYV/RGB565），短了就是坏帧。
- JPEG 单帧上限 2 MiB，顶到上限的帧标记为坏帧，不要当图片解析。
