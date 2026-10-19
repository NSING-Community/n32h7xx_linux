# 人形检测服务

触发：用户询问是否有人、查看检测状态或抓取彩色画面时读本技能。

后台服务独占 `/dev/video0`，周期以 320x240 GREY 做带位置框的人形检测，
同时提供本地 Unix socket。不要直接访问 `/dev/video0`，也不要启动
`v4l2-capture`。这里的模型会返回位置框，不是 TFLite 的纯二分类模型。

服务未运行时先启动它：

```sh
setsid /system/bin/human-detect --socket /run/human-detect.sock \
  --http-port 8080 >>/tmp/human-detect.log 2>&1 </dev/null &
sleep 1
pidof human-detect >/dev/null
```

```sh
/usr/bin/curl --unix-socket /run/human-detect.sock -fsS \
  http://localhost/status
```

`human_detected` 表示最近一帧是否检测到人，`frames` 应持续增长；返回的
`boxes` 是相对整幅画面的 16 位坐标。查看实时彩色画面时抓一张 JPEG：

```sh
/usr/bin/curl --unix-socket /run/human-detect.sock -fsS \
  "http://localhost/snapshot.jpg?size=1024x768" -o /mnt/sd/human.jpg
```

默认 `size=1024x768`。可选：

`160x120 176x144 320x240 640x480 720x480 720x576 1024x768 1280x720 1920x1080 2592x1944`

抓图成功后服务会自动恢复 GREY 检测。需要预览模型实际看到的画面时，用
`http://<板子IP>:8080/preview.mjpg?fps=5&duration=30`；预览固定为
320x240 灰度并带检测框，同时只允许一个客户端。HTTP 非 2xx、空文件或 socket
不存在时直接报告服务不可用，不要改走 V4L2。
