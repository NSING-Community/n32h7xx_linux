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
| `weniclaw` | ARM 二进制（已 strip，507112 字节） |
| `skills/` | 四份现成外设技能（拍照、录音放音、点灯、存储位置） |
| `LICENSE`、`THIRD_PARTY_NOTICES.md`、`licenses/` | 随二进制附带的声明文本 |

## 运行要求

板上不需要额外安装任何东西，只要本 SDK 的 rootfs 已提供：

- uClibc 运行库（`/lib/libc.so.0`）；
- rootfs 自带在 `/usr/lib` 的三个 Mbed TLS 共享库：
  `libmbedtls.so.21`、`libmbedx509.so.7`、`libmbedcrypto.so.16`；
- HTTPS 使用的 CA 证书包 `/etc/ssl/certs/ca-certificates.crt`；
- 可用的网络（DHCP 或手动 `ifconfig`），以及正确的系统时间（依赖 RTC）。

动态加载器的默认搜索路径已覆盖 `/lib` 和 `/usr/lib`，启动不需要设置
`LD_LIBRARY_PATH`；也不需要特殊内核选项，程序本身只是用户态网络加上一个
供模型调用的 shell 命令工具。

内存方面：二进制是 FDPIC，no-MMU 加载器按段分别映射，不需要几 MB 的连续
内存。实板实测：最大单块映射 479232 字节（468 KiB），正好落在板子 512 KiB
的伙伴块内；含 libc 和三个 Mbed TLS 库在内的总映射约 1.8 MB；微信通道空闲
时 `/proc/<pid>/status` 报告的驻留内存（`Mem:`）约 750 KiB。

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
  nohup /system/bin/weniclaw --config /system/weniclaw/weniclaw.json wechat run \
    >> /system/weniclaw/wechat.log 2>&1 </dev/null &
fi
```

本板 no-MMU 的 shell 只能后台化真正的可执行文件，请保持这个写法，不要包成
复合命令。日志写在 `/system` 会占用 NOR 空间；不想保留可以改到 `/tmp`。

## 技能：外设操作说明

`skills/` 里的四份技能教模型怎么用板上外设，安装到
`/system/weniclaw/workspace/skills/`（`../install.sh` 会自动拷过去）。没有
技能程序照常运行，只是模型不知道这些设备怎么用。技能是纯文本，直接用编辑器
按板的实际情况改；要加自己的，在 `skills/` 下新建目录放 `SKILL.md`。

system 提示里只出现技能路径，正文由模型按需读取，平时不占内存。

## 限制

- 微信通道只处理已配对用户的私聊文本消息；暂不支持群聊和媒体。
- 对话历史保存在内存里，重启即丢失。命令工具默认允许 agent 访问当前用户
  可达的任意路径和命令。
- 配置页面是本程序唯一监听的端口；微信通道只发起出站 HTTPS 请求。

