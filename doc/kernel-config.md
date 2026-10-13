# 定制内核配置（menuconfig）

内核是 **out-of-tree** 构建，工作配置放在 `out/kernel/build/.config`，不在源码
树里。所有配置命令都走顶层 `make`，不需要进 `kernel/` 目录敲 `make
menuconfig`。

## 界面依赖

[环境搭建](setup.md) 已包含 `libncurses-dev`，用于 `menuconfig` / `nconfig`。
使用 `xconfig` 时另外准备 Qt 开发包。纯命令行编译不需要菜单界面。

## 改配置

```sh
make menuconfig         # ncurses 菜单（等价于 make kernel-menuconfig）
make kernel-nconfig     # 新版 ncurses UI
make kernel-xconfig     # Qt 图形界面（需要 Qt 开发包）
```

改完在界面里保存退出即可。保存的就是 `out/kernel/build/.config`，下次
`make kernel` 会**保留**你改过的配置来编，不会再被出厂 defconfig 覆盖。

## 配置会被保留

出厂配置 `kernel/arch/arm/configs/nsing_n32h7xx_defconfig` 只当**种子**用：

- 还没有 `.config`（第一次编译）→ 用它初始化一份 `.config`。
- 你已经 `menuconfig` 改过 → 之后 `make kernel` 一直用你的 `.config`。
- 发布方更新了 defconfig，而你的 `.config` **没动过** → 自动跟着更新（这样
  `git pull` 后普通 `make` 能拿到新符号）。
- 发布方更新了 defconfig，而你的 `.config` **有你的改动** → 保留你的配置，
  并打印一条提示，列出下面几条命令。

## 其他命令

```sh
make kernel-oldconfig       # 内核升级后，逐个确认新增的配置符号
make kernel-savedefconfig   # 把当前配置导出成精简 defconfig
make kernel-defconfig       # 丢弃改动，回到出厂配置
```

`make kernel-savedefconfig` 会在 `out/kernel/build/defconfig` 生成一份精简
defconfig。想长期维护一套自己的板级配置，把它放进内核源码树并命名：

```sh
cp out/kernel/build/defconfig kernel/arch/arm/configs/myboard_defconfig
make kernel DEFCONFIG=myboard_defconfig
```

也可以把 `DEFCONFIG` 直接指到一个文件：

```sh
make kernel DEFCONFIG=/path/to/myboard_defconfig
```

`make kernel-defconfig`（内部给 `kconfig.sh` 传 `reset`）是唯一的"回到出厂"
入口，会丢弃你的改动再重新种一份 `.config`。

## 清一次再重来

`out/kernel/build` 里除了 `.config`，还有两个标记文件（`.config-seed`、
`.config-seeded`）用来区分"发布方换了 defconfig"和"你自己改了配置"。想彻底
重来，直接删掉构建目录即可：

```sh
rm -rf out/kernel/build
make kernel
```

## 不要直接改源码树

不要进 `kernel/` 敲 `make menuconfig`：那会在源码树里写一个 `.config` 和
`include/config/`，把 out-of-tree 构建搞脏。所有配置都从顶层 `make` 走。
