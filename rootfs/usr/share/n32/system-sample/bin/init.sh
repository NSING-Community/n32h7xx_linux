#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# /system/bin/init.sh - user startup hook.
#
# Called once per boot by /etc/init.d/boot-services, after the writable
# /system has been mounted.  This rootfs copy only seeds a brand new /system;
# an init.sh already on the board is never overwritten, so it is yours to edit.
#
# This kernel is no-MMU ARMv7-M running BusyBox hush:
#
#   * Background only real executables, e.g. /system/bin/tool &
#     Never a "( ... ) &" shell compound: it does not exec first, and with no
#     MMU the child keeps running in the caller's address space, corrupting it
#     and wedging the boot.  This is the rule that must not be broken.
#
#   * "x=$(command)" and "a | b" are safe: those exec immediately.
#
#   * Keep it short and non-blocking; boot-services gives it a bounded slice
#     of boot time.
#
#   * Anything that must outlive this script has to be backgrounded.
#
#   * "chmod 0644 /system/bin/init.sh" disables the hook without deleting it.
#
#   * /system holds bin and lib for you, on purpose: configuration is
#     read-only and ships in the rootfs.  Keep scratch files in /tmp (ramfs);
#     anything that must survive a reboot belongs in a subdirectory of your
#     own under /system.  Leave var/ alone -- that is the board's own
#     persistent state (the ssh host key), not a place for user files.
#
# ---------------------------------------------------------------------------
#
# /system/bin/init.sh - 用户开机自启脚本。
#
# 每次启动时由 /etc/init.d/boot-services 调用一次，此时可写的 /system 已挂载。
# 本文件是只读 rootfs 里的模板，仅用于初始化全新的 /system；板子上已有的
# init.sh 不会被固件升级覆盖，随你修改。
#
# 本内核是 no-MMU ARMv7-M，shell 为 BusyBox hush，规则如下：
#
#   * 只能后台化真正的可执行程序，例如 /system/bin/tool &
#     不要写 "( ... ) &" 这种 shell 复合后台任务：它不会先 exec，而 no-MMU
#     下 fork() 不复制地址空间，子 shell 会一直占用并改写父进程的内存，把
#     启动流程拖死甚至写坏。这一条最重要，不能违反。
#
#   * "x=$(command)" 和 "a | b" 是安全的，子进程会立刻 exec。
#
#   * 脚本要短、不要阻塞；boot-services 只给它一段有限的启动时间预算。
#
#   * 需要在本脚本返回后继续运行的服务，必须后台化。
#
#   * chmod 0644 /system/bin/init.sh 可在不删除文件的前提下禁用本钩子。
#
#   * /system 下留给你的是 bin 和 lib，这是刻意的：配置是只读的，随 rootfs
#     发布。临时文件请放 /tmp（ramfs）；需要掉电保留的文件，请自己在
#     /system 下建目录。var/ 目录请勿占用，那是板子自己的持久状态
#     （ssh 主机密钥）。

export PATH=/system/bin:/bin:/sbin
export LD_LIBRARY_PATH=/system/lib:/lib

# Add real services here, for example:
# 在这里添加真正的服务，例如：
#   [ -x /system/bin/app ] && /system/bin/app &
echo "init.sh ran at $(cat /proc/uptime)" >>/tmp/init.log
