# 摄像头拍照（OV5640）

触发：用户要求拍照或抓取当前摄像头画面时读本技能。

## 强制路由

- 拍照前先看 `human-detect` 是否运行；它运行时独占 `/dev/video0`。
- `human-detect` 在运行：必须通过 `/run/human-detect.sock` 的
  `/snapshot.jpg` 接口取图。不得运行 `v4l2-capture`，也不得杀
  `human-detect` 来释放摄像头。
- `human-detect` 没运行：才可以直接使用 `/system/bin/v4l2-capture`。
- 拍照和必要路由放在同一条 `exec` 中，不要单独占工具轮次。

用户要保留的照片放 `/mnt/sd`；`/tmp` 是内存，不要长期保存大图。AI 自检用的
中间照片改用精确路径 `/tmp/weniclaw-photo-$$.jpg`，用完后执行
`rm -f /tmp/weniclaw-photo-$$.jpg`。默认 1024x768，只有用户明确要求高清时才
使用更高分辨率。

```sh
out=/mnt/sd/photo.jpg
if pidof human-detect >/dev/null 2>&1; then
  /usr/bin/curl --unix-socket /run/human-detect.sock -fsS \
    "http://localhost/snapshot.jpg?size=1024x768" -o "$out"
else
  /system/bin/v4l2-capture -f JPEG -s 1024x768 -o "$out"
fi
```

接口或 V4L2 返回非零、文件为空时，直接报告拍照失败。`human-detect` 正在
运行时，即使 socket 取图失败也不要降级为 `v4l2-capture`。
若返回 HTTP 409，表示 MJPEG 预览正在占用抓图切换；报告“预览占用摄像头”，
等预览客户端断开后再重拍，不要停止 `human-detect`。

## 支持的分辨率

两种路径都支持以下尺寸；默认使用 `1024x768`：

    160x120   176x144   320x240   640x480   720x480
    720x576   1024x768  1280x720  1920x1080 2592x1944

技能只使用彩色 JPEG。

## 成功条件

- `human-detect` 只在 JPEG 头尾完整时返回成功，外部不要再手工检查。
- `v4l2-capture` 会把不完整帧标为 `BAD` 并返回非零；不要发送失败命令生成的文件。
- 返回成功且文件非空即可发送，不要再运行 `head`、`tail`、`od` 或 `xxd` 做二次校验。
