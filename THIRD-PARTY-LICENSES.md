# 第三方许可汇总（THIRD-PARTY LICENSES）

本发布包内所有第三方组件的许可信息。本移植本体（bbport 的自有改动与启动链）以
**GPL-2.0-or-later** 发布；下表组件按其自身许可随包分发。各条目的完整许可文本
可在括号内的仓库路径（源码树）或上游链接找到。

## 随包分发的组件

| 组件 | 许可 | 版权 | 来源 |
|---|---|---|---|
| bbport（本移植，自有改动与启动链） | GPL-2.0-or-later | bbport 作者 | 本仓库 |
| shadPS4 渲染核（video_core 适配层） | GPL-2.0 | shadPS4 贡献者 | github.com/shadps4-emu/shadPS4 |
| Dear ImGui 1.92.9b（游戏内菜单） | MIT | ocornut 及贡献者 | github.com/ocornut/imgui |
| FSR-Vulkan（FSR 3/4 Vulkan 提供者，含本项目的 INT8/DOT4 移植） | MIT | Q2RTX FSR Vulkan contributors, 2026 | github.com/FireBurn/Q2RTX |
| FSR4 v07 模型资产（fsr4_shaders/ 的 .bin/.spv） | MIT | Advanced Micro Devices, Inc., 2025 | 经 Q2RTX 构建，见 LICENSE-FSR4-v07.txt |
| LibAtrac9（Atrac9 音频解码） | MIT | Alex Barney, 2018 | github.com/Lyrsn/LibAtrac9 |
| half（半精度浮点） | MIT | Christian Rau | github.com/halfmanhalfpillow/half |
| sirit（SPIR-V 构建库） | BSD-3-Clause | sirit 贡献者, 2019 | github.com/Ryujinx/sirit |
| AMD GCN 寄存器/PM4 头表（gcn/include） | MIT | Advanced Micro Devices | github.com/GPUOpen-Drivers |
| DejaVu Sans 字体（已嵌入 bb-probe.exe） | Bitstream Vera 许可 + 公有领域修改 | Bitstream / DejaVu 贡献者 | dejavu-fonts.github.io |
| SDL3（窗口/输入/音频；SDL3.dll） | zlib | Sam Lantinga, SDL 贡献者 | github.com/libsdl-org/SDL |

## 静态链接进 bb-probe.exe / bbgpu 的构建期组件

| 组件 | 许可 | 用途 |
|---|---|---|
| Vulkan Memory Allocator (VMA) | MIT | 显存分配器 |
| magic_enum | MIT | 枚举反射 |
| miniz | MIT | 归档解包 |
| Xbyak | BSD-3-Clause | 运行时代码生成 |
| SPIRV-Headers | MIT（Khronos） | SPIR-V 定义 |
| Boost.Asio（standalone） | BSL-1.0 | 内核事件队列定时器 |

## 不随包分发、由用户自取的组件

| 组件 | 许可 | 获取方式 |
|---|---|---|
| nvngx_dlss.dll（DLSS） | NVIDIA SDK 许可 | tools/fetch_dlss.sh 或 NVIDIA 官网 |
| Bloodborne.xml（社区补丁集） | 无再分发许可（各作者保留权利） | 用户放置，见 RELEASE.zh.md |

## 本移植的构建依赖（不随包分发）

MSYS2 CLANG64 工具链（clang、cmake、ninja、pkg-config、glslang——Apache-2.0/Khronos）、
Vulkan 头文件与加载器（Apache-2.0/MIT，Khronos）。这些只参与编译，生成的二进制不含
其代码。
