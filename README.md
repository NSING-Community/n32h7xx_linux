# N32H7xx Linux 实验项目

本项目用于探索和验证 NSING N32H7xx/N32H787 上的 Linux，包含 bootloader、
内核、rootfs 和用户态 SDK。

**这是实验项目**，面向学习、开发与功能验证。功能和接口可能调整，尚未经过完整的
产品级稳定性、可靠性与安全验证；用于实际产品前，需要自行完成相应验证。

## 开始使用

先阅读[环境搭建](doc/setup.md)，再按[快速上手](doc/quickstart.md)完成源码获取、
编译、烧录和开发板登录。具体命令与配置以各专题文档为准。

## 功能清单

| 类别 | 功能 |
| --- | --- |
| 系统 | Linux 运行环境、进程管理、定时与调度、内核配置定制 |
| 启动与更新 | Bootloader、USB 刷机、探针烧录、网络固件更新 |
| 板级外设 | 串口控制台、GPIO、引脚配置、I2C、RTC |
| 网络 | 以太网、IPv4 / IPv6、自动获取地址、网络校时 |
| 远程访问 | SSH、SCP、SFTP、Telnet |
| 存储 | SPI NOR、SD 卡、U 盘、只读根文件系统和可写数据分区 |
| USB | Host / Device 角色、ADB 终端与文件传输、虚拟串口、外设接入 |
| 摄像头 | OV5640 图像采集与 MJPEG 视频采集 |
| 音频 | WM8978 录音与播放 |
| 用户态 | BusyBox 命令、Shell 脚本、文本处理、网络与进程管理工具 |
| 应用开发 | C / C++ 交叉工具链、运行库及应用示例 |

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [环境搭建](doc/setup.md) | 开发环境和工具准备 |
| [快速上手](doc/quickstart.md) | 获取源码、编译、刷机和首次运行 |
| [主机工具与烧写](doc/tools.md) | USB 刷机、探针烧录与串口连接 |
| [网络 OTA](doc/ota-update.md) | 通过网络更新固件 |
| [内核配置](doc/kernel-config.md) | 定制与保存内核配置 |
| [SDK 与应用开发](doc/sdk.md) | 编译、部署和运行应用 |
| [USB](doc/usb.md) | ADB、虚拟串口、存储与角色切换 |
| [摄像头](doc/camera.md) | 图像与视频采集 |
| [音频](doc/audio.md) | 录音、播放与音量设置 |

## 可选组件

可选组件随 SDK 提供，单独安装，不参与默认构建，也不进入固件镜像。

| 组件 | 用途 |
| --- | --- |
| [espeak-ng](sdk/optional/espeak-ng/README.md) | 英文、中文语音合成与播放 |
| [ns-flash](sdk/optional/ns-flash/README.md) | 通过外接 NSLink 探针识别、烧写目标芯片 |

安装、使用和运行条件见各组件说明；开源组件的移植代码及构建说明见
[可选组件源码](sdk/optional-src/README.md)。

## Licensing

根目录 [LICENSE](LICENSE) 为 **BSD-3-Clause**，适用于采用该许可的本项目自有代码。
Linux 内核遵循自身 **GPL-2.0-only** 及逐文件声明；第三方组件保留各自的版权、
许可证和 NOTICE，不受顶层 BSD 许可证覆盖。可选 ns-flash 为公司自有闭源程序。

分发 SDK 或固件时，须按各组件许可证提供声明和必要的完整对应源码，包括实际修改、
配置及构建脚本，并满足适用的库替换、重链接等要求。许可证和源码作为独立发行附件
提供，不增加固件镜像体积。

组件、版本、来源与声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，
发行要求和未确认项见[许可证说明](licenses/README.md)。
