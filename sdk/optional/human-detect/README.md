# 人形检测服务

`human-detect` 是可选的摄像头应用，持续做 320x240 灰度人形位置检测，并提供
彩色抓图、灰度 MJPEG 预览和浏览器仪表盘。它不属于固件镜像；安装后位于可写的
`/system/bin`，升级 kernel 或 rootfs 不会丢失。

使用前先构建 `CAMERA=1` 的固件，让内核提供 OV5640、DVP 和 `/dev/video0`。
检测服务会独占摄像头，运行时不要再启动 `v4l2-capture`。

## 安装

SSH：

```sh
BOARD=<板子IP> USE=ssh sh sdk/optional/human-detect/install.sh
```

ADB：

```sh
sh sdk/optional/human-detect/install.sh
```

安装脚本会停止旧实例、替换 `/system/bin/human-detect` 并启动新实例。它不会
注册开机自启，重启后需要按下面的命令手动启动。

## 使用

浏览器打开：

```text
http://<板子IP>:8080/
```

首页显示实时灰度画面、检测框、人物状态、推理帧率、推理耗时、累计帧数、运行
时间和错误计数。MJPEG 同时只允许一个预览客户端。

后台启动：

```sh
setsid /system/bin/human-detect --socket /run/human-detect.sock \
    --http-port 8080 >>/tmp/human-detect.log 2>&1 </dev/null &
pidof human-detect
```

停止：

```sh
killall human-detect
pidof human-detect || echo stopped
```

日志位于 `/tmp/human-detect.log`。修改 `--http-port` 可以更换 HTTP 端口。

## 接口

| 接口 | 内容 |
| --- | --- |
| `/` | HTML 仪表盘 |
| `/status` | 相机、推理、检测框和计数器 JSON |
| `/preview.mjpg?fps=5` | 320x240 灰度 MJPEG，连接到客户端断开为止 |
| `/snapshot.jpg?size=1024x768` | 彩色 JPEG；抓图时预览会暂时不可用 |

抓图支持 `160x120`、`176x144`、`320x240`、`640x480`、`720x480`、`720x576`、
`1024x768`、`1280x720`、`1920x1080` 和 `2592x1944`。

## 卸载

```sh
killall human-detect
rm /system/bin/human-detect
```
