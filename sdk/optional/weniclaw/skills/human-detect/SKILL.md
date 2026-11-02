# 人形检测服务

触发：用户说“打开端侧人员检测”“关闭端侧人员检测”，或要求开启/关闭人形
推理、询问是否有人、查看检测状态、抓取彩色画面时读本技能。

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

## 推理开关

用户说“打开端侧人员检测”“打开人形检测、开启人员推理、开始识别是否有人”
时，开启推理并立即复查状态：

```sh
/usr/bin/curl --unix-socket /run/human-detect.sock -fsS -X POST \
  'http://localhost/inference?enabled=1'
sleep 2
/usr/bin/curl --unix-socket /run/human-detect.sock -fsS \
  http://localhost/status
```

用户说“关闭端侧人员检测、关闭人形推理、停止人员识别”时，把 `enabled=1`
改成 `enabled=0`。
成功条件是响应和随后 `/status` 都显示 `inference_enabled` 为目标值；开启后
还应看到 `frames` 增长，`have_result` 变为 `true`。只有
`camera_frames` 增长而 `frames` 不变，表示相机仍在出图但推理没有运行。

推理开关只控制模型是否执行。关闭推理不关闭相机、预览或人形事件通知；停止
进程才使用 `killall human-detect`，不要混用这三个操作。

```sh
/usr/bin/curl --unix-socket /run/human-detect.sock -fsS \
  "http://localhost/snapshot.jpg?size=1024x768" -o /mnt/sd/human.jpg
```

默认 `size=1024x768`。可选：

`160x120 176x144 320x240 640x480 720x480 720x576 1024x768 1280x720 1920x1080 2592x1944`

抓图成功后服务会自动恢复 GREY 检测。需要预览模型实际看到的画面时，用
`http://<板子IP>:8080/preview.mjpg?fps=15&duration=30`；预览固定为
320x240 灰度并带检测框，同时只允许一个客户端。HTTP 非 2xx、空文件或 socket
不存在时直接报告服务不可用，不要改走 V4L2。

## 通知开关

用户说“别再发人员检测通知”“关闭人形检测提醒”时，调用
`event_notifications`，参数为 `action=disable`、`source=human-detect`。用户
要求恢复时用 `enable`，询问是否开启时用 `status` 并带上同一个 `source`。关闭
只会停止 `human-detect` 程序的直发微信文字和照片；检测、最近事件上下文和
照片文件仍会更新。重新开启后不补发关闭期间的事件，也不要用其他硬件动作或杀
进程代替这个开关。
