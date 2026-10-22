# WeniClaw — 可选组件

面向 N32H787（Cortex-M7、no-MMU Linux、uClibc、ELF FDPIC）的预编译
`weniclaw` 二进制。它是一个小型的实验性文本 agent：既可以从命令行调用
OpenAI 兼容的对话接口，也可以通过腾讯 iLink Claw 通道与微信配对，回复私聊
文本消息。

**本组件不属于任何镜像。** 它不在 `rootfs/` 里，`make` 不会编译或安装它，
`make flash-image` 也不会把它带进板子；它在 `/system` 下写入的配置和配对
状态同样不属于任何镜像。是否安装、装到哪里，由你自己决定。

| 文件 | 说明 |
| --- | --- |
| `weniclaw` | ARM 二进制（已 strip，243504 字节） |
| `skills/` | 五份现成外设技能（拍照、人形检测服务、录音放音、点灯、存储位置） |
| `LICENSE`、`THIRD_PARTY_NOTICES.md`、`licenses/` | 随二进制附带的声明文本 |

## 运行要求

板上不需要额外安装任何东西，只要本 SDK 的 rootfs 已提供：

- uClibc 运行库（`/lib/libc.so.0`）；
- 公共 FDPIC libcurl（`/usr/lib/libcurl.so.4`）；
- rootfs 自带在 `/usr/lib` 的三个 Mbed TLS 共享库：
  `libmbedtls.so.21`、`libmbedx509.so.7`、`libmbedcrypto.so.16`；
- HTTPS 使用的 CA 证书包 `/etc/ssl/certs/ca-certificates.crt`；
- 可用的网络（DHCP 或手动 `ifconfig`），以及正确的系统时间（依赖 RTC）。

动态加载器的默认搜索路径已覆盖 `/lib` 和 `/usr/lib`，启动不需要设置
`LD_LIBRARY_PATH`；也不需要特殊内核选项，程序本身只是用户态网络加上一个
供模型调用的 shell 命令工具。

内存方面：二进制是 FDPIC，no-MMU 加载器按段分别映射，不需要几 MB 的连续
内存。本版 ELF 的最大单块映射为 229376 字节（224 KiB），进程栈上限为
131072 字节（128 KiB）。公共 libcurl 本身的最大单段为 346176 字节，需要
一块 512 KiB 的 Buddy 块；TLS 握手和运行期缓冲区还会另外占用内存。

## 安装

`../install.sh` 是本目录的一键安装脚本（它也会安装其他可选组件）：用 `USE`
选择传输方式（默认 `adb`；走 `ssh` 时加 `BOARD=<板子IP>`）。脚本会把二进制
复制到可写的 JFFS2 分区 `/system/bin`，内核和 rootfs 升级后依然保留。
手动安装：

```sh
scp sdk/optional/weniclaw/weniclaw root@<板子IP>:/system/bin/
ssh root@<板子IP> 'chmod +x /system/bin/weniclaw'
```

注意：板上的非交互 `ssh`/`adb` shell 的 `PATH` 是
`/usr/sbin:/usr/bin:/sbin:/bin`，**不包含** `/system/bin`，所以命令要写全
路径：`/system/bin/weniclaw`。

## 首次运行：配置页面

发行包里不带任何配置。只要首次启动状态不完整——还没有配置文件，或配置文件
存在但微信配对还没完成——`wechat run` 会先提供一个配置页面。页面默认监听
80 端口（需要 root 运行；`weniclaw setup 8080` 可换端口），且只在需要时
存在：配置和配对都齐了以后，`wechat run` 直接启动微信通道，不再开任何端口。

页面地址会打印在串口控制台，同时写入 `/tmp/weniclaw-setup.url`（串口内容
滚掉时以这个文件为准）：

```text
weniclaw setup: open http://192.168.1.12/ in a browser on the same network
```

页面本身没有登录，同一局域网内任何人都能打开；请在可信网络下使用，配置完
尽快结束。页面上的步骤：

1. **大模型接口** —— 填 API 地址（OpenAI 兼容的 `/v1` 基地址）、模型名和
   API Token，点 **保存**。这些内容先暂存为 `<配置>.new`，此时还不生效；
   Token 不会被回显到页面上；出现“已保存过 Token，留空表示不修改”的提示
   后，留空即可沿用已暂存的值。
2. **微信配对** —— 点 **获取二维码**，页面会在新标签页打开腾讯 iLink 的
   配对页面（若被拦截，可用同页的“打开配对二维码”链接）。用手机微信扫码
   并确认。如果微信上显示了一串数字，页面会出现 **手机上显示的数字** 输入
   框，填进去点 **提交**。每步页面都做约 30 秒的服务端长轮询，请给每步一点
   时间；二维码约 120 秒有效，过期可点 **获取二维码** 重新获取。等到出现
   **配对成功**。
3. **完成配置** —— 唯一把暂存配置改名生效并关闭页面的动作。此步配对可选：
   跳过第 2 步也能保存配置，但微信通道不会回复任何消息，下次 `wechat run`
   还会再次打开这个页面。

### 终端配对

不用浏览器也能做同样的配对。需要配置文件已经存在（由上面的页面生成，或
手工编辑模板），且终端路径不会自动建目录，请先创建：

```sh
mkdir -p /system/weniclaw
/system/bin/weniclaw --config /system/weniclaw/weniclaw.json wechat login
```

它会打印 iLink 配对链接（在浏览器打开，或自行编码成二维码），等待扫码；
如果手机显示数字，会在 stdin 提示
`Enter the verification code shown on your phone:`。成功后写入配对状态
（0600），并打印接下来要用的 `wechat run` 命令。

需要重新配置时，删除文件后重启：

```sh
rm /system/weniclaw/weniclaw.json        # 只清模型/Token，配对保留
rm /system/weniclaw/wechat-state.json    # 连同微信配对一起重做
reboot
```

## 运行

```sh
/system/bin/weniclaw --config /system/weniclaw/weniclaw.json ask '你好'
/system/bin/weniclaw --config /system/weniclaw/weniclaw.json chat
/system/bin/weniclaw --config /system/weniclaw/weniclaw.json wechat run
```

配置、配对状态和 shell 命令工具的工作目录都在 `/system/weniclaw/`；配置
页面会自己创建该目录及其 `workspace/`，两个 JSON 文件都以 0600 写入。其他
命令不会创建该目录，所以上面和下面的脚本都保留了 `mkdir -p`。同一状态文件
同时只允许一个 `wechat run` 进程（第二个会以 `WechatAlreadyRunning` 退出）。

开机自启：把下面这段加进用户钩子 `/system/bin/init.sh`。请保持无条件调用，
因为“没有配置”或“配对不完整”正是需要提供配置页面的情况：

```sh
if [ -x /system/bin/weniclaw ]; then
  mkdir -p /system/weniclaw
  setsid /system/bin/weniclaw --config /system/weniclaw/weniclaw.json wechat run \
    >> /system/weniclaw/wechat.log 2>&1 </dev/null &
fi
```

本板 no-MMU 的 shell 只能后台化真正的可执行文件，请保持这个写法，不要包成
复合命令。启动脚本退出时可以保留的常驻服务应使用 `setsid`，不要只依赖
`&` 或 `nohup`。`wechat.log` 会记录用户请求、简短推理、工具动作和最终回复。
程序在每次记录前以及服务轮询期间定期检查文件，超过 128 KiB 就清空旧内容，
避免长期占满 NOR 空间；不想保留也可以改到 `/tmp`。

## AI 定时任务

微信通道运行后，可以直接用自然语言让 AI 创建、查询、暂停、恢复、取消或立即
执行定时任务。例如：

```text
30 分钟后提醒我检查设备温度
明天 09:00 汇总一次系统状态
每 2 小时检查一次 SD 卡剩余空间
列出当前定时任务
取消任务 <id>
查看定时任务历史
```

任务由 AI 调用 `scheduled_tasks` 工具管理，保存在配置目录下的 `schedules/`。
支持三种时间写法：

| 类型 | 示例 | 说明 |
| --- | --- | --- |
| 一次性延迟 | `in: 30m` | 最短 10 秒 |
| 一次性绝对时间 | `at: 2026-10-23T09:00:00+08:00` | RFC3339；AI 会先读取当前时间 |
| 固定间隔 | `every: 2h` | 最短 60 秒 |

`list` 只返回当前任务；已执行、最终失败和已取消的任务会进入历史记录，需用
`history` 查看。历史记录用于诊断并受 32 条总记录上限约束，任务完成后即使
记录仍在，也不会再次触发。

到期任务使用一次全新的、无历史 Agent 会话执行，因此仍可调用命令、文件和微信
发送工具；执行结束后立即释放该会话。结果会主动发送到已经配对的微信私聊。
任务不会另起进程或线程，调度循环复用 `wechat run` 本身；所以 `wechat run`
必须保持运行，设备关机期间不会执行任务。

执行失败最多重试 3 次，间隔约 30 秒、5 分钟，最终失败会发送一条微信通知。
重启后，未到期的一次性任务会继续等待；周期任务不会补跑关机期间错过的全部
次数，只执行最近一次到期任务并推进到下一次。当前限制为最多 16 个活动任务、
32 条记录，任务提前量不超过 366 天，说明最长 4096 字节。

系统时间不正确会直接影响 `at` 任务和所有间隔计算。请先确认 RTC/NTP 已把时间
校准；当前版本不接受五字段 cron 表达式。固定钟点可用 `at` 创建下一次任务；
固定间隔则从任务创建时间开始计算。

## 本机任务接口

`wechat run` 运行期间还会在 `/run/weniclaw/control.sock` 提供一个仅本机使用
的 HTTP/JSON 接口。它复用同一个 WeniClaw 进程，不会另起常驻进程或线程；
参数由配置文件中的 `control_socket` 修改。socket 权限为 0600，仅 root 可访问。

接口：

| 方法和路径 | 用途 |
| --- | --- |
| `POST /v1/tasks` | 持久化并排队一个任务 |
| `GET /v1/tasks/<id>` | 查询任务状态和结果 |
| `GET /v1/capabilities` | 查询支持的步骤、通道和限制 |
| `GET /v1/health` | 查询本机任务服务是否就绪 |

查询服务状态：

```sh
/usr/bin/curl --unix-socket /run/weniclaw/control.sock \
    -sS http://localhost/v1/health
```

提交一个直接发送微信的任务：

```json
{
  "schema": "weniclaw.task.v1",
  "source": "my-app",
  "event": "device.alarm",
  "payload": {
    "text": "设备温度过高"
  },
  "pipeline": [
    {
      "type": "deliver",
      "channel": "wechat",
      "template": "{{payload.text}}"
    }
  ],
  "idempotency_key": "alarm-20261022-001",
  "dedupe": {
    "window_s": 300
  }
}
```

```sh
/usr/bin/curl --unix-socket /run/weniclaw/control.sock \
    -H 'Content-Type: application/json' --data-binary @/tmp/task.json \
    -sS http://localhost/v1/tasks
```

成功返回 `202 Accepted` 表示任务已经写入 `/system/weniclaw/queue`，之后由
`wechat run` 按顺序执行。返回值中的 `id` 可用于查询：

```sh
/usr/bin/curl --unix-socket /run/weniclaw/control.sock \
    -sS http://localhost/v1/tasks/<id>
```

任务状态为 `queued`、`running`、`waiting_retry`、`delivered`、`failed` 或
`deduplicated`。执行失败最多尝试 3 次；队列最多保存 32 条记录，其中最多 16
条活动任务。相同 `source + event` 在 `dedupe.window_s` 内只执行一次；设置
`idempotency_key` 后，相同 `source + event + idempotency_key` 会返回原任务。

`pipeline` 最多 4 步，其中最多一个 `agent` 步骤。`agent` 使用 OpenAI 兼容接口
做一次无历史、无工具的文本处理，结果通过 `{{agent.output}}` 传给后面的微信
发送步骤。例如：

```json
{
  "type": "agent",
  "preset": "alarm-summary",
  "prompt": "用一句中文概括事件：{{payload.text}}"
}
```

任务做的是至少一次投递，不是严格恰好一次：微信发送成功之后，如果进程在保存
完成状态前掉电，重启后仍可能再发送一次。对重复敏感的业务应把
`idempotency_key` 设置为稳定值，并自行处理极小概率的重复通知。任务在现有
微信循环中串行执行，不要用它承载高频或长时间计算。

## 相机服务

拍照技能依赖 `human-detect` 服务。它独占 `/dev/video0`，持续用
320x240 GREY 做带位置框的人形检测，并通过 `/run/human-detect.sock` 提供状态、
灰度预览和彩色 JPEG。它不同于 TFLite 示例里的 `person_detect` 二分类模型。
`human-detect` 也是可选组件，不在标准固件中。安装：

```sh
BOARD=<板子IP> USE=ssh sh sdk/optional/human-detect/install.sh
```

手动启动和停止：

```sh
setsid /system/bin/human-detect --socket /run/human-detect.sock \
    --http-port 8080 >>/tmp/human-detect.log 2>&1 </dev/null &
pidof human-detect
killall human-detect
```

服务运行时，智能体只通过 Unix socket 取图；不要再运行
`v4l2-capture` 访问同一个相机。

`human-detect` 检测到“无人到有人”的变化时，会向本机任务接口提交
`human.detected` 事件。`wechat run` 会发送检测结果和事件 JPEG，同时把人数、
时间、检测框、图片路径等写入 workspace 的 `latest-event.json`。后续微信追问
会自动带上这次事件上下文，并在需要看画面时通过 `read_file` 懒加载
`latest-human.jpg`。本地和服务端都会做 5 分钟抑制；WeniClaw 未启动或接口暂时
不可用时，检测服务每 30 秒重试，摄像头检测和预览仍可正常使用。可用
`--task-socket PATH` 修改接口路径。

## 技能：外设操作说明

技能是纯文本 Markdown，一个技能一个目录，位置固定在 workspace 下：

```text
/system/weniclaw/workspace/skills/<名称>/SKILL.md
```

（workspace 取自配置文件的 `workspace` 字段；标准安装即
`/system/weniclaw/workspace`。）本目录的 `skills/` 里有五份现成技能：
拍照 `camera`、人形检测服务 `human-service`、录音放音 `audio`、点灯
`gpio-led`、存储位置 `storage`。

`../install.sh` 会自动安装技能；手动安装就是拷贝这个目录：

```sh
mkdir -p /system/weniclaw/workspace
scp -r sdk/optional/weniclaw/skills root@<板子IP>:/system/weniclaw/workspace/
```

加自己的技能：在 `skills/` 下新建 `<名称>/SKILL.md`，可以在这里编辑好后
拷到板上，也可以让 agent 用 write_file 直接写。名称只用字母、数字、`_`、
`-`（不要中文或空格），最多列出 16 个技能。每条消息都会重新扫描技能目录，
改完不用重启。没有技能程序照常运行，只是模型不知道这些设备怎么用。

模型只拿到技能路径清单，正文按需读取，平时不占内存。

## 限制

- 微信通道只处理已配对用户的私聊文本消息；暂不支持群聊和媒体。
- 对话历史保存在内存里，重启即丢失。命令工具默认允许 agent 访问当前用户
  可达的任意路径和命令。
- 配置页面启用时监听 HTTP 端口；微信通道和本机任务接口不监听局域网 TCP，
  只发起出站 HTTPS 请求。

