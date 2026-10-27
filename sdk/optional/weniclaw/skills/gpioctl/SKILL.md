# GPIO 与板载 LED

触发：点灯、关灯、闪灯、读写 GPIO，或查看按键状态时读本技能。

你拥有 `exec_command`，可以直接执行 shell。不要先运行环境探测，读完本技能
后直接把需要的命令放进同一条 `exec_command` 执行。

## 板载 LED

`gpioctl` 会自动选择后端：目标与 `/sys/class/leds/<label>` 完全匹配时写
标准 LED brightness，否则才按 GPIO 名解析。不要自己把颜色换算成 GPIO，
也不要在命令中写死 `PB3`、`PF10` 或 `PI8`；DTS 改引脚后 label 仍可继续使用。

| 颜色 | label | 点亮 | 熄灭 |
| --- | --- | --- | --- |
| 红色 | `red` | `/system/bin/gpioctl set red 1` | `/system/bin/gpioctl set red 0` |
| 绿色 | `green` | `/system/bin/gpioctl set green 1` | `/system/bin/gpioctl set green 0` |
| 蓝色 | `blue` | `/system/bin/gpioctl set blue 1` | `/system/bin/gpioctl set blue 0` |

用户只说“点亮 LED”时用红色；明确说了颜色时使用对应颜色。一次设置多路时可
依次写入：

    /system/bin/gpioctl set red 1; \
    /system/bin/gpioctl set green 1; \
    /system/bin/gpioctl set blue 1

一次点亮会保持输出，退出后灯不会自动熄灭，也不要在后面追加 `sleep`、
定时关闭或补偿性关灯。只有用户明确要求关闭或指定持续时间时，才写入亮度
`0` 或设置定时器。

## 闪烁与呼吸

LED GPIO 只有亮、灭两档，没有硬件 PWM。需要软件闪烁、PWM 或呼吸效果时，
直接让 `gpioctl` 操作 LED label；命令会先关闭已有 trigger，结束后熄灭 LED：

    /system/bin/gpioctl blink blue --period-ms 500 --duration-ms 5000
    /system/bin/gpioctl pwm red 30 --hz 100 --duration-ms 3000
    /system/bin/gpioctl breathe green --cycle-ms 3000 --duration-ms 10000

如果只需要内核 timer trigger，也可以直接写标准属性。时间是亮、灭各自的毫秒数：

    echo timer > /sys/class/leds/red/trigger
    echo 500 > /sys/class/leds/red/delay_on
    echo 500 > /sys/class/leds/red/delay_off

停止闪烁并熄灭：

    echo none > /sys/class/leds/red/trigger
    echo 0 > /sys/class/leds/red/brightness

## 查询

    /system/bin/gpioctl get red
    /system/bin/gpioctl info red

label 不明确时才运行 `/system/bin/gpioctl list`；已知 `red`、`green`、`blue`
时不要再做环境探测。

## 其他 GPIO

LED 之外的调试 GPIO 也使用 `/system/bin/gpioctl`。`--active-low` 只对 GPIO
目标有意义；LED 亮度已经是逻辑值，极性由 DTS 决定：

    /system/bin/gpioctl list
    /system/bin/gpioctl info PG3
    /system/bin/gpioctl get PG3

## 按键

`KEY1` 是 `PG3`，低有效，按住时为 0。上电时按住会进入 USB MSC；系统运行
时可以直接读取，不需要额外探测：

    /system/bin/gpioctl get PG3

## 注意

- `gpioctl set/release` LED 时会先写 `trigger=none`，再设置 brightness。
- 多个 LED 可以分别设置不同 timer；不需要启动后台用户态进程。
- 命令成功返回后，简短报告颜色、动作和是否保持。不要用 `od`、`xxd` 或其他
  二次检查验证 LED。
