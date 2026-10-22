# GPIO 与 LED

触发：点灯、闪灯、读写引脚、看按键状态时读本技能。

工具 `/bin/gpio`（按 gpiochip label 自动算 sysfs 号，不要自己写死数字）：

    gpio list                 # 各 bank 当前 base
    gpio num PB3              # 查某引脚的 sysfs 号
    gpio export PB3           # 申请（幂等）
    gpio unexport PB3         # 释放
    gpio dir PB3 out          # 方向：in|out
    gpio get PB3              # 读当前值
    gpio set PB3 0            # 输出（未导出会自动 export 并设 out）
    gpio info PB3             # direction/value/active_low/edge

（`gpio info` 在现有固件上成功时也可能返回 1，这是脚本退出码问题不是失败；
判断成败看输出内容，更新 rootfs 后消除。）

引脚名 PA0..PK15（每组 16 脚）。不是每个封装都有全部引脚。
读完本技能直接执行需要的命令，不要先做额外探测或信息收集。


## 板上三个用户 LED

LED 接 3V3 经限流电阻到引脚，三路都是**低电平点亮：0 亮，1 灭**。

| 颜色 | 引脚 | 点亮 | 熄灭 |
| --- | --- | --- | --- |
| 红色 | `PB3` | `gpio set PB3 0` | `gpio set PB3 1` |
| 绿色 | `PF10` | `gpio set PF10 0` | `gpio set PF10 1` |
| 蓝色 | `PI8` | `gpio set PI8 0` | `gpio set PI8 1` |

用户只说“点亮 LED”时点亮红色 `PB3`；明确说颜色时使用对应引脚。
三色全亮或全灭要放在同一条 exec 中：

    gpio set PB3 0; gpio set PF10 0; gpio set PI8 0    # 红绿蓝全亮
    gpio set PB3 1; gpio set PF10 1; gpio set PI8 1    # 红绿蓝全灭

闪指定颜色 3 次（红、亮 1 秒、灭 1 秒；`gpio set` 会自动 export 和设 out）：

    for i in 1 2 3; do gpio set PB3 0; sleep 1; gpio set PB3 1; sleep 1; done

三色依次点亮（每条 exec 内执行完整个序列）：

    gpio set PB3 0; sleep 1; gpio set PB3 1
    gpio set PF10 0; sleep 1; gpio set PF10 1
    gpio set PI8 0; sleep 1; gpio set PI8 1

**整个序列必须放在一条 exec 命令里执行**（每条消息工具轮次有限，
一次一条 `gpio set` 会把轮次用光）。板上 `sleep` 只收整数秒，没有
`usleep`，也没有小数秒延时；要更快只能靠 C 程序或改驱动，不要用
`sleep 0.5`、`read -t` 之类的写法，它们在这块板上不成立。

## 按键

KEY1 = PG3（低有效，按住为 0）。上电时按住 KEY1 会进入 USB MSC 烧录模式；
系统运行时可以当普通输入读：`gpio dir PG3 in && gpio get PG3`。

## 注意

- 输出状态会保持，不会自动复位；临时测试结束后熄灭用过的 LED。用户明确要求
  保持点亮时，按请求保留状态。
- exec 命令默认 10 秒超时：闪烁轮数要控制在超时内，超时命令会被终止。
