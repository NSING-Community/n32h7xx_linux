# 音频：麦克风录音与喇叭播放（WM8978）

触发：录音、放音、喇叭/线路输出测试、放提示音时读本技能。

声卡 0、设备 0；仅支持 16-bit、2 声道；采样率仅
8000 11025 16000 22050 32000 44100 48000 Hz。
读完本技能直接执行需要的命令，不要先做额外探测或信息收集。


## 录音（麦克风）

    tinycap /mnt/sd/rec.wav -D 0 -d 0 -c 2 -r 16000 -b 16 -t 5

- 文件名必须在最前，选项在后；`-t` 是秒数，不写则录到 Ctrl-C。
- 启动后约 0.8 秒才开始写采样（驱动等麦克风偏置稳定），正常现象。
- 一条 exec 命令默认最长 10 秒，`-t` 录超过 8 秒会被超时终止；更长录音要用户直接在串口/SSH 跑。
- 长录音放 /mnt/sd：16 kHz 双声道约 64 KB/s，48 kHz 约 192 KB/s；
  /tmp 是内存，只放短录音。

## 播放（喇叭 / LINE-OUT）

    tinyplay /mnt/sd/rec.wav -D 0 -d 0

- WAV 必须 16-bit 双声道；不符会报 `cannot set hw params: Invalid argument`。
  单声道、8-bit、22050 Hz 等文件先在电脑转换好再传到板上。
- 喇叭和 LINE-OUT 同时输出，没有插入检测。
- 缺文件时 tinyplay 会直接段错误退出；确认和播放放同一条命令：
  `ls -l /mnt/sd/rec.wav && tinyplay /mnt/sd/rec.wav -D 0 -d 0`。
- 命令返回成功不等于真的有声音，要听音确认。

## 音量与开关（tinymix）

    tinymix contents                                  # 全部 64 项
    tinymix set "Speaker Playback Volume" 57          # 0..63
    tinymix set "Headphone Playback Volume" 57        # LINE-OUT
    tinymix set "PCM Volume" 255
    tinymix set "Speaker Switch" On

## 同时录放

可以并行，但两边采样率必须一致，否则后打开的一方失败。
