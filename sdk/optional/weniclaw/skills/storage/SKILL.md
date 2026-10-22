# 存储与文件存放位置

触发：要保存文件、查空间、访问 SD 卡时读本技能。

| 位置 | 介质 | 用途 |
| --- | --- | --- |
| /mnt/sd | SD 卡 vfat | 可读写，插卡自动挂载；照片、录音、大文件放这里 |
| /tmp | RAM (ramfs) | 断电丢失，无容量配额；只放小文件，使用后立即删除 |
| /system | JFFS2 约 9 MB | 可写、重启保留；空间紧张，别堆大文件 |
| / | squashfs | 只读，不能写 |

读完本技能直接执行需要的命令，不要先做额外探测。

- 确认 SD 已挂载：`ls /mnt/sd`；没有挂时 `mount /dev/mmcblk0p1 /mnt/sd`，
  仍失败多半是卡没插好或不是 FAT。
- 查剩余空间：`df -k /mnt/sd /tmp /system`。
- 往板上拷文件用电脑侧 adb push / scp，板上不需要操作。

## 临时文件清理

`/tmp` 是 RAM，不占 SPI flash。系统启动时会新建空的 ramfs，所以重启会清空
全部 `/tmp`；运行期间没有后台清理器，ramfs 也没有容量上限，写满会直接消耗
可用内存。WeniClaw 发送文件后不会自动删除源文件。

- 用户要求保留的照片、录音、下载和附件放 `/mnt/sd`，不要自动删除。
- AI 自己产生、只用于中间处理的文件使用
  `/tmp/weniclaw-<用途>-$$.<后缀>`，例如
  `/tmp/weniclaw-photo-$$.jpg`。
- 中间文件使用或发送完成后，用精确路径删除：`rm -f /tmp/weniclaw-photo-$$.jpg`。
- 不会立刻使用的临时文件要记住完整路径，下一次使用完再删除。
- 禁止执行 `rm -rf /tmp/*` 或按宽泛通配符清理；服务日志和运行状态也在
  `/tmp`。
- 大文件或长录音不要先写 `/tmp`；先用 `cat /proc/meminfo` 查看
  `MemAvailable`，优先写 `/mnt/sd`。
