<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Linux 适配（fork 修改说明）

本文件是本 fork 相对上游 iPhoneMirror 的**修改声明与状态记录**，用于满足 GNU GPL
v3.0 §5(a) 对修改版的标注要求，并作为适配工作的唯一进度真源。

## 派生信息

| 项目 | 值 |
|---|---|
| 上游项目 | <https://github.com/RayrenSX/iPhoneMirror> |
| 上游许可 | GNU GPL v3.0 only（不变） |
| 派生基点 commit | `3090cafd2edf46834123193473cc3a70561e4aec` |
| 派生基点版本 | 上游 v1.8.1（2026-08-30） |
| 本 fork 修改起始 | 2026-09-04，持续进行 |
| 本 fork 新增代码许可 | GPL-3.0-only（与上游一致） |

上游作者与本 fork 无关联。本 fork 的问题不应提交到上游仓库。

## 适配目标与架构决定

适配范围是把 Windows 专有子系统替换为 Linux 原生方案，**不改变协议层与策略层行为**。
已确定的技术路线：

| 子系统 | 上游（Windows） | 本 fork（Linux） |
|---|---|---|
| 仓库策略 | — | 同树跨平台，Windows 构建保持可编译 |
| GUI | WPF / .NET 10 | Avalonia（X11 后端 + Vulkan 渲染模式），复用既有 Services/ViewModels/本地化 |
| 渲染 | D3D11 + DirectComposition | libplacebo（Vulkan 后端）+ Avalonia `CompositionDrawingSurface` |
| 音频 | WASAPI | PipeWire 原生 |
| 视频解码 | Media Foundation | FFmpeg（VAAPI / 软解） |
| USB 采集 | libusb-win32 内核过滤驱动 + libusb-1.0 | libusb-1.0 + 自有 usbmux 客户端（AF_UNIX） |
| 设备发现 | SetupAPI / cfgmgr32 | libudev / sysfs |
| 驱动安装 | `iPhoneMirror.Driver.exe` | 不需要；改为 udev 规则 + 文档 |
| AirPlay 引擎 | vendored `airplay2dll.dll`（AirPlayServer v1.1.2） | UxPlay `lib/`（GPL-3.0，源码构建） |
| DNS-SD | 自有 `DnsSdShim.cpp` | Avahi 的 `libdns_sd` 兼容层 |
| 虚拟摄像头 | Media Foundation | 延后（v4l2loopback / PipeWire） |

同树跨平台的硬规则：**对上游既有文件只允许做接口抽取，不允许夹带行为变更。**
Linux 后端一律以新增文件实现。

## 状态

图例：`[ ]` 未开始 · `[~]` 进行中 · `[x]` 完成 · `[!]` 受阻

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | fork 声明、许可合规、SPDX 基线 | `[x]` |
| P1 | CMake 跨平台化；可移植模块在 Linux 编过并通过测试 | `[x]` |
| P1-WP1 | 纯机械抽取：`VideoFormats.h`/`VideoFrameCopy.cpp`、`PcmBufferPolicy.{h,cpp}`、`Text/Utf`、`Socket` 跨平台 + AF_UNIX、`QtUsbTransport` 裁剪 | `[x]` |
| P1-S1 | spike：libusb 只读枚举 + 读隐藏配置描述符 | `[x]` |
| P1-S2 | spike：AF_UNIX usbmux `ListDevices` | `[x]` |
| P1-S3 | spike：Avalonia 嵌入原生 surface | `[x]` |
| P1-S4 | spike：解码 → dmabuf → libplacebo 出画并测延迟 | `[x]` |
| P2 | `CaptureSession.cpp` 抽缝共享（方案 X；`wchar_t` 保留，`ApiVersion` 保持 18） | `[x]` |
| P3-WP3 | `LinuxCoreApi.cpp` + `LinuxDeviceManager` + `LinuxEnvironmentProbe` + udev 规则 | `[x]` |
| P3-WP4 | 重枚举恢复策略 + libudev 监视器 + 无头采集工具（USB 半边真机通过） | `[x]` |
| P3 | Linux USB 采集出画（**受阻**：握手在第一次交换后停，两台设备一致） | `[!]` |
| P4 | FFmpeg 解码 / libplacebo 渲染 / PipeWire 音频 | `[~]` |
| P4-WP5A | `LinuxFFmpegVideoDecoder`：libavcodec 解码 + libswscale 转 NV12/P010 | `[x]` |
| P4-WP5B | `LinuxPipeWireAudioRenderer`：PipeWire 播放，队列策略与 WASAPI 共享 | `[x]` |
| P4-WP5C | VAAPI 硬件解码，失败一律退回软解；与软解逐位相同 | `[x]` |
| P4-WP5D1 | `LinuxPlaceboRenderer`：NV12/P010 → libplacebo → RGBA，与 CPU 色彩数学一致 | `[x]` |
| P4-WP5D2 | 导出内存 FD + 双向 Vulkan 信号量（Avalonia 导入端属 WP6） | `[x]` |
| P5 | Avalonia GUI（P5a 最小外壳进 M1，P5b 全量对齐随后） | `[ ]` |
| P6 | UxPlay 引擎无线接收 | `[ ]` |
| P7 | 打包、CI、文档 | `[ ]` |

CI 现状（2026-09-05）：`linux-build.yml`（GCC/Clang 双矩阵）与 `windows-build.yml`
在 fork 上均已通过——`windows-2025-vs2026` 是 GitHub 托管镜像 label（VS 2026 +
CMake 4.4.2），不是自托管机。

WP1 抽取（零行为变更，Windows CI 复验）：`MediaFoundationDecoder` 里的平台中性
面（枚举、`DecodedFrame`、缓冲/letterbox/色彩数学）拆为 `Media/VideoFormats.h`
+ `Media/VideoFrameCopy.cpp`；`materialize_gpu_frame`（D3D11）与
`classify_dxva_mode`（DXVA 常量）留在 Windows 翻译单元。WASAPI 的环形缓冲与
队列阈值策略拆为 `Audio/PcmBufferPolicy.{h,cpp}`（PipeWire 将复用）。新增
`Text/Utf`（Windows 走 WinAPI，Linux 手写 UTF-8↔UTF-32，含单测 `UtfTests`）。
`Transport/Socket` 跨平台化并新增 `connect_unix`（AF_UNIX usbmuxd 用）；
`QtUsbTransport` 的 UsbDk 探测包 `#ifdef`，Linux 走系统 libusb-1.0。

P2 共享 `CaptureSession.cpp`（方案 X，2402 行的握手状态机、看门狗、防护内容
检测、解码器切换与视频队列预算现在两平台共用一份）：

- 媒体后端改走接缝 `Media/IVideoDecoder.h` 与 `Audio/IAudioRenderer.h`，由
  `Media/ActiveVideoDecoder.h` 的工厂按平台构造。Windows 是 Media Foundation
  与 WASAPI；Linux 现在是 `Media/LinuxFFmpegVideoDecoder.cpp` 与
  `Audio/LinuxPipeWireAudioRenderer.cpp`，平台选择留在
  `Media/LinuxMediaBackends.cpp`（WP5 之前它叫 `LinuxMediaStubs.cpp`，装的是会抛
  "not implemented yet" 的桩）。
- libusb0/UsbDk 后端、`LibUsb0RestoreLease`、`restore_libusb0_configuration`
  与 PnP 观察全部包在 `#ifdef _WIN32` 内。`UsbBackend` 枚举保留三个取值，
  后端亲和性记账与诊断因此仍是一份代码，Linux 只会选到 `LibUsb1`。
- usbmux 端点访问抽象成 `for_each_usbmux_endpoint`：Windows 探测 loopback
  27015/37015，Linux 连 `/var/run/usbmuxd`。
- `Logging` 跨平台化：`getrandom`/`getpid`/`getenv`/`localtime_r`，SHA-256 由
  BCrypt 换成 libcrypto（**仅用于日志里匿名化设备序列号**，salt 每进程随机）。
- `apple_usb_serial_equal` 与序列号归一化从 libusb0 后端抽到
  `Transport/AppleUsbSerial.{h,cpp}`。
- Apple USB 设备状态查询在 Linux 由 `Device/LinuxAppleUsbDiscovery.cpp` 回答：
  过滤驱动安全性是**真实结论 Safe**（Linux 路径上根本没有过滤驱动），
  `is_apple_usb_parent_present` 走 libusb 枚举。其余 PnP 接口状态查询没有
  Linux 对应物，只被 Windows 分支调用。

Linux 侧新增系统依赖：`libusb-1.0`、`libcrypto`（`pkg-config` 解析，CI 装
`libusb-1.0-0-dev libssl-dev pkg-config`）。

WP3 Linux C ABI 与环境探测：

- `src/LinuxCoreApi.cpp` 实现 `CoreApi.h` 的全部 60 个导出。**真实实现**：
  `im_initialize`/`im_shutdown`/`im_api_version`/`im_log_message`/
  `im_last_error`/`im_get_environment`/`im_refresh_devices(_ex)`。其余导出返回
  明确失败并在错误串里点名负责的 WP，**任何导出都不会在没做事的情况下返回 Ok**
  ——否则调用方会拿着 Ok 一直等帧。
- `Device/LinuxEnvironmentProbe.{h,cpp}` 回答 Linux 特有的两个问题：usbmuxd
  状态与 Apple USB 设备节点权限。usbmuxd 区分**未安装**、**已安装但未运行**
  （由 udev 在插入设备时拉起，无设备时这是正常状态，不是故障）、**套接字存在
  但拒连**、**已连接**四态。设备节点权限用 libusb 打开尝试判定，
  `LIBUSB_ERROR_ACCESS` 正是需要区分出来的那种失败。
- `Device/LinuxDeviceManager.cpp` 填 `EnvironmentRecord`/`DeviceRecord`：
  `apple_mobile_device_service_*` 映射为 usbmuxd，`capture_usbmux` 恒为 false
  （Linux 只有一个 usbmuxd），`usbdk_backend` 报「已知且不可用」。设备枚举合并
  AF_UNIX usbmux 与 libusb 两个来源——设备可能在 USB 上可见而 usbmuxd 未认领，
  那正是隐藏采集配置留下的状态。
- `tools/linux/70-iphonemirror.rules`：靠 `TAG+="uaccess"` 让 systemd 给活动
  本地会话的用户加 ACL，**不改 OWNER、不动 `bConfigurationValue`、不启停服务**，
  也不需要 `usermod` 或重新登录。规则号在 usbmuxd 的 39- 之后。无头/SSH 会话
  没有活动 seat，`uaccess` 不覆盖，注释里给了改用组的写法。
- 验收工具 `iPhoneMirror.Linux.EnvironmentReport`（只读、不需要设备）：

```sh
LD_LIBRARY_PATH=build/linux/src/Core \
  ./build/linux/src/Core/iPhoneMirror.Linux.EnvironmentReport
```

本机实测（2026-09-05，未接设备）：安装 udev 规则前诊断以「有线采集尚不可用」
结尾，`install` + `udevadm control --reload-rules` 之后该结论消失，
`im_start_capture` 稳定返回 `-7` 并说明由 WP4/WP5 负责。`uaccess` 的 ACL 是否
真的落到 `/dev/bus/usb` 节点上**未验证**，要等 WP4 的真机闸门。

### WP4：重枚举恢复（**已真机验证**）

这是 §4.1 那条最高风险约束的对策。Windows 不需要它：AppleUsbFilter 会保留
0x52 选中的配置。

**这里有一条对交接文档的事实更正。** 交接文档写的是「usbmuxd 的
`39-usbmuxd.rules` 含 `ACTION=="add", ATTR{bConfigurationValue}="0"`，所以 udev 把
活动配置写回 0」。实测**不是这样**：我们丢配置时被写成的值是 **4**（设备的普通
最高配置），而且把 usbmuxd stop 掉之后配置 5 能无限期留住。**真正的竞争者是
usbmuxd 守护进程本身，不是那条 udev 规则。** 所以对策不是「跟 udev 抢」，而是
「在 usbmuxd 还没 claim 之前把 claim 做完」，见下面的 14 ms 时序。

- `Capture/UsbReenumerationPolicy.h`：纯状态机，把重发次数限死在 5 次，并
  **统计配置被覆盖的次数**。`StableSamplesBeforeClaim = 1`——**这里也改过**：原本
  要求连续两次采样都看到 QuickTime 配置才允许 claim，实测那一次等待就足以输掉
  竞争，所以看到一次就必须立刻 claim。单测 `UsbReenumerationPolicyTests` 覆盖了
  分类优先级、覆盖计数、预算耗尽后不谎报成功。
- `Device/LinuxUdevMonitor.{h,cpp}`：libudev netlink 监听。轮询 libusb 只能回答
  「设备在不在」，答不了「它刚刚重新出现」，而后者才是要抓的那个边沿。
  `bConfigurationValue` 从 sysfs 读，**不需要打开设备**，所以在 udev 还没授权
  节点的窗口里也读得到。用 devpath 末段（端口链）作为跨重枚举的稳定身份——地址
  和 product id 都会变。
- `Transport/LinuxUsbConfiguration.{h,cpp}`：Linux 专用的 SET_CONFIGURATION 与
  `ClaimedQuickTimeInterface`。`LIBUSB_ERROR_BUSY` 表示 usbmuxd 还占着接口，
  是预期竞争而非故障。
- `tools/LinuxHeadlessCapture.cpp`（`-DIPHONEMIRROR_BUILD_DANGEROUS_USB_TOOLS=ON`
  才构建）：跑完整链路并把裸流写盘——**不解码、不渲染、不开窗**。这样 USB 半边
  可以独立验证，不必等 FFmpeg 解码器；一个能被任意播放器打开的文件也比预览窗口
  更硬的证据。视频写 Annex-B（含参数集，否则裸 dump 会丢），音频写 RIFF/WAVE。
  每条退出路径都发 HPA0/HPD0 停止控制并请求恢复普通配置。

```sh
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DIPHONEMIRROR_BUILD_DANGEROUS_USB_TOOLS=ON
cmake --build build/linux
./build/linux/src/Core/iPhoneMirror.Linux.HeadlessCapture \
    --serial <udid> --seconds 15 --verbose
```

**已验证项**（详见下面的真机实测各节）：`uaccess` ACL 确实落到 `/dev/bus/usb`
节点；`0x52 wIndex=2` 确实重枚举并追加采集配置；重发 SET_CONFIGURATION + 原子
claim 能拿住配置；usbmuxd 确实会抢设备且抢不到就彻底放弃；握手状态机在
iOS 27 Beta 4 上能推进过 `WaitingForPing`。

**仍未验证**：bulk OUT 写为什么恒超时。

### WP4 真机实测（2026-09-05，iPad Air M3 / iPadOS 27 Beta 4）

设备：`05ac:12ab`，udid `00008122-000161993C98401C`，端口 `3-2`。

**已验证通过**：

| 项 | 结论 |
|---|---|
| `uaccess` ACL | **有效**。`/dev/bus/usb/003/032` 属主是 `usbmux:root`，但 ACL 含 `user:furruka:rw-`，正是 `TAG+="uaccess"` 的产物 |
| usbmuxd 由 udev 拉起 | **成立**。接入后 `usbmuxd.service` 自动从 inactive 变为运行 |
| S1 真机（libusb 枚举） | **通过**，`apple usb devices: 1` |
| S2 真机（AF_UNIX usbmux ListDevices） | **通过**，本项目自带的 `Plist.cpp` 解出 udid |
| 0x52 `wIndex=2` | **有效**。设备重枚举后 `bNumConfigurations` 从 4 变 5，新增配置 5 = `PTP + Apple Mobile Device + Valeria` |
| `expected_quicktime_configuration = highest + 1` | **猜对了**，就是 5 |
| QuickTime 端点 | 描述符与端点都取到：config=5 interface=2 alt=0 in=0x84 out=0x03，512 字节 |

**根因更正：拦路的是 usbmuxd，不是 udev。**

原假设 §4.1 说 udev 会把 `bConfigurationValue` 写回 **0**。规则文本确实含
`ATTR{bConfigurationValue}="0"`，`PRODUCT=5ac/12ab/1503` 也确实匹配
`5ac/12[9a][0-9a-f]/*`。但实测丢配置时的值是 **4**，不是 0：

```
14:11:16.462  cfg=5     ← 我们 SET_CONFIGURATION 成功（日志 applied=true）
14:11:16.488  ← +26 ms，后续 set 全部 LIBUSB_ERROR_BUSY（有人已 claim 接口）
14:11:16.703  cfg=4     ← +240 ms，配置被换回 4
```

决定性实验：`systemctl stop usbmuxd` 后写 `bConfigurationValue=5`，值稳定保持
8 秒以上不变；`systemctl start usbmuxd` 之后立刻又变回 4。**usbmuxd 是那个改
配置的进程。**

而且两者**本不冲突**：配置 5 同时含
interface 1（`0xFF/0xFE` = usbmux）与 interface 2（`0xFF/0x2A` = QuickTime）。
usbmuxd 完全可以在配置 5 里工作，它只是自己选了 4。

由此得到两条硬约束：

1. **usbmuxd 持有接口时无法改配置。** 每次 `libusb_set_configuration` 都是
   `LIBUSB_ERROR_BUSY`。唯一可用窗口是设备重枚举后、usbmuxd claim 之前的
   约 26 ms。
2. **0x52 `wIndex=2` 只在首次生效时触发重枚举。** 配置 5 已存在后再发一次是
   空操作（`acknowledged=yes` 但设备不脱离），所以拿不到新窗口。

**仍未验证**：QuickTime 端点 claim 成功后（停掉 usbmuxd 时做到过），设备
**一个字节都没发**（`bulk reads: with_data=0`）。可能原因未区分：屏幕锁定、
未信任、或 usbmuxd 停止导致 iOS 没有 lockdown 会话因而拒绝启动 Valeria。

#### usbmuxd 与采集配置互斥（双向实测）

- **我们先 claim**：claim 住 config 5 的 interface 2 之后启动 usbmuxd，配置稳定
  保持 5——已 claim 的接口确实让别人的 `set_configuration` 拿到 BUSY。但 usbmuxd
  自己的日志是
  `Could not set configuration 4 for device 3-33: LIBUSB_ERROR_BUSY`，
  然后**直接放弃、不添加设备**（环境报告 `state=1` = UsbPresentNoMux）。
- **usbmuxd 先连上**：它 `Connected to v2.0 device 1` 之后，我们的每次
  `set_configuration` 都是 BUSY。

结论：**usbmuxd 1.1.1 硬要求它自己选的配置（本机是 4），拿不到就整体退出。**
而配置 5 里其实同时有 interface 1（`0xFF/0xFE` mux）与 interface 2
（`0xFF/0x2A` AV），两者本可共存，只是 usbmuxd 不肯用配置 5。

#### 干净序列实测（usbmuxd 被 mask，无竞争）

拔插数据线拿到干净状态（4 个配置）后，`systemctl mask usbmuxd` 让 udev 无法拉起
它，再跑一次完整序列：

```
0x52 acknowledged     : yes
recovery              : ready=yes set_attempts=1 overwrites=0 claim_attempts=1
endpoints             : config=5 interface=2 alt=0 in=0x84 out=0x03
clear_halt            : ok
recover_handshake     : ok
ping write            : failed (LIBUSB_ERROR_TIMEOUT (-7))   ×12
bulk reads            : with_data=0 bytes=0 packets=0
```

配置轨迹（0.12 s 采样）：

```
14:36:38.440  numcfg=4  cfg=（空，即未配置）  ← udev 的 bConfigurationValue="0" 可观测
14:36:40.316  numcfg=5  cfg=5                ← 我们 set 成功，此后 25 s 不变
```

**所以 USB 与配置这一侧全部走通了**：0x52 生效、重枚举、抢窗口（无竞争时一次
成功）、set_configuration 生效并稳定保持、claim 成功、端点正确。**竞争从来不是
真正的阻塞点。**

**真正的阻塞点：iOS 不服务 Valeria 端点。** bulk OUT 写恒为
`LIBUSB_ERROR_TIMEOUT`，bulk IN 在 25 s 内零字节。屏幕已解锁、已信任（用户确认），
usbmuxd 有无都一样。

参考实现的协议文档给了一条关键线索：AV 配置里那 4 个 bulk 端点是
**「2 个用于与设备上的 usbmuxd 通信，另 2 个收发 AV 数据」**。本机上这 4 个端点
分在两个接口（interface 1 = mux，interface 2 = AV）。据此的最强假设是：
**iOS 只在主机同时服务同一配置里的设备端 mux 通道时才启动 Valeria。**
Windows 上这件事由 Apple 的 usbmux 驱动在同一配置内完成；Linux 上 usbmuxd 拒绝
使用配置 5，所以那条通道从来没有建立过——这解释了目前观察到的全部现象。

以上结论均基于 **iPadOS 27 Beta 4**，未在正式版或 iPhone 上复核。

#### iPhone 16 Pro 实测（iOS 27 Beta 4）：设备发了 PING

设备 `05ac:12a8`，udid `00008140-00046DE20111801C`。0x52 生效后新增配置 5
`PTP + Apple Mobile Device + Valeria`，其 interface 2（`0xFF/0x2A`）端点是
**in=0x86 / out=0x05**（iPad 上是 0x84/0x03，所以端点号必须从描述符读，不能写死）。

**关键突破**：在「0x52 → 重枚举 → set config 5 → claim」这一序列刚走完的那次运行里：

```
handshake state       : 1        ← 从 WaitingForPing 前进
bulk reads            : with_data=1 bytes=16 packets=1
```

16 字节正是参考文档描述的 PING 包（magic `676E6970` = "ping"）。**设备主动发了
PING，本项目的 QuickTime 解析器在 Linux 上解得对，协议状态机前进了。**
iPad Air M3 从未发过 PING。

由此确认：**iOS 只在配置切换后的一小段窗口内武装 Valeria 端点。** 配置 5 处于
陈旧的已激活状态时，设备一个字节都不发。

#### 竞争窗口只有 14 ms，set 与 claim 必须原子

带 usbmuxd 运行时的精确时序：

```
14:51:18.391  我们 set_configuration(5)     → applied=true
14:51:18.405  我们 claim_interface(2)       → LIBUSB_ERROR_BUSY
14:51:18.406  usbmuxd: Connected to v2.0 device 1
```

那 14 ms 花在共享的 open 路径上——它枚举每个 Apple 设备并逐个打开读序列号。
于是新增 `Transport/LinuxUsbConfiguration` 的 `ClaimedQuickTimeInterface`：
在**同一个 handle** 上完成 open → set_configuration → 从活动配置描述符定位
`0xFF/0x2A` 接口 → claim，中间不做任何重枚举。它自带 bulk 读写与控制请求，
故意不建立在 `QtUsbConnection` 之上，因为后者的 open 路径正是输掉竞争的原因。

另外 Linux 上曾一度**不发 `clear_halt`**，理由是 `CLEAR_FEATURE(ENDPOINT_HALT)`
会重置主机侧 data toggle、对刚武装的端点可能让 OUT 管道与设备错开。**该判断已被
参考实现推翻并撤回**，见下面的逐项对照。现在两个端点都发，`--no-clear-halt`
保留作对照开关。

#### 与参考实现的逐项对照（`quicktime_video_hack`）

一次离线对照，用来判断我们是不是漏了序列里的某一步。三条结论，其中两条推翻了
我自己先前的判断。

**① `0x52` 的 wLength 假设是错的，作废。** 我先前提出「参考实现传了 data 缓冲区、
我们传 `nullptr, 0`，wLength 不同」。查了源码：参考实现两条路都是
`response := make([]byte, 0)`——**零长度切片，wLength 也是 0，和我们完全一致**。

```go
val, err := device.Control(0x40, 0x52, 0x00, 0x02, response)  // enable
val, err := device.Control(0x40, 0x52, 0x00, 0x00, response)  // disable
```

**② 参考实现的 disable 是个 20 次循环**，每次之后重新选一次 usbmux 配置；enable
只发一次。这和我们实测「disable 立刻 acknowledged，但配置要一分钟量级才消失」
互相印证：对方是拿重试把这段延迟磨掉的。

**③ 参考实现对两个端点都发 `CLEAR_FEATURE(ENDPOINT_HALT)`**，而且这是它开始
streaming 之前**唯一**的控制传输：

```go
usbDevice.Control(0x02, 0x01, 0, uint16(inboundBulkEndpointAddress), make([]byte, 0))
// 出向端点同样一次
```

`0x02` = host→device / standard / **endpoint recipient**，正是 `libusb_clear_halt`
发的那条请求。

**这条推翻了我先前「Linux 上不发 clear_halt」的改动。** 那个改动基于一条
HYPOTHESIS（重置 data toggle 会让刚武装的 OUT 管道和设备错开），而一个跑得通的
实现做的恰好相反，且把它放在 streaming 前的必经路径上。已恢复，默认发，保留
`--no-clear-halt` 供真机 A/B。**该 A/B 已经做了，结果是决定性的**：不发 clear_halt
一个字节都收不到，发了 PING 就来——见下面那一节。

**已验证：`CLEAR_FEATURE(ENDPOINT_HALT)` 是必需步骤。** 先前对 OUT 超时提的
「data toggle 不同步」假设方向对了一半：clear_halt 确实是缺失的那一步，但它解决的是
**IN 完全不出数据**，而第一次写之后的 OUT 超时是另一个问题，见下。

对照里确认一致、不用改的部分：按 subclass 找接口而不是按 index；alt setting 在
claim 时就选 0，没有单独的 SET_INTERFACE；主机被动等 ping，不先发；读帧是 4 字节
小端长度前缀且**长度含自身**（所以 payload 是 `length - 4`）。

#### 仍未解决

**第一次交换之后握手就停**，两台设备表现一致：收到设备的 16 字节 PING、第一条出站
消息写成功之后，IN 再无数据、后续写全部 `LIBUSB_ERROR_TIMEOUT`。首选假设是我们回给
PING 的那条消息不被接受、设备据此拆掉了 Valeria 会话（因为两个方向是同时停的）。
**不需要特定设备，iPad 就能继续验。**

#### 决定性实验：不是 iPad 不兼容，是我们漏了 `clear_halt`

**先前这一节写的「iPad Air M3 不武装 Valeria、是 iPadOS 与 iOS 的行为差异」是错的，
在此撤回。** 那个结论建立在一个没控住的变量上：iPhone 那轮**发了** `clear_halt`，
iPad 那轮**没发**（当时刚按一条错误假设把它去掉）。设备不是唯一的差异项。

把 `clear_halt` 恢复后，**同一台 iPad、同一份代码、usbmuxd 同样 mask**，重跑：

```
clear_halt            : both endpoints
handshake state       : 1                              ← 越过 WaitingForPing(0)
handshake kick        : ok
bulk reads            : with_data=1 bytes=16 packets=1 ← 16 字节 PING 到了
outbound writes       : ok=1 failed=0                  ← 一次写成功
handshake final state : 4 (Stopping)
```

对照上一轮（唯一差异是不发 clear_halt）：`bulk reads: with_data=0 bytes=0
packets=0`、`handshake final state: 4` 但从未离开过状态 0。

**结论：`CLEAR_FEATURE(ENDPOINT_HALT)` 是必需步骤，不是可选的恢复手段。** 不发就
一个字节都收不到。参考实现把它放在 streaming 前唯一的控制传输位置上是对的，我先前
「重置 data toggle 会让刚武装的 OUT 管道错开」那条假设是错的。**iPad Air M3 与
iPhone 16 Pro 行为一致，两台都能进握手。**

#### 第二个错误：工具违反了上游对「恢复」的守卫条件

上一节说「下一步核对回给 PING 的消息」，核对之前先发现了更基础的问题。上游
`CaptureSession.cpp` 发那条恢复序列时有三重守卫：

```cpp
if (!ping_recovery_attempted &&
    protocol.state() == quicktime::SessionState::WaitingForPing &&   // 只在设备还没说话时
    now >= ping_recovery_deadline) {
    ping_recovery_attempted = true;                                  // 一次性
    if (newly_activated_libusb0)                                     // 只有 libusb0 路径才发控制 kick
        usb->recover_handshake();
    usb->write(quicktime::make_ping(), 1000);
}
```

无头工具三条全违反了：`0x40/0x40` 控制 kick 无条件发（而我们走的是 libusb1，上游在
这条路径上**根本不发**）、每 2 秒重发一次未请求的 PING、且不看状态。所以在那次成功
的运行里，握手已经进到 `WaitingForAudioClock` 之后，第 3 秒我们往一个活着的会话里
插了一条厂商控制请求，然后开始刷 PING——**「第一次写成功、之后全部超时、设备也不再
说话」的直接嫌疑就是这个**，而不是协议字节错。

已按上游的三重守卫改回，并把控制 kick 变成 `--control-kick` 显式开关。

#### 设备可以卡进「采集配置摘不掉」的状态，只有重启能救

- `0x52 wIndex=0` 在 180 秒里**发了 36 次、36 次全部 acknowledged**，`bNumConfigurations`
  始终是 5。
- 之后让 usbmuxd 正常运行（它把活动配置选到 4）再等 2 分钟，仍然是 5。所以「需要有人
  SET_CONFIGURATION 回普通配置才会掉」这条假设**也不成立**。
- 在这个状态下 `--no-cycle`（直接 claim 已存在的配置）也不行：5 次 set + 5 次 claim
  全失败，`active_config` 一直停在 4，`overwrites=0`。

即设备既不肯摘掉采集配置，也不肯把它设为活动配置。已知拔插不能复位、disable 被
acknowledged 但无效，**重启设备是唯一出路**（重启后实测回到 `numcfg=4 active=4`）。

顺带一条工具改进（来自参考实现，它的 disable 是 20 次循环）：reset 等待期内现在每
5 秒重发一次 disable 并报告发了几次、几次被 ack，而不是只发一次。上面那个「36 次」
就是这条改进测出来的。

#### 重启后复验：守卫修复是对的，但不是原因

重启后带守卫修复重跑：

```
recovery              : ready=yes set_attempts=2 overwrites=0 claim_attempts=2
clear_halt            : both endpoints
handshake state       : 1
ping attempts         : 0          ← 不再发 kick、不再刷未请求的 PING
outbound writes       : ok=1 failed=0
bulk reads            : with_data=1 bytes=16 packets=1
```

`ping attempts : 0` 说明守卫生效了：状态一离开 `WaitingForPing`，恢复序列就不再触发。
也没有任何写超时——因为我们不再乱写。

**但握手仍然停在 `WaitingForAudioClock`。** 设备发一个 PING、接受我们的回复、然后
40 秒内不再说话，`SYNC CWPA` 始终不来。所以**「控制 kick 破坏了会话」这条假设也被
推翻了**——它确实是个该修的错误，但不是当前阻塞的原因。

#### 已核对：包 magic 的字节序是对的，不要去「修」它

`fourcc('p','i','n','g')` 按 Apple 的显示顺序等于 `0x70696E67`，`append_u32_le` 再把它
按小端写出去，所以线上字节是 `67 6E 69 70`——**看起来像 "gnip"**。头文件里那段注释
说的就是这件事。

判据是解析器：`read_u32_le` + 同一个 `fourcc` 常量，而它**确实认出了设备发来的 PING**
（状态从 0 进到 1 就是证据）。收发两侧用同一约定并且能对上设备，所以约定正确。把它
「改成 ASCII 正序」会破坏一个已经在 Windows 上工作的协议。

#### 记下一处与参考实现的差异（要等 CWPA 之后才用得上）

参考实现在收到 `SYNC CWPA` 之后**连发两次 `ASYN HPD1`**（`WriteDataToUsb(deviceInfo)`
调了两次，日志也打两遍），我们只发一次。目前还走不到那一步，先记着。

#### 下一个待测变量：屏幕是否解锁 —— 已排除

星翼确认设备**全程没有锁过屏**。所以解锁不是变量，这条排除掉。

#### 已确认：iOS 每个开机周期只服务一次 Valeria 会话

重启后连跑两轮，**两轮的 claim 都成功**，所以这不是竞争或时序问题：

```
===== run 1: 重启后的第一次会话 =====
recovery              : ready=yes set_attempts=2 claim_attempts=2
clear_halt            : both endpoints
inbound               : 16 bytes
  10 00 00 00 67 6e 69 70 00 00 00 00 01 00 00 00     ← 设备的 PING
outbound              : 16 bytes
  10 00 00 00 67 6e 69 70 00 00 00 00 01 00 00 00     ← 我们的回复
handshake state       : 1
bulk reads            : with_data=1 bytes=16 packets=1

===== 主机端口复位 =====  numcfg=5 active=1

===== run 2: 同一开机周期内的第二次会话 =====
recovery              : ready=yes set_attempts=1 claim_attempts=1
clear_halt            : both endpoints
bulk reads            : with_data=0 bytes=0 packets=0     ← 零字节
```

**结论：第一次会话能收到 PING，同一开机周期内的第二次就沉默。** 这解释了今天所有的
「结果不可重复」，也解释了采集配置为什么会变成摘不掉——iOS 那边留着一个半开会话。
推论：**我们的 teardown 没有落地。**

#### 副产品一：字节序约定在线上得到证实

设备发来的 PING 是 `10 00 00 00 67 6e 69 70 ...`——magic 字节确实是 `67 6e 69 70`
（"gnip"），和 `fourcc` 显示顺序 + `append_u32_le` 产生的完全一致。我们的回复与设备的
PING **逐字节相同**。这条从「按代码推理」升级成了「线上实测」，不要再动它。

#### 副产品二：Valeria 接口的端点布局已封闭，不存在选错的可能

配置 5（`iConfiguration 10 = "PTP + Apple Mobile Device + Valeria"`）的完整布局：

| 接口 | 类/子类/协议 | alt | 端点 |
|---|---|---|---|
| 0 | 06/01/01 Imaging PTP | 0 | 0x01 OUT, 0x81 IN, 0x82 INT |
| 1 | FF/FE/02 usbmux | 0 | 0x02 OUT, 0x83 IN |
| **2** | **FF/2A/FF Valeria** | **只有 0** | **0x84 IN, 0x03 OUT，各 512 B** |
| 3 | 02/0D Communications | 0 | 无 |
| 4 | 0A/00/01 CDC Data | 0 / 1 | alt 0 无；alt 1 有 0x85 IN 等 |

Valeria 接口**只有一个 alt setting、只有两个 bulk 端点**，所以
`interface=2 alt=0 in=0x84 out=0x03` 是唯一可能的选择。「参考文档说有 4 个 bulk 端点、
我们可能挑错了那一对」这条假设**彻底排除**。

#### 根因找到了：usbmuxd 1.1.1 把配置号钉死在 4

**这一条把前面所有关于 lockdown 协议栈的推测都作废了，而且修法便宜得多。**

先看 Windows 那边到底是什么安排。`src/App/Services/IPhoneFilterDriverService.cs` 检查的
是这三件事：

```csharp
service == "usbccgp"                       // 微软复合设备父驱动，原样保留
UpperFilters 含 "libusb0"                  // 我们的访问只是一个上层过滤器
LowerFilters 含 "AppleLowerFilter" / "AppleKmdfFilter"   // 苹果的过滤器原样保留
```

即 **Windows 上苹果的驱动栈完整保留，libusb0 只是加了一层上层过滤器来获得额外访问**。
AMDS 的 lockdown 会话一直活着，我们同时也能说话。这也解释了星翼的观察——UsbDk 那类后端
在 Windows 上和苹果驱动不兼容，因为它是**替换**驱动而不是加过滤器。

Linux 上对应的安排本来天然可行：**usbmuxd 拿 interface 1（`FF/FE`），我们拿
interface 2（`FF/2A`），同一个配置 5 的两个不同接口**——内核允许。

那为什么不行？看 usbmuxd **1.1.1** 的 `src/usb.c`：

```c
int desired_config = devdesc.bNumConfigurations;
if (desired_config > 4) {
    desired_config = 4;          // ← 就是这一行
}
```

采集配置存在时 `bNumConfigurations = 5`，本来 `desired_config` 就该是 5，**这个 clamp
硬把它压回 4**。于是 usbmuxd 永远去抢配置 4、抢不到就放弃设备，lockdown 会话从来不存在。

而 usbmuxd **master** 早就改了。它有个 `guess_mode()`，专门认 Valeria：

```c
if(intf->bInterfaceClass == INTERFACE_CLASS &&
   intf->bInterfaceSubClass == 42 &&        // 0x2A
   intf->bInterfaceProtocol == 255) {
    has_valeria = 1;
}
...
usbmuxd_log(LL_NOTICE, "Found Valeria and Apple USB Multiplexor in device %i-%i configuration 5", ...);
```

并且 `set_valid_configuration()` 是**从高到低**遍历配置的
（`for(j = bNumConfigurations ; j > 0 ; j--)`），所以采集配置在时它会选**配置 5**，
只 claim 自己的 interface 1，把 interface 2 留给我们。

**这正是 Windows 那套安排的 Linux 等价物。**

#### 结论与代价

- **不需要写 lockdown 协议栈。** 那条路（USBMUX 版本握手 → 62078 → pair record TLS）
  的前提是「Linux 上没法共存」，而共存不成立的唯一原因是 usbmuxd 1.1.1 的那行 clamp。
- 修法是**版本要求**，不是打补丁：upstream 至今没有 1.1.1 之后的 tag，所以只能用 git
  master。Arch 的 AUR 有 `usbmuxd-git`（`1:1.1.1.r47.g049877e`），已核实提交
  `049877e`（2023-04-21）确实含 `guess_mode` / `has_valeria`。
- **代价要讲清楚**：替换系统级 usbmuxd 会影响这台机器上**所有** iOS 设备访问
  （libimobiledevice、GNOME 的照片导入、Steam 之类都走它）。它是可回滚的
  （`pacman -S extra/usbmuxd` 装回），但属于系统组件变更，需要星翼同意。
- 对发行的影响：Linux 版需要在文档里写明「需要支持 Valeria 的 usbmuxd（git master
  之后）」。这比要求用户装打过补丁的私有版本干净得多。

#### 两个命题，只有一个被证明了

必须分清，因为我上一轮把它们混着说了：

1. **已证明（代码级）**：usbmuxd 1.1.1 把 `desired_config` clamp 到 4，所以它**不可能**
   在配置 5 上共存。
2. **未证明**：iOS 需要一个活着的 lockdown/mux 会话才肯发 `SYNC CWPA`。

如果第 2 条是假的，那换新 usbmuxd 对我们这个卡死**毫无帮助**，真正的原因仍然未知。
所以实验的价值是双向的：中了就修好了，没中就便宜地砍掉一整个分支。

#### 实验环境已就绪，且没有改动系统

usbmuxd git master 已在 `/tmp/ipm_muxd` 本地构建完成（`1.1.1-git-3ded00c`，
`--prefix=/tmp/ipm_muxd/prefix`），**没有装进系统、没有动 pacman**。这比装 AUR 包更轻：
测试时把系统 usbmuxd mask 掉、前台跑我们这份，测完 unmask，除此之外无残留。

#### 打包方式：几个方案与代价（待星翼拍板）

星翼提出「客户端自带一个 usbmuxd 也不是不行」。前提约束是：**usbmuxd 设计上是单例**
——它独占 `/var/run/usbmuxd`、由 udev 激活、还专门有 `--exit`/`-X` 用来让已在运行的实例
退出。两个实例会抢同一批设备。

**方案 A：要求系统上的 usbmuxd 支持 Valeria。**
优点：不打包、单一守护进程、符合 Linux 打包惯例、其它 iOS 工具照常工作。
缺点：**upstream 至今没有带这段代码的 release**（1.1.1 是 2020 年的最新 tag），
Debian/Ubuntu/Fedora 稳定版用户无法满足，实际上等于长期挡住大部分发行版。

**方案 B：自带 usbmuxd，采集期间跑自己那份。**
做法就是现在测试脚本干的事：停掉并 mask 系统守护进程 → 跑我们的 → 退出时恢复。
优点：不管发行版带的是哪个版本都能用、自洽。
缺点：**我们运行期间这台机器上其它 iOS 工具全部失效**（照片导入、libimobiledevice……）；
需要 root 去停系统服务；要防 udev 把系统那份重新拉起；还要自己构建维护一份 GPL 守护进程。

**方案 D：不用 usbmuxd，自己讲 mux/lockdown。** 只有在第 2 条成立、且 A 和 B 都不可接受时
才值得——成本是那套 USBMUX + 62078 + pair record TLS 协议栈。

**倾向 A，B 作为兜底**，理由不是打包洁癖，而是这个项目自己的既定立场。
`IPhoneFilterDriverService.cs` 上有一句注释写得很清楚：

> The WPF process intentionally never installs or mutates drivers.

Windows 侧的做法是**检查驱动状态并告知用户**，而不是偷偷替换。Linux 侧对等的做法就是
**检测 usbmuxd 能力并在环境报告里说明**，而不是默默替换用户的守护进程。B 作为可选模式
提供给满足不了 A 的发行版是合理的，但不该是默认。

**这个决定可以等实验之后再做**——如果第 2 条是假的，整个讨论就不成立。

#### 待验证

装上 `usbmuxd-git` 之后重启设备再跑一轮。判据是 usbmuxd 日志里出现
`Found Valeria and Apple USB Multiplexor in device ... configuration 5`，
并且我们的握手能从 `WaitingForAudioClock` 走到 `Negotiating`。

#### 剩下的问题，以及它为什么难迭代

即使是开机后的第一次会话也停在 `WaitingForAudioClock`：设备发 PING、我们回一个字节
相同的 PING、写成功（URB 完成即设备已 ACK）、然后 `SYNC CWPA` 不来。

接线层面已经全部排除：接口对、端点对、claim 成功、`clear_halt` 成功、回复字节正确。
所以问题在会话语义上。

#### 已测：usbmuxd 开着也没用（但这一轮没能证伪 lockdown 假设）

一轮**不 mask usbmuxd** 的运行（重启后的第一次会话，代码是修好守卫、发 clear_halt 的
版本）：结果与 mask 时**完全一样**——PING 进、字节相同的回复出、然后沉默。

但它**没有真正检验那条假设**，因为 usbmuxd 从未在配置 5 上建立会话。它的日志：

```
17:35:30 Removed device 1 on location 0x30037     ← 我们重枚举时它的会话死了
17:35:30 usbmuxd shutting down                    ← -z/socket activation 导致重启
17:35:30 usbmuxd v1.1.1 starting up
17:35:30 Could not set configuration 4 for device 3-56: LIBUSB_ERROR_BUSY
17:35:30 Initialization complete                  ← 放弃了这个设备
```

所以这一轮的环境实际上等于「没有 mux 会话」。**要真正检验，必须让配置 5 的
interface 1 上有一个活着的 mux 会话**，而 usbmuxd 1.1.1 不会这么做——`usbmuxd --help`
里没有任何「不要改配置」的开关，只有 `-p/--no-preflight`（关 lockdownd preflight），
不是我们要的。

#### 估算更正：自己讲 mux 不是「一百行探针」

先前把「claim interface 1、发一条 usbmux `ListDevices` 看 iOS 反应」估成约一百行，
**这个估算是错的**。`ListDevices` 是 usbmuxd **守护进程**的 API，不是 USB 线上的东西。
线上要做的是：USBMUX 自己的版本握手 → 类 TCP 通道连到 lockdown 的 62078 端口 → 用
`/var/lib/lockdown` 里的 pair record 做 TLS。这是一整套协议栈，不是探针。

#### Windows 基线已实测：同一台设备、同一个系统版本，Windows 能出画

星翼在 Windows 机器上用上游 release 插同一台 iPad Air M3（iPadOS 27 Beta 4）实测
**可以镜像并控制**，但**并不稳定**：有时只出一帧就卡死掉线，有时能一直出，
「再怎么说都能出一帧」。他同时观察到数据线接触不太稳、容易掉。

**所以 Windows 不是一条干净的基线，这一点必须记清楚。** 但它仍然排除了协议本身，
而且两边的失败点是**不同的**：

| | 能走到哪 | 稳定性 |
|---|---|---|
| Windows | **每次都至少出一帧**——过了 CWPA、HPD1、格式协商，拿到真实视频样本 | 时好时坏 |
| Linux | **一次都没过 ping 交换**，`SYNC CWPA` 从未到达 | 三次真实首会话全部同一处停 |

一根能让 Windows **每次**都走完协商的线，不太可能刚好让 Linux **每次**都死在最早的
那一步。所以数据线是一个**已确认的干扰项**，但它解释不了这个定性差异。

**这条排除了「iPadOS 27 改了 Valeria 握手」**，也就是说问题不在协议本身，更不在
`CaptureSession.cpp` / `QuickTimeSession.cpp`——那两个文件两平台**共用同一份**，
Windows 上它们工作正常。差异只可能在 Linux 侧的传输环境里。

于是范围收窄成两条：

1. **总线上还有谁。** Windows 走的是 AppleUsbFilter + libusb0 过滤驱动路径，AMDS 的
   lockdown 会话与我们的采集**同时存在**——过滤驱动在底层，两边都能说话。Linux 上
   `libusb_claim_interface` 是排他的，usbmuxd 被彻底挤出去（它甚至直接放弃设备）。
   这正是那条 lockdown 假设，现在它是首选。
2. **后端差异本身。** 上游 Windows 默认走 libusb0 过滤后端；代码注释也写了
   「libusb1/UsbDk 需要显式 clear halt，libusb0 过滤后端历史上不需要」。

**下一步按成本排序，先做便宜的：**

1. **换一根数据线。** 零成本。既然线已经被确认不稳，那它污染的是两个平台的全部数据。
   如果换线后 Linux 能收到 CWPA，上面所有推理都要重新过一遍——这件事宁可现在发现，
   也不要在写完一套 lockdown 栈之后才发现。
2. **Windows 上抓一份 USB trace。** 它会直接给出 ping 交换之后主机发了什么、
   interface 1 上有没有 lockdown 流量。线不稳反而让它更有价值：trace 里能区分
   「USB 链路层错误/stall」和「协议流程干净但缺了一步」，这两者的修法完全不同。

#### 迭代成本

**每做一次实验要重启一次设备**，这是当前真正的成本，也是为什么 teardown 必须修：只要
会话能被干净关掉，每个开机周期就不再只有一次机会。


同一条命令、同样干净的起点（`count=4`）、usbmuxd 同样 mask、`clear_halt` 同样两个端点
都发、claim 同样成功——**一次收到 PING，下一次零字节**。

| 运行 | 前置状态 | 方式 | set/claim 次数 | 结果 |
|---|---|---|---|---|
| A | 刚重启设备 | cycle | 2 / 2 | **PING 到了**，状态进到 1 |
| B | 重插数据线后 | cycle | 1 / 1 | 零字节 |
| C | 主机侧端口复位后 | `--no-cycle` | 1 / 1 | 零字节 |

**唯一收到 PING 的一轮，是设备刚重启之后的第一次尝试。** 两次零字节都发生在同一个
开机周期内、且此前当天已经开过 Valeria 会话。这条观测催生了上面那个已确认的结论。

#### 一个有用的副产品：主机侧端口复位让 claim 变可靠

`echo 0 > /sys/bus/usb/devices/<port>/authorized` 再 `echo 1` 之后，采集配置虽然**仍然
在**（`numcfg=5`），但活动配置回到 1，而且此时 `--no-cycle` **一次就 claim 成功**
（`set_attempts=1 claim_attempts=1 configuration_was_set=yes`），完全不需要 0x52 循环。

所以：**那套 180 秒的 disable/reset 等待对「拿到接口」不是必需的。** 这条对迭代速度
很有价值——原来每次实验要付 180 秒，现在不用。

同时它也再次确认了复位的边界：deauthorize/reauthorize 这种主机侧端口复位**也**清不掉
采集配置。清不掉它的手段现在有四种被证否：拔插、主机端口复位、36 次被 ack 的
`0x52 wIndex=0`、让 usbmuxd 去选普通配置。**只有重启设备有效。**

#### 关于「怎么复位」的事实更正

早先记的「拔插数据线能让设备回到基础配置集」是**错的**。实测两台设备都是：
**采集配置跨拔插存活**。唯一能让它消失的是 `0x52 wIndex=0`，而且该请求立刻回
acknowledged，配置却要**一分钟量级**之后才真正消失。所以：

- 拔插不是复位手段。
- 每轮运行前的复位必须是「发 disable → 轮询等配置数掉回去」，工具里的等待上限
  因此设为 180 秒。

这条纠正很重要：它解释了之前多次「0x52 acknowledged 但没有重枚举」——那些运行
里采集配置本就还在，wIndex=2 是空操作，于是从来没有窗口。

P1 的构建结论：`src/Core` 的 5 个可移植翻译单元（Protocol / Media / CoreMedia / H264）
在 GCC 16.2 与 Clang 22.1 下都能构建出 `libiPhoneMirror.Core.so`，`ctest` 3/3 通过
（`OutputModeStateTests`、`UsbConfigurationRestorePolicyTests`、
`DnsSdRegistrationPolicyTests`）。Windows 侧目标全部按平台裁剪，未做行为改动。

### WP5-A：libavcodec 视频解码器（已通过，无需设备）

`src/Core/src/Media/LinuxFFmpegVideoDecoder.{h,cpp}` 实现 `IVideoDecoder`，替换掉
原 `LinuxMediaStubs.cpp` 里那个会抛 "not implemented yet" 的桩。那个文件已改名
`LinuxMediaBackends.cpp`，现在只做平台选择；`materialize_gpu_frame` 单独拆到
`LinuxSharedGpuFrame.cpp`，因为凡链接 `VideoFrameCopy.cpp` 的目标都需要它，而只有
库需要那两个工厂——合在一起会把 PipeWire 拖进从不播音的工具里。

三个不是库默认值的决定：

- **切片线程，不用帧线程。** 帧线程用「延后几帧输出」换吞吐，而镜像的全部意义就是
  桌上的屏幕和显示器上的窗口一起动。同理设了 `AV_CODEC_FLAG_LOW_DELAY`。
- **Annex-B extradata。** 格式描述里的参数集本来就是一条条裸 NAL，libavcodec 的
  H.264/HEVC 解码器接受起始码分隔的参数集流，所以不再去拼一个 avcC/hvcC——那只是
  多一份可以写错的序列化。
- **色彩以 QuickTime 格式描述为准。** libavcodec 解析出的 VUI 只填格式描述留空的
  项，剩下的空缺套用与 Windows 解码器**逐条相同**的兜底（primaries→bt709、
  transfer→bt709、matrix→`height >= 720 ? bt709 : bt601`、range→limited）。这样一帧
  到渲染器时两个平台带的色彩元数据是同一套。

输出格式按 `bit_depth_luma > 8 || bit_depth_chroma > 8` 选 P010 否则 NV12，和
Windows 侧 `prefer_p010` 同一条判据。奇数宽高向下取偶（4:2:0 的色度寻址不到半个
像素）；目前测到的 Apple 采集几何全是偶数，所以这只是防畸形流。

#### 验收：与 ffmpeg 自己的输出逐字节相同

`src/Core/tools/LinuxDecodeProbe.cpp`（Linux 上总是构建，不需要设备、不改任何状态）
读一个 MP4，把每个 `AVPacket` 直接喂给 `decode()`——**容器里的包本来就是长度前缀的
AVCC 样本，和连上的 iPhone 发的是同一种形状**，所以这条路径就是采集路径。参数集从
`avcC` 里取，对应格式描述里的参数集。libavformat 只链进这个工具，不进 Core：采集
路径的样本来自 USB，库本身没有理由认识容器。

8 bit / NV12：

```
input                 : probe.mp4 1170x2532 depth=8 nalu_length_size=4 sps=1 pps=1
decoder               : h264 (libavcodec slice-threaded software) output=nv12
samples               : 120        frames : 120
first frame           : 1170x2532 stride=1170 bytes=4443660 format=nv12
colour                : primaries=bt709 transfer=bt709 matrix=bt709 range=limited hdr=no
```

10 bit / P010：

```
input                 : p010.mp4 640x360 depth=10 nalu_length_size=4 sps=1 pps=1
decoder               : h264 (libavcodec slice-threaded software) output=p010
samples               : 30         frames : 30
first frame           : 640x360 stride=1280 bytes=691200 format=p010
colour                : primaries=bt709 transfer=bt709 matrix=bt601 range=limited hdr=no
```

`bytes` 两边都对得上（`1170*2532*3/2` 与 `640*360*3`），`matrix=bt601` 正是
`height < 720` 那条兜底在起作用。

最硬的一条证据是**逐字节比对**：

```sh
ffmpeg -i probe.mp4 -pix_fmt nv12   -f rawvideo reference.nv12
ffmpeg -i p010.mp4  -pix_fmt p010le -f rawvideo ref.p010
cmp out.nv12 reference.nv12   # 相同（533,239,200 字节 / 120 帧）
cmp out.p010 ref.p010         # 相同（20,736,000 字节 / 30 帧）
```

两条都**完全相同**。这同时证明了解码、libswscale 的平面重排、以及紧凑打包的
stride 计算都没有偏差——不是「看起来对」，是位级相同。

### WP5-C：VAAPI 硬件解码（已通过，无需设备）

同一个 `LinuxFFmpegVideoDecoder`，`DecoderPreference` 不是 `SoftwareCompatible` 时尝试
VAAPI，否则走软解。

**每一条失败路径都退回软解而不是抛异常**：Linux 上没有 render node、驱动不实现这个
codec 都是正常状态，不是拒绝解码的理由。`decoder_acceleration()` 和
`selected_decoder_is_hardware()` 报的是**实际拿到的**东西——`get_format` 回调如果发现
libavcodec 没给出 `AV_PIX_FMT_VAAPI`，就把状态改回 `Software`，不会谎报硬件。

能力检测问 codec 自己（`avcodec_get_hw_config` + `AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX`）
而不是猜：构建里可能根本没编 hwaccel，即使平台有 render node。

VAAPI 帧在显存里，所以 `av_hwframe_transfer_data` 先取下来再做打包转换，并且补一次
`av_frame_copy_props`——传输只搬像素不搬元数据，而色彩合并要读那些字段。

#### 验收：与软解**逐位相同**

```
=== software ===
decoder : h264 (libavcodec software, slice-threaded) output=nv12 hardware=no
frames  : 120        first frame : 1170x2532 stride=1170 bytes=4443660

=== hardware (vaapi) ===
decoder : h264 (libavcodec vaapi) output=nv12 hardware=yes
frames  : 120        first frame : 1170x2532 stride=1170 bytes=4443660
```

```sh
cmp sw.nv12 hw.nv12   # 完全相同（533,239,200 字节 / 120 帧）
```

H.264 解码在规范上是逐位确定的，所以「相同」是**应该**出现的结果；它同时验证了
hwframe 下载路径、元数据补齐和打包转换都没有引入偏差。这也意味着 WP5-A 那次与
`ffmpeg -pix_fmt nv12` 的逐字节比对结论对硬解路径同样成立。

#### 尚未做的部分
- HEVC 走的是同一套代码，但验收素材是 High 10 的 H.264：探针只解析 `avcC`，MP4 里
  的 HEVC 参数集在 `hvcC` 里，要验 HEVC 得先给探针加 `hvcC` 解析。

### WP5-B：PipeWire 音频渲染器（已通过，无需设备）

`src/Core/src/Audio/LinuxPipeWireAudioRenderer.{h,cpp}` 实现 `IAudioRenderer`。

**队列决策不重写**：容量、startup / high-water 阈值、丢弃计划全部来自 WP1 抽出的
`Audio/PcmBufferPolicy.h`——那次抽取的目的就是让两个平台对「迟到的包」「超大的包」
做同样的判断。本文件里属于本地的只有环形缓冲的 memcpy 机械动作和 PipeWire 管线。

增益也刻意对齐：同样的 1/10000 单位、同样在一个输出缓冲内做线性斜坡，所以调音量和
静音切换在两个平台听起来一样，不会咔哒。**没有**走会话管理器的音量控制——那会让
播放行为取决于装的是哪个管理器。

格式只offer一种：`checked_wasapi_buffer_layout` 已经把可接受集合限定成交错的有符号
PCM16，对应 `SPA_AUDIO_FORMAT_S16_LE`，没有第二种可谈。

#### 验收：`src/Core/tools/LinuxAudioProbe.cpp`

按设备发包的粒度（512 帧）实时推 440 Hz 正弦，读回计数器。判据是
`rendered_frames` 在涨而 underrun 为 0——只有 PipeWire 图真的在从这个渲染器的环里
取数据才可能这样，所以一次跑过就同时覆盖了连接、格式协商和 process 回调。

```
format                : 44100 Hz 2 ch s16le packet=512 frames
while streaming       : active=yes queued=3303 rendered=127976 dropped=1329 underruns=0   ← 3 s
while streaming       : active=yes queued=3482 rendered=349064 dropped=734  underruns=0   ← 8 s
```

**`dropped` 不是缺陷，而且已经验证过**：8 秒那轮的音频量是 3 秒的 2.7 倍，丢弃数却
更少（734 < 1329）。丢弃不随时长增长，说明它全部发生在启动瞬间——PipeWire 协商流的
那几十毫秒里 `process` 还没被调用，环先被填过 high-water（4096 帧），共享策略按设计
丢掉了积压而不是把延迟一直背着。之后稳态是 0 丢弃 0 underrun，队列稳定在 3.3–3.5 k
帧（约 75–79 ms）。

#### 与 WASAPI 的一处已知差异

WASAPI 侧拿不到输出端点时构造就失败，采集会话据此关掉音频。PipeWire 的
`pw_stream_connect` 是异步的：没有守护进程时它照样返回成功，之后状态转到
`PW_STREAM_STATE_ERROR`。所以这里**不**在构造函数里等——那需要凭空定一个超时常数。
错误状态会写日志，`stats().active` 是给上层的真实信号。这是差异，不是等价实现。

#### 一个留给星翼拍板的选项

环形缓冲的 memcpy 机械动作现在在 WASAPI 与 PipeWire 两份文件里各有一份（各约
15 行；所有*决策*逻辑已经共享）。要彻底去重就得把环本身从 `WasapiRenderer` 里抽出来，
那不是「接口抽取」而是搬状态，属于对上游文件的结构性改动，需要 Windows CI 复验。
**没有擅自做。** 现状是可接受的重复量。

### WP5-D 第一步：libplacebo 预览渲染器（已通过，无需设备、无需窗口）

`src/Core/src/Media/LinuxPlaceboRenderer.{h,cpp}`：把 `DecodedFrame` 变成显示就绪的
RGBA，这是取代 Windows D3D11 预览的那一半。

**第一步刻意停在「渲染出来、主机能读回」**。把图像导出成 dmabuf、再配上 Vulkan 信号量
交给 Avalonia 导入，是第二步，而且它属于那个要导入它的窗口（WP6）。分开的好处是：
色彩管线可以**单独**验证，不需要窗口、不需要设备。

帧是紧凑打包的半平面缓冲，所以两个平面各上传成一张纹理，再用 `pl_frame` 的
plane/component 模型描述给 libplacebo。色彩描述直接来自帧，不让 libplacebo 猜——把
`VideoColorDescription` 一路带到这里就是为了这个：**同一份元数据同时驱动这里的 GPU
路径和 `VideoFrameCopy.cpp` 里的 CPU 路径**，于是两者可以互相校验。

P010 的 10 位有效数据在每个 16 位字的高端，所以显式告诉 libplacebo
`sample_depth=16, color_depth=10, bit_shift=6`，否则它会当成 16 位信号。缩放用
`pl_rect2df_aspect_copy` 做 letterbox 而不是拉伸——手机的宽高比就是重点。

#### 验收：与两个独立参考同时对得上

`src/Core/tools/LinuxRenderProbe.cpp` 读一帧裸 NV12（**输入正是 `LinuxDecodeProbe` 的
输出，那份已经和 ffmpeg 逐字节相同**，所以这个工具测的确实是 GPU 色彩管线而不是解码器），
渲染、读回、逐像素与本项目自己的 `convert_yuv_to_sdr` 比较：

```
device                : NVIDIA GeForce RTX 4060 Laptop GPU
target                : 1170x2532
vs CPU colour maths   : mean=0.873 worst=163 at (1147,1256)
verdict               : PASS
```

再与 ffmpeg 的 RGB 转换独立对一次（抽样网格）：`mean=0.953 worst=91`。

**`worst` 大而 `mean` 小于一个通道步进，正是 4:2:0 该有的样子**：libplacebo 做色度插值，
CPU 参考取同位采样，硬色边上必然差得多，平坦区域则几乎一致。判据设在
mean ≤ 1 个通道步进，因为超过它就意味着两边对色彩描述的理解不同，而不只是插值不同。

肉眼复核了产出的 PPM：`testsrc2` 的色条顺序正确（红/绿/黄/蓝/洋红/青）、斜扫线连续无
撕裂、左上角时间码在**上方**（没有 S3 那个垂直镜像问题）、1170×2532 竖屏比例正确。

#### 第二步：导出内存 FD 与双向信号量（已通过）

`ExportedSurface` 给出外部 Vulkan 导入方需要的全部东西：内存 FD、**两个**信号量 FD、
`VkFormat`、分配大小与偏移。FD 的所有权留在渲染器手里，生命周期跟着它；导入方要更长的
生命周期就自己 `dup()`。

**两个方向的信号量是关键**，不是一个：`render_completed` 由渲染器发信、导入方等它之后
才采样；`available` 由导入方发信、渲染器等它之后才再动这张图。少了任何一个，导入方都会
采到画了一半的图。

平台不支持导出时（`export_caps.tex/sync` 不含 `PL_HANDLE_FD`）**不当致命错误**：
`exported_surface().valid == false`，读回路径照常工作。诚实报告比假装成功有用。

本机实测：

```
exported surface      : memory_fd=65 done_fd=66 free_fd=67 vk_format=37 size=12124160 offset=0
vs CPU colour maths   : mean=0.873 worst=163 at (1147,1256)
verdict               : PASS
```

`vk_format=37` 即 `VK_FORMAT_R8G8B8A8_UNORM`；分配大小 12,124,160 大于
1170×2532×4 = 11,849,760，是行距/平铺对齐的正常结果。

**并且加了导出之后读回结果与之前那一轮逐字节相同**——导出没有扰动渲染路径。这是这一步
能拿到的最强验证；真正把 FD 导入窗口是 WP6 的事。

CI 只**构建**不运行它：托管 runner 没有可用 GPU，而
`make_placebo_preview_renderer` 在那种情况下抛异常而不是假装成功。

### v1 明确不包含

- 虚拟摄像头（`src/VirtualCamera`）：Linux 侧应走 v4l2loopback 或 PipeWire 虚拟节点，
  保留 `VirtualCameraApi.h` 的接口形状，实现延后。
- BLE HID 鼠标/键盘控制：WinRT GATT → BlueZ D-Bus HOGP 属于独立模块，需真机验证
  iOS 辅助触控行为后另行安排。
- 应用内覆盖式更新器：Linux 交由包管理器，仅保留"检查并通知"部分。

## Spike 实测结论

以下数字来自本机（Arch Linux、KWin Wayland、Intel RPL-P iGPU + NVIDIA RTX 4060
Laptop 双 GPU），测试素材是 `testsrc2` 生成的 1170x2532@60 H.264 裸流，帧数 300。
**它们是本机实测值，不是跨硬件的普遍结论。**

### S3：Avalonia 能否呈现 libplacebo 渲染的图像 — 通过

这是 B1（Avalonia 而非 Qt/GTK）的 go/no-go 闸门，结论是**成立**。

Avalonia 12.1.1 的 `ICompositionGpuInterop` 只接受 `VulkanOpaquePosixFileDescriptor`
一种图像句柄类型，同步能力只报告 `Semaphores`（不含 `TimelineSemaphores`）。libplacebo
的 `pl_tex_create(export_handle = PL_HANDLE_FD)` 与 `pl_vulkan_sem_create` 导出的句柄
正好满足这两项。

| 渲染后端 | 图像句柄类型 | 结果 |
|---|---|---|
| `X11RenderingMode.Vulkan` | `VulkanOpaquePosixFileDescriptor` | 300/300 帧呈现，PASS |
| `X11RenderingMode.Glx` | `VulkanOpaquePosixFileDescriptor` | 300/300 帧呈现，PASS |
| `X11RenderingMode.Egl` | 空列表 | 无法导入，FAIL |

- 生产者 GPU 开销（`pl_vulkan_release_ex` + 渲染 + `pl_vulkan_hold_ex`）：
  mean 0.097 ms，p95 0.152 ms。
- 端到端（生产者 + 合成器呈现完成）：mean 5.9 ms，p95 6.1 ms。这个数字里主要是
  合成器的 vsync 等待，**不能当成渲染成本**。屏幕是 1920x1080@165Hz。
- `VK_QUEUE_FAMILY_IGNORED` 与 `VK_QUEUE_FAMILY_EXTERNAL` 两种所有权移交方式实测
  没有差异，按 Avalonia 导入端不发 acquire barrier 的事实，应当用 `IGNORED`。
  shim 现已把队列族固定为 `IGNORED`，托管侧不再暴露这个旋钮。
- **呈现方向（2026-09-05 新增）**：Avalonia 导入端采样共享图像的 Y 方向与
  libplacebo 渲染方向相反。渐变素材看不出来，真实视频第一张截图就是垂直镜像的。
  shim 通过把渲染目标声明为 `flipped`（`pl_frame_from_swapchain` 的
  `pl_swapchain_frame.flipped = true`）修正。**WP5 的正式渲染器必须保留这一点。**

对 P5 的硬约束：
1. **`Egl` 渲染后端不可用**，Linux 端必须强制 `X11RenderingMode.Vulkan`，并在拿不到
   GPU interop 时明确降级而不是静默黑屏。
2. **导入端必须与合成器同一物理设备。** `ICompositionGpuInterop.DeviceUuid` 报告的是
   合成器选中的 GPU，本机是 iGPU；libplacebo 默认偏好独显。生产者必须用
   `pl_vulkan_params.device_uuid` 显式对齐，否则 opaque FD 内存无法跨物理设备共享。
3. Avalonia 的导入端会校验 `MemorySize` 必须**恰好等于**
   `vkGetImageMemoryRequirements().size`、`MemoryOffset` 必须为 0，并且只认
   `R8G8B8A8_UNORM` / `B8G8R8A8_UNORM`。libplacebo 的 `rgba8` 导出满足全部三条。
4. Avalonia 12.1.1 有 `Avalonia.Wayland` 包，但**渲染后端选项里没有 Vulkan**，
   `WaylandPlatformOptions` 只有 `GlProfiles` 与 `UseDmabufSwapchain`。因此 Wayland
   会话下应走 XWayland + `Avalonia.X11`；原生 Wayland 后端要等上游补 Vulkan 支持。

### S4：解码 → libplacebo 渲染 — 通过，但零拷贝有硬件约束

| 组合 | 解码 | 渲染 GPU | 结果 | 每帧 |
|---|---|---|---|---|
| VAAPI Intel + Vulkan Intel | vaapi | iGPU | **300/300** | 4.10 ms |
| VAAPI Intel + Vulkan Intel + 可导出目标 | vaapi | iGPU | **300/300** | 4.14 ms |
| 软解 + Vulkan NVIDIA | yuv420p | dGPU | 300/300 | 1.74 ms |
| 软解 + Vulkan Intel | yuv420p | iGPU | 300/300 | 3.64 ms |
| VAAPI Intel + Vulkan NVIDIA（跨设备） | vaapi | dGPU | **0/360，全部 map 失败** | — |
| VAAPI NVIDIA + Vulkan NVIDIA | vaapi | dGPU | **0/360，全部 map 失败** | — |

**跨设备失败的根因已定位（FACT）：DRM format modifier 不兼容。**
`av_hwframe_map` 导出的 dmabuf 带有硬件专属的 tiling modifier，
`pl_map_avframe_drm` 会用 `pl_fmt_has_modifier` 校验：

- Intel iHD 导出 modifier `0x0100000000000002`（Intel Y-tiled）。Intel Vulkan 接受，
  NVIDIA Vulkan 拒绝。
- NVIDIA VAAPI（`libva-nvidia-driver`）导出 modifier `0x0300000000606014`
  （NVIDIA block-linear）。NVIDIA Vulkan 接受，Intel Vulkan 拒绝。

**NVIDIA 路径还有第二个独立的阻塞点**：`libva-nvidia-driver` 的 NV12 色度层用
fourcc `RG88`，而 libplacebo 只注册了 `GR88`（分量顺序相反），`pl_find_fourcc(RG88)`
直接返回 NULL。即使 modifier 匹配也过不去。

对 P4 的结论：
1. 解码设备与渲染设备**必须是同一块 GPU**。要用 `VK_EXT_physical_device_drm` 的
   `renderMinor` 把 Vulkan 物理设备映射回 `/dev/dri/renderD*`，再据此选 VAAPI 设备，
   而不是硬编码节点号。本机映射是 iGPU→`renderD129`、dGPU→`renderD128`，
   **与"iGPU 一定是 128"的直觉相反**。
2. 由于 S3 要求渲染设备必须等于合成器设备，链条被完全钉死：
   **合成器 GPU → Vulkan 物理设备 → DRM render node → VAAPI 设备**。
3. NVIDIA + VAAPI 组合在当前版本组合下零拷贝不可用，必须有软解回退路径。软解 300 帧
   全部成功，1170x2532 下 CPU 约 1.4 s user time / 300 帧，可用但不是长期方案；
   后续应评估 NVDEC/CUDA interop 或 Vulkan Video（本机 Intel Vulkan **不支持**
   `VK_KHR_video_decode_queue`，NVIDIA 支持）。
4. 渲染目标带 `export_handle = PL_HANDLE_FD` 后性能无明显变化（4.10 → 4.14 ms），
   说明 S3 要求的可导出目标不会拖累 S4 的解码渲染路径。

**托管探针端到端复核（2026-09-05，shim 升级为真实解码渲染器后）**：
S3 的 Avalonia 探针接上 FFmpeg 解码后再跑一遍同一素材（1170x2532@60，30 s，
1800 帧），与上表 C 探针的数字互相印证：

| 路径 | 结果 | 生产者每帧 | 端到端每帧 |
|---|---|---|---|
| VAAPI（`h264 (vaapi)`，renderD129）| 1800/1800 PASS | 1.9–3.4 ms | 6.8 ms |
| 强制软解 | 1800/1800 PASS | 9.0 ms | 12.4 ms |

报告里的 `decoder: h264 (vaapi)` 行与 VAAPI/软解的每帧差值（约 5–7 ms）
互相印证硬件解码确实生效。`zero_copy` 字段在 `pms_describe` 时（首帧之前）
尚不可知，所以探针打印的是建流快照；WP5 的正式渲染器拥有完整生命周期后
再如实上报。

### S1 / S2：无设备部分已通过

- S2 已确认本项目自带的 `Plist.cpp` 与 usbmuxd 的线协议**字节级兼容**：AF_UNIX
  连接成功，`ListDevices` 回包 `length=237 version=1 message=8 tag=1` 解析正常，
  设备数 0（未接设备）。
- S1 的 libusb 只读枚举路径已跑通，`Apple devices found: 0`。
- 两者都还需真机才能得出结论，见下方"验证环境"。

## 验证环境

适配工作的真机验证设备：

| 设备 | ProductType 族 | 系统 |
|---|---|---|
| iPhone 16 Pro | iPhone17,1 族 | iOS 27 Beta 4 |
| iPad Air M3 | iPad15,x 族 | iPadOS 27 Beta 4 |

> iOS/iPadOS 27 Beta 属于未发布系统，Apple 私有 Screen Capture 协议与 AirPlay
> 行为都可能与正式版不同。基于 Beta 观察到的结论必须标注系统版本，不能当作正式版
> 的既定事实。

## 安全与风险记录

### USB 配置切换

启用隐藏的 QuickTime 采集配置需要对设备发送 `SET_CONFIGURATION` 与厂商请求，会触发
USB 重枚举。已知影响与处置：

- Linux `usbmuxd` 会占用 Apple class `0xFE` 接口，重枚举期间可能丢失设备；共存策略
  必须由 S1/S2 spike 的结论决定，不得凭假设实现。
- 任何配置切换都必须带显式回滚，并在进程退出路径上强制恢复常规配置。
- 该操作**不触及固件或启动链，没有变砖风险**；最坏情况是设备停在采集配置，重新插拔
  数据线即可恢复。

### 验证顺序

真机验证一律遵循：只读枚举 → 读取描述符 → 才允许一次受控的写操作。

## Spike 复现方式

Spike 默认不构建。开启方式：

```sh
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DIPHONEMIRROR_BUILD_LINUX_SPIKES=ON
cmake --build build/linux
```

| Spike | 命令 |
|---|---|
| S1 | `./build/linux/tools/linux-spikes/iPhoneMirror.Linux.AppleUsbEnumerationProbe` |
| S2 | `./build/linux/tools/linux-spikes/iPhoneMirror.Linux.UsbMuxUnixSocketProbe` |
| S4 | `./build/linux/tools/linux-spikes/iPhoneMirror.Linux.DecodeRenderLatencyProbe <input> [--software] [--device /dev/dri/renderDN] [--vulkan-device NAME] [--export-target] [--frames N] [--verbose]` |

S3/S4 是托管进程，需要 .NET SDK 与真实合成器，并要能找到 CMake 构建出的
`libiPhoneMirror.Linux.PlaceboSurfaceShim.so`。探针是双模式的：不带 `--input`
跑固定帧数的渐变并打印 PASS/FAIL 与延迟统计（S3 的可复现口径）；带 `--input`
解码真实视频常驻播放（S4 的目视验证口径）：

```sh
cd tools/linux-spikes/AvaloniaSurfaceProbe

# S3 口径：渐变 300 帧 + 统计
LD_LIBRARY_PATH=../../../build/linux/tools/linux-spikes \
  dotnet run -- --frames 300 --rendering-mode vulkan

# S4 口径：真实视频（可加 --software 强制软解、--loop 循环播放）
LD_LIBRARY_PATH=../../../build/linux/tools/linux-spikes \
  dotnet run -- --input /tmp/ipm_s4/phone_screen.mp4
```

测试素材按"一眼能判断对不对"合成（帧号/时间码查解码与同步，移动方块查撕裂，
色条查色彩；正立与否直接查呈现方向）：

```sh
tools/linux-spikes/make_test_asset.sh   # 生成 /tmp/ipm_s4/phone_screen.mp4
```

S3 的 `global.json` 与仓库根的不同（本机 SDK 10.0.111 而非 10.0.301），这是刻意的：
spike 不参与 `build.ps1`，也不进 Windows 发布流程。

## 第三方组件

本分支引入的第三方组件及其许可记录在 [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)。
Linux 分支不再 vendored 任何预编译二进制，全部依赖系统库或从源码构建。
