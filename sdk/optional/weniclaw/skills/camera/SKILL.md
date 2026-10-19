# 摄像头彩色抓图

触发：用户要求拍照、抓帧或查看当前画面时读本技能。

`/dev/video0` 由后台 `human-detect` 服务独占。不要直接运行
`v4l2-capture`，也不要探测 `/dev/video0`；统一通过 Unix socket 取图。

默认抓 1024x768，文件放 `/mnt/sd`。读完直接执行，不要先做额外检查。

```sh
/usr/bin/curl --unix-socket /run/human-detect.sock -fsS \
  http://localhost/status

/usr/bin/curl --unix-socket /run/human-detect.sock -fsS \
  "http://localhost/snapshot.jpg?size=1024x768" -o /mnt/sd/photo.jpg
```

- 只有用户明确要高清时才请求 `1920x1080` 或 `2592x1944`。
- 支持尺寸：`160x120 176x144 320x240 640x480 720x480 720x576 1024x768 1280x720 1920x1080 2592x1944`。
- 接口返回的是彩色 JPEG。HTTP 非 2xx 或文件为空说明服务当前无法抓图，直接报告错误，不要改用 `v4l2-capture` 抢设备。
- 低成本实时预览地址为 `http://<板子IP>:8080/preview.mjpg?fps=5&duration=30`；它是 320x240 灰度检测画面，不适合作为高清照片交付。
