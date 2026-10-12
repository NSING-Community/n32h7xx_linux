# 通过网络更新分区（SSH OTA）

设备启动后自己就是一个 Linux：有网络、有 SSH、能用标准 MTD 接口擦写外部
SPI NOR。所以更新分区不需要拆机、不需要探针，也不需要把板子切成 USB 存储设备。

本文讲三件事：更新 rootfs、更新里面的应用程序，以及更新 kernel / dtb。

## 前提

- 设备已启动，网络可达，SSH 能登录（默认用户名 `root`）。
- 主机侧有本仓库（`make ota-rootfs` 在这里）。
- 设备上存在 `/sbin/n32-flash` 或 `/system/bin/n32-flash`。两者都没有时，
  先 `make tools` 编出来，再 scp 进可写的 `/system/bin/`
  （这条不依赖 rootfs 已被更新，正是为了打破"要更新 rootfs 得先有工具"的循环）：

  ```sh
  make tools
  scp rootfs/sbin/n32-flash root@<板子IP>:/system/bin/
  ```

  这里的 `make tools` 指**板子侧**的 `n32-flash`，
  和主机侧的 `tools/` 目录不是一回事（后者见
  [tools.md](tools.md)）。

先确认设备在线：

```sh
ssh root@<板子IP> 'uname -a; cat /proc/mtd'
```

`/proc/mtd` 应该列出 `dtb`、`kernel`、`rootfs`、`system` 四个分区。这个列表就是
后面命令里 `PARTITION` 参数的取值。

## 更新 rootfs

### 一条命令

在仓库根目录：

```sh
make ota-rootfs H787_BOARD=root@<板子IP>
```

它做三件事：把 `out/rootfs/rootfs.squashfs` 打包好 → 把镜像通过 SSH 管道直接
喂给设备上的 `n32-flash ota -` → 设备边收边擦写，每 32 KiB 回读校验，然后重启。

镜像**不在设备上落地**：设备全程只用 32 KiB 缓冲，跟镜像多大无关，也不用先腾出
一块内存来装它。这是 `make ota-*` 选管道的原因。想先拷到 `/tmp` 再刷也可以，
见下面的"手动做：先拷到 /tmp 再刷"——只是那种做法要花的是设备内存。

`H787_BOARD` **没有默认值**：板子用 DHCP，地址每次启动都可能变，写死一个只会
在别人的机器上连错地方。先在串口控制台敲 `ifconfig eth0` 确认当前地址，再传给
`make`；也可以 `export H787_BOARD=root@<板子IP>` 后直接 `make ota-rootfs`。
不填会直接报错提示。

root 密码为空时，`ssh` 在非交互环境下不会自己弹密码提示，需要一个 askpass 垫片：

```sh
printf '#!/bin/sh\necho ""\n' >/tmp/askpass.sh; chmod +x /tmp/askpass.sh
H787_SSH_ASKPASS=/tmp/askpass.sh make ota-rootfs H787_BOARD=root@<板子IP>
```

### 输出长什么样

```text
==> root@<板子IP>: streaming <仓库根>/out/rootfs/rootfs.squashfs (1433600 bytes) into rootfs
    sha256 a151b49b503b9eb3da9e10fe8071b6006436a82d1e04e639bf8ba6b718189cd4
==> flashing rootfs in place and rebooting
n32-flash: OTA stdin -> rootfs (/dev/mtd2) offset 0x0
  image   SquashFS superblock ok
  note    this replaces the live "rootfs" in place, programming as the
          bytes arrive; an interrupted stream is recovered through the
          bootloader's MSC mode (hold KEY1 while resetting)
  ... 1024 KiB
  erase   0x000000..0x15e000    1400 KiB   16.13 s
  program 0x000000..0x15e000    1400 KiB    5.23 s  (267.7 KiB/s)  (read back as written)
  total  21.56 s  (1433600 bytes written and read back)
n32-flash: rebooting into the new rootfs
Read from remote host <板子IP>: Connection reset by peer
    n32-flash/ssh returned 255 (a reset connection is expected)
==> waiting for <板子IP> to come back
    <板子IP> is up; rootfs now runs the pushed image
```

那条 `... 1024 KiB` 是进度（打在 stderr，每 1 MiB 一行），镜像大时才知道它没
卡住。`program` 一行的 KiB/s 已经包含逐块回读的开销。

`Connection reset by peer` 是正常的：设备校验通过后立刻重启，SSH 连接被切断。
脚本会自己等设备回来，最后一行出现才算成功。

### 手动做：管道

不想用脚本时，一条管道就够了（`make ota-*` 内部也是这样调的）：

```sh
cat out/rootfs/rootfs.squashfs | \
    ssh root@<板子IP> '/sbin/n32-flash ota - rootfs --size 1433600'
```

`-` 表示镜像从标准输入来。`--size` 是主机这边知道的镜像长度，用来把"传到一半
断了"变成一条明确的报错，而不是默默写进去一个短镜像；不传也能用，只是没这个
检查。

设备侧自己下载时同样的写法，全程不占内存：

```sh
wget -O - http://<服务器>/rootfs.squashfs | n32-flash ota - rootfs
```

### 手动做：先拷到 /tmp 再刷

不想用管道也行——先把镜像放进 `/tmp`，再让 `n32-flash` 从文件刷：

```sh
scp out/rootfs/rootfs.squashfs root@<板子IP>:/tmp/r.squashfs
ssh root@<板子IP> 'n32-flash ota /tmp/r.squashfs rootfs'
```

对这块板子上的常规镜像（kernel 约 2.2 MiB、rootfs 约 1.4 MiB）这完全没有问题：
设备空闲内存约 16 MiB，镜像只占其中一小块。`n32-flash` 在擦除前会把这一条算给你看：

```text
  note    the image is on a ramfs: 1400 KiB of RAM held now, 15700 KiB available.
          Putting an image there is fine while it stays well under that number.
```

需要留意的是为什么有上面这句话。`/tmp` 是 **ramfs**：每个字节都是真金白银的物理
内存，而且**没有配额**——no-MMU 内核用不了 tmpfs 的大小限制（`CONFIG_SHMEM`
依赖 MMU），`mount -o size=` 会被静默忽略。所以如果镜像大到装不下，结果不是
"写入失败"，而是内核 OOM 杀掉正在写文件的那个进程（实测：往 `/tmp` 灌到
13 MiB 时，`dd` 被 `Out of memory: Killed process ... (dd)` 干掉），而那个半截
文件会一直占着这些内存直到被删掉——`/tmp` 满了之后，板子连正常干活都会受影响。

判断标准就一条：**镜像要明显小于 `/proc/meminfo` 里的 `MemAvailable`**。
只要满足，拷到 `/tmp` 再刷是最省事的做法；镜像大小不好预期、或者设备内存本来就
吃紧时，用上面的管道方式，它跟镜像大小无关。

### 为什么擦除这么慢

上面 1400 KiB 擦了 16 秒，约 88 KiB/s；写入（含逐块回读）有 264 KiB/s。这不是
驱动慢，是 NOR 扇区擦除本身的物理开销：4 KiB 一个扇区约 45 ms，而且 NOR 只能
把 1 写成 0，所以每次更新都必须先擦干净再写。擦除时间只和镜像大小有关，改不了。

擦除是按需做的：只擦镜像实际覆盖到的扇区，所以镜像是 5988 字节的 dtb 时，
只擦 8 KiB，不是整个 64 KiB 分区。

## 更新应用程序（不动 rootfs）

`/system` 是可读写的 JFFS2，装在单独的 `system` 分区上，**升级 rootfs 不会碰它**。
所以日常改程序直接往里放文件：

```sh
scp sdk/example/hello root@<板子IP>:/system/bin/
ssh root@<板子IP> '/system/bin/hello'
```

升级自己的程序/库，这是最常用的路径，**用 `scp` 就够了，不需要 OTA**：`/system`
有 8.9 MiB，装应用绰绰有余，`scp` 一个二进制文件不会碰到任何分区擦写，也就没有
下面那些恢复讲究。只有当你要换的是整个 `rootfs` / `kernel` / `dtb` 时，才需要看
前面的 `n32-flash ota`。

要开机自启就写 `/system/bin/init.sh`（由 `/etc/init.d/boot-services` 每次启动
调用）：

```sh
#!/bin/sh
export PATH=/system/bin:/bin:/sbin
export LD_LIBRARY_PATH=/system/lib:/lib
/system/bin/my-app &
```

注意这台设备**没有 MMU**：后台任务只能是真正的可执行文件（`/path/cmd &`），
不要写 `( ... ) &` 这种 shell 复合后台任务，它永远不会 `exec`，会把启动卡死。
详见 [sdk.md](sdk.md)。

## 更新 kernel / dtb

四个分区在设备树里都是可写的，所以 `dtb` 和 `kernel` 和 rootfs 一样能走网线：

```sh
cd <仓库根>
make ota-kernel H787_BOARD=root@<板子IP>
make ota-dtb    H787_BOARD=root@<板子IP>
```

两个目标都会先跑 `make kernel`，把 `out/kernel/zImage` 和
`out/kernel/n32h787-hmi.dtb` 编出来再推给板子。`n32-flash` 擦之前会校验镜像
头部：`ota kernel` 要求 0x24 处是 zImage 魔数 `0x016f2818`，`ota dtb` 要求开头
是 `0xd00dfeed`。写错分区会被直接拒绝，不会先擦再报错。

也可以手动指定分区，同样走管道：

```sh
cat out/kernel/zImage | \
    ssh root@<板子IP> 'n32-flash ota - kernel --size 2314592'
cat out/kernel/n32h787-hmi.dtb | \
    ssh root@<板子IP> 'n32-flash ota - dtb --size 5988'
```

两者都是**重启后生效**：`n32-flash ota` 写完、读回校验通过就自动重启，所以
新 kernel / dtb 立刻生效（不加 `--no-reboot` 就是默认行为）。

### 写坏了怎么办

dtb 和 kernel 在启动关键路径上，写坏了（或者写到一半掉电）设备就起不来。这时
退回 bootloader：把外部 SPI NOR 整个暴露成 U 盘，主机直接写偏移。

```sh
cd <仓库根>
make flash-image                  # 生成 out/dist/spi-nor-boot.img（make 已做过）
reboot msc                        # 设备侧；或按住 KEY1(PG3) 再复位
# 主机：把设备的 USB 存储 attach 进来后
sudo tools/h787ctl msc-write out/dist/spi-nor-boot.img
```

`spi-nor-boot.img` 只覆盖到 rootfs 结束（`0x710000`），**不动 system 分区**，所以
用户放进 `/system` 的东西会保留。

也可以用 `make flash-kernel`，它把上面两步串起来。

## 安全规则

`n32-flash` 在擦任何东西之前会做这些检查，请不要绕过：

- **内核标记的只读分区拒绝**。当前四个分区都可写，所以这条平时不会触发；如果
  以后哪个分区节点加了 `read-only`，`open(O_RDWR)` 会返回 `EACCES`，`n32-flash`
  会直接说明是该标志导致的。
- **挂载中的文件系统拒绝**。判断方式是遍历 `/proc/mounts`，比较挂载点的
  `st_dev` 和目标分区 `/dev/mtdblockN` 的 `st_rdev`。`/` 和 `/system` 都会被拦。
  就地更新 rootfs 必须显式加 `--force`，这正是 `ota` 子命令在做的事。
- **镜像不能放在要被擦的分区上**。`ota` 会拿镜像所在文件系统的 `st_dev` 和目标
  分区的 `/dev/mtdblockN` 比，是同一个就拒绝——擦除会把源文件毁掉。用 `-` 从
  标准输入喂就没有这个问题。
- **镜像在 RAM 文件系统上时会提示**。镜像位于 `/tmp`（ramfs）等吃物理内存又没有
  配额的地方时，`n32-flash` 会打印镜像占用多少、当前还有多少可用内存，见上面
  "手动做：先拷到 /tmp 再刷"。这不是拒绝，只是让你在擦除前看见这个数。
- **镜像类型校验**，见上。流式写入时这个检查发生在第一个 32 KiB 到达之后、
  第一次擦除之前，所以喂错镜像一样不会先擦坏分区。
- **`--size` 截断检查**（只在 `ota -` 时生效）。流长度和预期不符会报错退出。
  注意它发生在擦除**之后**：管道方式没法在擦之前先验完整份镜像，这是"给文件
  路径时可以顺带校验、走管道时依赖 SSH 自身完整性"这个差别的来源。

写一半掉电的恢复办法：按住 `KEY1`（`PG3`）同时复位，设备进 USB MSC 模式，
从主机重写分区即可。

## 相关命令速查

设备侧 `n32-flash`：

```sh
n32-flash info                                          # 列出分区和可写性
n32-flash write IMAGE PARTITION [--offset N] [--force] [--no-erase]
n32-flash erase PARTITION [--offset N] [--length N] [--force]
n32-flash ota IMAGE [PARTITION] [--no-reboot] [--size N]  # 就地更新 + 重启
n32-flash ota - PARTITION [--no-reboot] [--size N]        # IMAGE 走标准输入
```

主机侧：

```sh
make ota-rootfs                                    # 打包 + 管道进 flash + 校验 + 重启
make ota-kernel                                    # 同上，更新 zImage 分区
make ota-dtb                                       # 同上，更新 dtb 分区
make flash-image                                   # 生成整片 SPI NOR 镜像（make 已做过）
make flash-kernel                                  # 通过 USB MSC 写 dtb+kernel+rootfs
make tools                                         # 重编并把 n32-flash 装进 rootfs
```

## 烧 bootloader（只在换 bootloader 时做）

bootloader 在片内 flash，需要 CMSIS-DAP 探针，用 NSLink 仓库的恢复工具在复位
窗口内抢占内核再编程（普通 DAP `halt` 在 Linux 已经跑起来时可能超时）：

```sh
make flash-bootloader H787_RECOVER_TOOL=/path/to/ns-link/tools/h787_recover_flash.py
```

片内 flash 的编程流程和注意事项见 [tools.md](tools.md) 的烧写一节。
