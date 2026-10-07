# 时域超分与帧生成 — 帧分析与方案

## 原生 TAA 与缺失的 FSR 4 资产（2026-10-01）

`upscaler=taa` / `BB_UPSCALER=taa` 是一种独立的原生分辨率时域 AA 模式，
在启动器与游戏内菜单中实时可用。它在单个 compute pass 中结合已有的 jitter 与
相机/物体运动矢量、重投影、深度剔除与 3×3 邻域裁剪，配两套 ping-pong 的
RGBA16F 历史。历史把深度存进 alpha；场景原本的 alpha 由 HDR 合并保留。
HUD 在之后合成，永不进入历史。输出变化与 AA/provider 变化会重置历史。
TAA 使用 Native AA 的尺寸，同时保留已保存的 FSR 预设，且不分配 FSR context、
不执行 FSR 模型/RCAS。即便如此，相比关闭时域 AA 它仍增加 GPU 工作；
原生 4K 的光栅化代价也依然存在。必须取消设置显式的 `BB_RENDER_RES` 兼容路径。

FSR Native AA 不是廉价的直通：它按输出分辨率渲染，并且同样执行时域重建
（FSR 4 还包含模型）与可选的锐化。因此在同一输出下，它可能比不做 AA 更慢。

FSR 4.1.1 资产有分别对应 1080p/2160p 的 tier 与standard/Ultra 模型：
`t1080_m0`、`t1080_m1`、`t2160_m0`、`t2160_m1`。高于 1080p 的输出需要 2160
tier，与预设无关。缺少 `t2160_m0/spd.spv` 意味着资产缺失，而非 GPU 特性失败。
这些资产不随仓库分发：用 `tools/fsr4cap/build_assets.sh` 构建完整集合，
放入 `BB_FSR411_DIR` 或打包数据目录的 `fsr4_411`。资产上的致命失败现在会在
实时设置中回退到 FSR 3.1，因此菜单会显示当前生效的 provider、保留错误信息，
并允许在改变输出/安装资产后重试 FSR 4。

`taa-shader-test` 在 Vulkan 上检查真实 SPIR-V 输出的累积、裁剪、深度解遮挡、
无效历史与出框运动（Lavapipe 可用）。`out/taa-appimage-build.log` 记录这些检查、
设置往返与 64 个 Python 测试。`out/taa-live-validation.log` 与
`out/taa-appimage-validation.log` 在单个游戏进程内检查 1080p/720p/1440p/4K 下的
TAA、TAA ↔ FSR 3 切换，以及缺失 2160 tier 的 FSR 4.1.1 模型，含输出截屏与相机移动。
`out/taa-launcher-validation.log` 检查实际的 GTK 控件与保存的设置。
TAA 尚未在测试者的 GTX 1060 上验证。

## 非 1080p 输出重新默认走启动期补丁（2026-10-02）

下面的实时路径让 Steam Deck 与 GTX 1060 + 4 核 Haswell 掉到 7–8 FPS
（0.1 版 AppImage：Deck 上 40–50 FPS）。客户分配固定在 1920×1080 时，
即便输出 720p，游戏后处理仍停在 1080p；客户 compute pass 每帧都要把场景代理
resolve 回 1080p（RX 7800 XT，4 核上 720p FSR 3 Performance：826 对 576
draw/帧，185 对 210 FPS）；而在没有 shader stencil export 的 GPU 上，
每次 depth/stencil 拷贝要占九次 draw。

- `run.sh` 再次为非 1080p 输出修补游戏渲染尺寸（与 0.1 相同），并以
  `BB_AUTO_RENDER_RES=1` 标记，使游戏内重启能重新计算。此后预设与输出的变化
  需要重启（菜单会提供该选项）。`BB_LIVE_RES=1` 选择实时路径。
  1080p 输出与 TAA 保持实时路径。
- depth/stencil 场景代理再次需要 `VK_EXT_shader_stencil_export`；没有它时这些
  目标保持原生（与 0.1 相同）。`BB_SCENE_STENCIL_BITS=1` 仍强制使用可移植的
  重采样器以供测试。

## 实时分辨率变更（2026-10-01）

游戏内输出选择器现在可在 display pass 边界上调整 720p/1080p/1440p/2160p 的
宿主目标尺寸。预设按 `输出 / scale` 计算，包含高于 1080p 的光栅尺寸
（4K Quality：2560×1440；Native AA：3840×2160）。客户分配保持 1920×1080；
`run.sh` 不再为常规输出选择插入分辨率补丁。显式的 `BB_RENDER_RES` 兼容覆盖
仍然保留。

`SceneTargets` 在改尺寸前先解析并退休旧代理。FSR 资源与历史为新的输入/输出
重建，FSR 3 与 FSR 4 都有完成等待。1080p 路径在游戏后处理之前保留 HDR 重建；
其他输出在第一个 Scaleform pass 处重建已tonemap 的场景代理，随后把 HUD/菜单
几何绘入输出尺寸的目标。显示缓冲也随之调整尺寸。影片着色器识别防止原生尺寸的
全屏后处理 pass 被误认为 UI。

客户 compute pass 保留其原生 dispatch 尺寸，且可以把代理 resolve 回 1080p。
本变更并不缩放每一个后处理 pass；性能与中间细节可能与旧的启动期修补渲染尺寸不同。

验证：`out/dev/live-resolution-summary.log` 在单个 PID 内切换输出、预设与 FSR 开/关。
`BB_PRESENT_TRIGGER` 把完成的宿主显示缓冲（含 HUD）捕获到 `BB_DUMP_DIR`。
截图确认了实际的 720p/1440p/2160p 缓冲。`scene-resolution-test` 覆盖了
经代理的 color/depth/stencil 往返，代理尺寸既可小于也可大于客户分配。
`BB_PRESET_FILE` 接受 `preset [output-index [upscaler-index]]`，用于脚本化
的等价菜单变更。

帧分析器：`BB_CAPTURE_TRIGGER=<file> BB_CAPTURE_DIR=<dir>`；创建该文件会记录
下一帧（边界为写入显示缓冲的那个 pass）—— pass、目标、着色器、采样纹理，
以及小 pass 与首批 draw 的绑定常量前 1 KiB。

## Bloodborne 的帧（1920x1080，猎人之梦 / 噩梦）

| Pass | 内容 |
|-----------------------|-------------------------------------------------------------------------------|
| 帧的首个 pass | 把上一个 UI 目标拷贝到显示缓冲（sRGB） |
| shadow | 4096x4096 D32 深度 |
| G-buffer | 6 个 1920x1080 目标（RGBA8 x3、sRGB albedo、B10G11R11、RGBA16F）+ D32S8 深度 |
| lighting | 光照体积累到两个 B10G11R11 目标 |
| scene color | RGBA16F 1920x1080，同时承接前向/透明绘制与特效 |
| volumetric fog | compute，读取线性深度（R32F 1920x1080）并合成进场景色 |
| half-res effects | RGBA8 960x540 配半分辨率深度 |
| post | RGBA16F / B10G11R11 中的 combine/bloom 金字塔 |
| tonemap | 到 RGBA8 1920x1080（即 UI 目标） |
| game AA | tonemap 之后的 ping-pong RGBA8 1920x1080（超分时应跳过） |
| UI | 在同一 RGBA8 目标上的 stencil 掩码绘制 |

没有速度缓冲，即便相机在动：运动矢量是算出来的。

## 场景常量（864 字节，多数 pass 会绑定）

签名：`[0]=3000 (far) [1]=1/3000 [4]=1920 [5]=1080 [6]=1/1920 [7]=1/1080`。

| 浮点索引 | 含义 |
|----------------|-------------------------------------------------------------------------------------------------------------|
| 8–19 | view 矩阵，3x4 行（旋转 + 平移）；唯一随相机变化的块 |
| 36–51 | 逆投影（0.700285 = 1/1.42799，0.39391 = 1/2.53865） |
| 52、57、62、63 | 投影（第 52–67 行），D3D 深度 0..1：x 1.42799，y 2.53865，z 1.00002 / -0.0500679（near 0.05，far 3000） |
| 112–175 | 阴影级联矩阵 |
| 180–191 | 逆 view（相机到世界），3x4（176–179：2、8、15、0） |

上一帧的矩阵不在其中；移植自己保留它们。

## 方案

1. 找到每帧的场景常量（签名），保留上一帧的 view/投影。
2. 相机运动矢量：由深度与当前/上一帧矩阵算出、写入 RG16F 目标的 compute pass；
   配调试视图检查。物体运动稍后做（用重编译器以��一帧的常量重放顶点着色器）。
3. Jitter：场景 pass 中裁剪空间位置的亚像素偏移（重编译器）。
4. 超分器放在场景色阶段（后处理与 UI 之前）：先 FSR 3.1（开源、原生 Vulkan，
   也支持帧生成），然后 DLSS（Linux 上原生），XeSS/XeFG 与 OptiScaler 通过加载器
   使用其 Windows DLL。

## 状态（2026-10-07 时点，中文为准）

- 相机运动矢量：已实现并用上一帧重投影验证（`BB_DEBUG_MOTION=1`，toggle 1<<22 重投影帧、1<<23 误差图）：静态几何吻合。
- FSR 3.1（`BB_UPSCALER=fsr3`，FireBurn/FSR-Vulkan 子模块）：后处理合成前的场景色（compute shader 9a9cf8a9），1:1，RGB 写回（游戏在 alpha 通道保留数据）。toggle 1<<24 运行时关闭。
- Jitter：场景几何的视口偏移（带场景深度的几何，非全屏 quad），Halton(2,3) 8 相；符号用锐化校验（正确 255、翻转 208、关闭 271）。toggle 1<<25 关闭。
- 物体运动矢量：已实现（2026-09-30，顶点着色器以上一帧场景常量重放）；FSR 4 Ultra Performance 档角色仍有残影。
- 渲染分辨率缩放：已实现（预设 + bbport.ini `output_res` 1440p/2160p，UI 原生输出分辨率）。
- DLSS：已实现（Windows 版，vk_dlss.cpp），RTX 卡上为推荐档。
- 静止站立时帧间差异：FSR 下约降低 33%。
- 仍未做：粒子与雾的反应性/透明遮罩（现用启发式 mask）、帧生成、场景精灵（≤6 索引全屏规则）不随 jitter 移动。

## 2026-10-01：FSR 4 在 1440p/2160p 输出下不工作（闪烁与抖动）

视频 `video_2026-10-01_03-58-59.mp4`（输出 3840x2160，FSR 4 Performance）：
所有物体闪烁与抖动。原因——该模式下超分器根本没有执行，而 jitter 仍处于
开启状态：屏幕上是被拉伸的场景帧，每帧按各自的 Halton 相位偏移。

1. UI pass 的识别依据是 `BB_RENDER_RES` 的精确目标尺寸（1916x1078），而游戏分配的
   目标高度是对齐的（1916x1080；场景常量中也是 1916x1080）。`RunScaled` 一次都没被
   调用（日志里没有 `UI: native composition`）。现在允许最多 8 像素的对齐
   （`RenderTarget`），而 FSR 的场景尺寸取自场景常量
   （`CameraMotion::RenderSize`、`SceneSize`）。
2. 之后 FSR 4 以 `external image registration failed (-1000069000)` 失败：
   `RunScaled` 每帧都创建新的 image view，而 FSR 4 的注册表只能容纳八个。
   现在改用与 Native AA 路径相同的那些 `CachedView`。

用 dump 验证（`BB_DUMP_TRIGGER=<文件> BB_DUMP_DIR=<目录>`、`BB_DUMP_FRAMES`，
默认 8：FSR 输入、运动矢量与输出、raw）：相机静止时输出相邻帧的 PSNR 约 45 dB，
而带 jitter 的输入约 28 dB；相机转动与行走时无拖尾。该模式下 FPS 约 107 而非约 220
—— 此前 FSR 4 根本没执行。

遗留问题：精灵（每个 4 个索引）以场景深度写入场景色 —— 发光、火光 —— 仍不随
jitter 移动（沿用「≤ 6 个索引 = 全屏 pass」的规则）。

## 2026-10-01：FSR 4 更快 —— post pass（4K 下 2.9 → 0.8 ms）

测量：`BB_FSR4_PROFILE=1`（FSR 4 各pass 的时间，provider 的补丁在子模块中，
见 `gpu/patches/fsr-vulkan`），游戏外的基准 `out/gpu/fsr4-bench`
（`ninja -C out/gpu fsr4-bench`；`--stats` 给出 RADV 的寄存器与指令数）。
4K Balanced（2260x1272 -> 3840x2160），RX 7800 XT：整个 FSR 4 为 5.8 ms，
其中 **post 占 2.5–2.9 ms**（不是神经网络：最后几层、2x2 的 pixel shuffle
以及与历史的混合），模型的 12 个 pass 为 2.4 ms，pre 0.55。

原因：每个线程计算一块 2x2 的输出像素，并把它们逐个写入三个图像
（循环状态、历史、输出）—— 每次写指令的波形都逐个写像素。去掉三次写入中的任意一次，
pass 都会在代码不变的情况下加速 1.3–4 倍。

方案：`tools/fsr4_optimize.sh` 反编译 post（spirv-cross），
`tools/fsr4_post_lds.pl` 把 16x16 的工作组块放进 shared memory 并以整行写入，
glslang 编译回 `fsr4_shaders/opt/`；`vk_fsr4.cpp` 从那里取用
（`BB_FSR4_OPT=0` 用原始版本）。通过对比输出发现两处微妙之处：
- spirv-cross 把 int8 的有符号解包（`i8vec4` 中的 `OpBitcast`）翻译为
  `unpack8(uint)`（无符号）—— 不修正的话结果完全不同（PSNR 17 dB）；
- shared memory 中的值必须保持 float：编译器会把 half 变成 16 位写入，
  而那会以不同方式舍入到 unorm8。

`tools/fsr4_verify.sh` 在伪随机输入下比较所有预设、1080p/1440p/4K 输出的
原始与优化后 post：1440p 与 4K 逐位相同；4K post 2.1–3.5 -> 0.81–0.87 ms，
1440p 1.2–1.5 -> 0.36–0.38 ms，1080p 0.37–0.69 -> 0.20–0.22 ms。
游戏中（4K Balanced）：FSR 4 5.8 -> 4.0 ms，GPU 帧 13.1 -> 约 11.6 ms；
此后 FPS 受限于 CPU（GPU 命令线程等待客户内存的拷贝，「host copies」约 20%）。

顺带发现：**原始 FSR 4 在 1080 级别（输出 1920x1080）是非确定性的** ——
相同输入下每次运行，左边缘（0–132 列）的条带都会变化，也就是模型/provider 中某处
存在竞态或未初始化内存读取。在 1080p 输出下这可能造成帧左边缘闪烁。未深入研究。

FSR 4 下不再计算反应性遮罩（FSR 4 不接受它）。

### 同期还验证了什么（同为 4K Balanced）

- **单进程内 A/B**（`ab.sh`，位 24—— 关闭 FSR，UI 拷贝不做超分）：
  带 FSR 4 为 86.5 FPS，不带为 87.7。post 优化后帧受限于 CPU（GPU 命令线程
  等待客户内存拷贝），而非 GPU。为 FSR 做 async compute（与下一帧开头重叠）
  带来的收益不超过约 1% —— 推迟到 CPU 部分提速之后；何况它需要把 UI 与第 N 帧
  输出的命令重排到第 N+1 帧工作之后。
- **WMMA（`VK_KHR_cooperative_matrix`，RDNA3 上的 RADV 支持）**：
  模型 pass 1 的原型（残差块，16 通道：3x3 的 16->16、1x1 的 16->32 ReLU、
  1x1 的 32->16）—— 第一版 0.85 ms，shared memory 中按字面布局为 0.49 ms，
  而原始 dot4 为 0.30 ms。WMMA 上的算术只需约 0.1 ms，但在 16 通道下瓦片布局、
  经 shared memory 的尾声处理与占用率（6 波形/SIMD，受 LDS 限制）会吃掉这些收益。
  上限是全部 12 个 pass 约 1 ms，但需要把每个都手工细改；未开始。
- `RADV_PERFTEST=cswave32`（compute 用 wave32）：FSR 4 更慢，4.2 -> 5.4 ms。
- pre pass（0.54 ms）的写入不是瓶颈（去掉写入为 0.50 ms）。

## 2026-10-01：1080p 输出下 FSR 4 左边缘闪烁 —— 模型 pass 11 中的竞态

1080 级别的非确定性（见上）—— 是 v07 模型着色器的 bug，pass 11（解码器，
1/4 -> 1/2 分辨率）。每个线程对应 1/4 分辨率的一个输入像素，写出 1/2 分辨率的
一块 2x2 输出。dispatch 把宽度向上取整到 64 线程，而超出输入宽度的线程并不停止：
在 1080 级别（输入宽 480），线程 480..511 会写出输出像素 960..1023，
也就是下一行的前几个像素（任意张量的一行是 15360 字节），并与它们真正的作者竞态。
在 4K 输出下宽度 960 可被 64 整除，没有这个错误。

`tools/fsr4_pass11_guard.pl` 为越界线程加入提前退出（尺寸取自该 pass 自身的
邻域检查）；`tools/fsr4_optimize.sh` 连同 post 一起为所有预设构建到
`fsr4_shaders/opt`；`vk_fsr4.cpp` 与基准都从 `opt/` 取任意 pass。
`tools/fsr4_verify.sh`：1440p/4K 与原始版本逐位相同（保护在那里不触发）；
1080p 每次运行结果一致，与原始版本的差异在首帧之后只出现在原先左边缘的那条带里。

同时发现：spirv-cross 把所有 int8 有符号解包（`v4char` 中的 `OpBitcast`）
翻译为无符号的 `unpack8(uint)`；在 pass 11 里有九种不同形式。
对脚本的通用修正是 `tools/Fsr4SpirvCrossFixes.pm`（所有 `unpack8`
都走有符号辅助函数）。

## 2026-10-01：FSR 4.1.1 —— 内部有什么，以及如何在 Vulkan 中运行（侦察）

来源：OptiScaler（`FSR4_LATEST`）中的 `amd_fidelityfx_upscaler_dx12.dll` 4.1.1.2740。
- DLL 中有 provider `ffxProvider_FSR4_Int8` / `Fsr4Int8UpscalerModel`
  （版本「FSR4-i8 4.1.1」）。1028 个 DXIL 容器（25.7 MB）：模型 pass
  `fsr4_model_v07_fp8_no_scale_pass1..12`（外加 `_post`）、270 个 `prepass` 变体、
  144 个 `postpass`、`fsr_rcas_pass` 以及备用的 FSR 3.1/2。没有压缩数据。
- 尽管名字里有「fp8」，模型 pass 只是普通 DXIL 加 `dot4AddPacked`（int8 dot4），
  不含 AMD 扩展（WMMA）。架构同为 v07（12 个 pass），但权重与偏移不内嵌在着色器中，
  而是从 `InitializerBuffer`（t18，typed buffer）读取，张量尺寸来自 cbuffer
  `CsTensorSizes`，scratch 来自 `u11`。每个 pass 有 9 个变体：网格是固定的
  （1080 / 4K / 8K 级别：pass 5 为 480x270、960x540、1920x1080），以及可能的预设。
- `amdxcffx64.dll`（加载器 2.3.0.2913，Proton 会加载它）—— 是同样的 FP8 FSR 4 pass
  与 ML 帧生成（`mlfi_*`），没有独立的 INT8 模型。`FSR4_INT8` 4.0.2 就是 `v07_i8`，
  也就是移植已经在用的那个。
- dxil-spirv（HansKristian-Work/dxil-spirv，需从源码构建）把 pass 转换为
  正确的 SPIR-V：`dxil-spirv <pass> --enable-shader-i8-dot --ssbo-uav --use-reflection-names`。

自建 4.1.1 provider 所缺的东西：容器到（预设、级别、pass）的对应关系、
`InitializerBuffer` 的内容、`CsTensorSizes` 的值、prepass/postpass 的排列、
常量与 dispatch 尺寸。最可靠的路径不是逆向 provider 的 x86 代码，而是录制一帧：
在 Wine/Proton + vkd3d-proton 下用 FFX API（DX12，MinGW）写一个小型测试程序，
通过版本 override 显式选择「FSR4-i8」provider，并加一个 Vulkan 层来记录管线
（SPIR-V 来自 dxil-spirv）、缓冲内容与 dispatch 尺寸。然后用自己的 provider 通过
同一个基准（`fsr4-bench`）复现，并与录制的输出比对。

## 2026-10-01：移植中的 FSR 4.1.1（`upscaler=fsr411`）

AMD 4.1.1（INT8）模型在 Vulkan 上原生运行，与 DLL 逐位一致。

**如何获得。** `tools/fsr4cap/fsr4cap.exe`（C，MinGW）以显式版本 4.1.1 调用
`amd_fidelityfx_loader_dx12.dll` 中的 FidelityFX API 2.3，并通过替换 D3D12 方法表
记录该 DLL 在 vkd3d-proton 下所做的一切：管线（DXIL）、根签名、描述符、
加载、常量、dispatch。一帧共 29 个 dispatch：SPD 自动曝光、prepass、pass0_post、
模型 pass 1..12（每个之后跟一个 `_post`、重置张量边界）、postpass、RCAS。
- 有两个模型：m0（Native..Performance）与 m1（Ultra Performance，比例 3.0），
  权重为 128 KB（`InitializerBuffer`）。级别：t1080（输出至 1920x1080）、
  t2160（更大，含 2560x1080）。
- 张量尺寸由输出按 8 对齐后得出；`CsTensorSizes` 表（17 个尺寸）、工作组数量与
  常量（MLSR 与 v07 相同，但 width/height 不对齐且 `inv_scale = 1 / scale`；
  SPD；RCAS）由 `extract.py` 在 20 条记录上验证。
- dxil-spirv 转换 DXIL 的方式与 vkd3d-proton 相同：`--mixed-float-dot-product`
  （`VK_VALVE_shader_mixed_float_dot_product`，half 的 dot2 累积到 float）——
  没有它 PSNR 是 40 dB 而非精确一致。`--class-bindings`（补丁）把 SRV/UAV/CBV/
  采样器寄存器按+0/+32/+64/+96 的 bgroup 布局展开，运行时按名字绑定资源。
- `fsr411/fsr411.cpp` —— 独立的 Vulkan 内核（游戏与 `fsr4-bench --fsr411`）。
- `verify.sh`：相同输入下 DLL 与复现版本，8 帧 —— 在 1080p（两个模型）、
  1440p、2560x1080、4K（两个模型）上逐位相同。1600x900 仅在右边缘有差异，
  那里 DLL 本身就是非确定性的（其着色器中在张量宽度非 64 倍数时的竞态）。

**加速。** postpass 在 SPIR-V 汇编层面用 workgroup memory 重写
（`postpass_lds.py`；经GLSL 转换不精确：会丢失 `DenormPreserve` 与
`RelaxedPrecision` 的精确摆放，还有别的东西会改变结果）：4K 下 2.18 -> 0.95 ms，
与 DLL 逐位一致。整个 FSR 4.1.1：1080p（1280x720）时 1.09 ms，4K Balanced 时 4.2 ms。
游戏中（4K Balanced）超分器 4.12 ms，而加速后的 v07 为 4.32 ms。

**资产构建**（由用户用自己的 DLL 构建；仓库中没有任何来自 AMD 的东西）：

    bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x> <amd_fidelityfx_loader_dx12.dll 2.3.x>

需要 nix-shell（或 MinGW、cmake、ninja、python3、SPIRV-Tools、umu-run），
以及用于 dxil-spirv、FidelityFX SDK 与 Proton（GE-Proton）头文件的网络访问。
DLL 例如可从 OptiScaler（`FSR4_LATEST/`）与带 FSR 3.1/4 的游戏中获得。
从头构建会得到相同的文件；末尾的 `VERIFY=1` 会与 DLL 比对。

**对上文 A/B 的修正（「帧受限于 CPU」）。** 那些测量进行时，有 12 个卡住的
shadPS4 进程各占一个核。在空闲的系统上：「host copies」1.6%，等待 GPU 约 41% ——
帧受限于 GPU，因此为 FSR 做 async compute 又有了意义。
