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
| P1-S1 | spike：libusb 只读枚举 + 读隐藏配置描述符（**需真机**） | `[~]` |
| P1-S2 | spike：AF_UNIX usbmux `ListDevices`（**需真机**） | `[~]` |
| P1-S3 | spike：Avalonia 嵌入原生 surface | `[x]` |
| P1-S4 | spike：解码 → dmabuf → libplacebo 出画并测延迟 | `[x]` |
| P2 | Core 抽平台缝；ABI 改 `im_char`，`ApiVersion` 18→19 | `[ ]` |
| P3 | Linux USB 采集（无头验证优先） | `[ ]` |
| P4 | FFmpeg 解码 / libplacebo 渲染 / PipeWire 音频 | `[ ]` |
| P5 | Avalonia GUI | `[ ]` |
| P6 | UxPlay 引擎无线接收 | `[ ]` |
| P7 | 打包、CI、文档 | `[ ]` |

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

S3 是托管进程，需要 .NET SDK 与真实合成器，并要能找到 CMake 构建出的
`libiPhoneMirror.Linux.PlaceboSurfaceShim.so`：

```sh
cd tools/linux-spikes/AvaloniaSurfaceProbe
LD_LIBRARY_PATH=../../../build/linux/tools/linux-spikes \
  dotnet run -- --frames 300 --rendering-mode vulkan
```

S3 的 `global.json` 与仓库根的不同（本机 SDK 10.0.111 而非 10.0.301），这是刻意的：
spike 不参与 `build.ps1`，也不进 Windows 发布流程。

## 第三方组件

本分支引入的第三方组件及其许可记录在 [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)。
Linux 分支不再 vendored 任何预编译二进制，全部依赖系统库或从源码构建。
