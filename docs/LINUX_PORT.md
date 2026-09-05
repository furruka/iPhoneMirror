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
| P1-S1 | spike：libusb 只读枚举 + 读隐藏配置描述符（**需真机**） | `[~]` |
| P1-S2 | spike：AF_UNIX usbmux `ListDevices`（**需真机**） | `[~]` |
| P1-S3 | spike：Avalonia 嵌入原生 surface | `[x]` |
| P1-S4 | spike：解码 → dmabuf → libplacebo 出画并测延迟 | `[x]` |
| P2 | `CaptureSession.cpp` 抽缝共享（方案 X；`wchar_t` 保留，`ApiVersion` 保持 18） | `[x]` |
| P3-WP3 | `LinuxCoreApi.cpp` + `LinuxDeviceManager` + `LinuxEnvironmentProbe` + udev 规则 | `[x]` |
| P3-WP4 | 重枚举恢复策略 + libudev 监视器 + 无头采集工具（**真机未验**） | `[~]` |
| P3 | Linux USB 采集（无头验证优先，**需真机**） | `[ ]` |
| P4 | FFmpeg 解码 / libplacebo 渲染 / PipeWire 音频 | `[ ]` |
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
  `Media/ActiveVideoDecoder.h` 的工厂按平台构造。Windows 仍是 Media Foundation
  与 WASAPI；Linux 目前是 `LinuxMediaStubs.cpp`，解码器构造即抛
  "not implemented yet (WP5)"，音频渲染器沿用同一套格式校验后静默丢弃。
  **桩不会假装成功**——WP5 落地 FFmpeg 解码器与 PipeWire 渲染器时替换。
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

### WP4：重枚举恢复（设计已就绪，**真机未验**）

这是 §4.1 那条最高风险约束的对策。Windows 不需要它：AppleUsbFilter 会保留
0x52 选中的配置。Linux 上 usbmuxd 的 `39-usbmuxd.rules` 含
`ACTION=="add", ATTR{bConfigurationValue}="0"`，所以设备每次重新出现，udev 都把
活动配置写回 0，`libusb_claim_interface` 随即失败——**主机必须自己重发
SET_CONFIGURATION**。

- `Capture/UsbReenumerationPolicy.h`：纯状态机。要求 QuickTime 配置在**连续两次
  采样**中都活着才允许 claim（一次 udev 处理窗口过去了才算稳定），把重发次数
  限死在 5 次，并**统计配置被覆盖的次数**——那个计数就是这条 HYPOTHESIS 的证据。
  单测 `UsbReenumerationPolicyTests` 覆盖了分类优先级、稳定性要求、覆盖计数、
  预算耗尽后不谎报成功。
- `Device/LinuxUdevMonitor.{h,cpp}`：libudev netlink 监听。轮询 libusb 只能回答
  「设备在不在」，答不了「它刚刚重新出现」，而后者才是要抓的那个边沿。
  `bConfigurationValue` 从 sysfs 读，**不需要打开设备**，所以在 udev 还没授权
  节点的窗口里也读得到。用 devpath 末段（端口链）作为跨重枚举的稳定身份——地址
  和 product id 都会变。
- `Transport/LinuxUsbConfiguration.{h,cpp}`：Linux 专用的 SET_CONFIGURATION。
  `LIBUSB_ERROR_BUSY` 表示内核驱动（usbmuxd）还占着接口，是预期竞争而非故障。
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

**未验证项（全部等真机）**：0x52 后 udev 是否真的把配置写回 0、写回几次；
重发 SET_CONFIGURATION 能否让配置留住；usbmuxd 是否会抢回设备；
`uaccess` ACL 是否覆盖 `/dev/bus/usb` 节点；握手状态机在 iOS 27 Beta 4 上的行为。

P1 的构建结论：`src/Core` 的 5 个可移植翻译单元（Protocol / Media / CoreMedia / H264）
在 GCC 16.2 与 Clang 22.1 下都能构建出 `libiPhoneMirror.Core.so`，`ctest` 3/3 通过
（`OutputModeStateTests`、`UsbConfigurationRestorePolicyTests`、
`DnsSdRegistrationPolicyTests`）。Windows 侧目标全部按平台裁剪，未做行为改动。

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
