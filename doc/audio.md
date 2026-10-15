# N32H787 音频

板载 WM8978，通过 ALSA 卡 0、设备 0 录放音。用户态用 TinyALSA 工具，
不需要直接操作 I2C 或 codec 寄存器。

## 录音和回放

以下命令在板上执行，录 5 秒后播放，直接听录音是否正常：

```sh
tinycap /tmp/rec.wav -D 0 -d 0 -c 2 -r 48000 -b 16 -t 5
tinyplay /tmp/rec.wav -D 0 -d 0
```

参数含义：

| 参数 | 含义 |
| --- | --- |
| `-D 0 -d 0` | 使用卡 0、设备 0 |
| `-c 2` | 双声道 |
| `-r 48000` | 采样率 48000 Hz |
| `-b 16` | 每个采样点 16 bit |
| `-t 5` | 录 5 秒后停止；不写则录到 Ctrl-C |

**文件名必须紧跟命令，选项放在文件名后面。** 不要写成
`tinycap -t 5 rec.wav`，否则 `-t` 会被当作文件名。

`/tmp` 使用内存。上述录音约占 960 KB，只适合短录音；长录音请写到已挂载的
SD 卡目录。录音用完后可删除：

```sh
rm /tmp/rec.wav
```

## 播放自己的 WAV

WAV 必须是 **16-bit PCM、双声道**，采样率支持以下七档：

```text
8000、11025、16000、22050、32000、44100、48000 Hz
```

`tinyplay` 从 WAV 文件头读取格式，不会转换声道、位深或采样率。
给它加 `-c`、`-r`、`-b` 不能改变文件格式。单声道、8-bit 或其他采样率的
文件，需要在电脑上转换好再传到板上。

例如在电脑上通过 ADB 传入文件，再在板上播放：

```sh
# 电脑上执行
adb push tone.wav /tmp/tone.wav

# 板上执行
tinyplay /tmp/tone.wav -D 0 -d 0
```

也可以在电脑上用 SCP 传入，替换为板子的 IP：

```sh
scp tone.wav root@<ip>:/tmp/tone.wav
```

将录音取回电脑：

```sh
adb pull /tmp/rec.wav .
```

## 查看设备和调节音量

```sh
cat /proc/asound/cards        # 应能看到 WM8978 声卡
tinypcminfo -D 0 -d 0         # 查看设备支持的录放音参数
tinymix contents             # 查看音量、静音和输入输出通路控制
```

`tinymix` 列出控制项后，可按名称读取或设置。名称和值以板上实际输出为准：

```sh
tinymix get "控制项名称"
tinymix set "控制项名称" 值
```

## 使用限制与常见问题

- **录放音可以同时进行，但采样率必须一致。** 例如录音和播放都用
  16000 Hz；采样率不同，后打开的设备会失败。
- **录音启动约需 0.8 秒。** 这是驱动等待麦克风电路稳定，等待结束后才开始
  写入采样，不是在 WAV 开头补一段静音。
- **报 `cannot set hw params: Invalid argument`：** 先检查 WAV 是否符合上述
  格式；同时录放音时，再检查两边采样率是否一致。
- **播放命令崩溃：** 先用 `ls -l 文件路径` 确认文件存在。当前 TinyALSA 的
  `tinyplay` 在打不开文件时可能直接段错误退出。
- **命令结束但没有声音：** 检查实际连接的输出、音量和静音控制；命令返回成功
  不能代替听音检查。

## espeak-ng 直接说话（可选）

espeak-ng 不在默认固件里。按[可选组件说明](../sdk/optional/espeak-ng/README.md)
部署并配置启动命令后，可以直接合成并播放：

```sh
espeak-ng "Hello world."
espeak-ng -v cmn "你好，欢迎使用语音合成。"
espeak-ng -v en -s 150 "Hello world."  # -v 选择语言，-s 设置语速
```

这份移植的音频后端会把 22050 Hz 单声道语音转成双声道，直接送给声卡，
不需要先生成 WAV 再交给 `tinyplay`。词典缺失或连续内存不足时的检查方法见
可选组件说明；不要仅凭退出码判断中文语音是否正常。

TinyALSA 采用 BSD-3-Clause；许可文件位于板上的
`/usr/share/licenses/tinyalsa/`。
