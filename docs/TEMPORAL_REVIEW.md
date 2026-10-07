# TAA 与时域超分器输入的校验 — 2026-10-01

## 项目状态

bbport 是一个专用 Linux 运行时，面向原始 x86-64 代码的 Bloodborne
CUSA03173 1.09，基于 shadPS4 将 GPU 命令转译为 Vulkan。这是一个实验性移植：
加载器、系统调用 HLE、声音、输入、存档、游戏内设置界面以及 temporal
upscaling 均已实现。该工作副本中包含源码、已编译的库、游戏转储和存档。
项目没有 Git 元数据，因此无法核对变更历史和工作树的干净程度。

本次校验不证实完整通过，也不证实向其他驱动和 Steam Deck 的可移植性。文档中
仍保留的限制：人为的 occlusion query、不完整的 predication、没有自有运动
矢量的透明特效，以及 FSR 4.1.1 模型对某些非标准输出尺寸的竞态。

## 视频与原因

原始录像 `video_2026-10-01_21-21-02.mp4`：94.89 s，2560×1080，60 FPS。
其中依次切换 TAA、FSR 4.1.1、FSR 4 和 FSR 3.1。在建筑、细轮廓和运动中的
武器上可见时域不稳定。菜单中开启了游戏内运动模糊；不能把它的贡献归到 TAA。
文件 `video_fixed_taa.mp4` 时长相同，但帧尺寸不同（2560×1082）；该录像本身
并不能证明渲染器已修复。供查看的帧和序列位于 `out/dev/temporal-review/`。

独立于对视频的主观评价，已在代码中查明以下错误：

1. **TAA history 中的 FP16 depth。** half 1 附近的步长约为 0.000488。
   在 near ≈ 0.05 时，远处的表面变得无法区分。历史校验阈值曾小于该步长，
   因此历史既可能被错误丢弃，也可能与另一表面混合。历史现在为 RGBA32F。
2. **不可比的相机。** 旧校验将上一帧的深度与当前帧的深度直接比较。现在把
   当前点变换到上一帧相机的坐标系中，并与该处的期望深度比较。容差按透视
   深度缩放。
3. **远处矢量被置零。** `depth >= 0.99999` 曾被视为天空并得到零运动。对于
   near ≈ 0.05、far 3000 的投影，该阈值影响大约从 1875 个单位开始的几何体。
   现在只有 clear depth 1 被视为无限远背景；背景获得相机的旋转运动而无平移。
   远处几何体则得到正常的重投影。
4. **object motion 中的 FP16 depth。** RGBA16F 会丢失用于校验矢量是否被
   更晚 draw 遮挡的深度。图形管线的缓冲与格式现在为 RGBA32F；深度校验不再
   以宽泛的固定容差接受另一远表面。供 FSR 使用的最终矢量仍为 RG16F。
5. **边缘处混合深度的校验。** TAA 曾在不同表面之间插值历史深度。现在四个
   bilinear tap 被独立校验，颜色仅由相互一致的 tap 收集；部分覆盖时降低
   历史权重。
6. **远处累积过弱。** 先前 depth-adaptive 的权重降低（至 0.8/0.5）已替换为
   历史有效时按运动量取 0.98…0.85 的权重（下限权重在 8 px/frame 时达到）。
   权重 0.94 的诊断运行显示，即使历史被接受，各 jitter 相位之间仍有残余的
   颜色波动。历史的 RGB 被限制在当前帧邻域的范围内。
7. **斜坡与细小细节上的深度 jitter。** 现在深度按局部单向梯度搬运到稳定的
   像素位置，并带有防止表面断裂的保护。motion/depth 在 3×3 内选择最近的
   表面；颜色和 clipping 范围围绕原始颜色像素取用。这使细轮廓能在 jitter 下
   foreground/background 交替时保留历史。
8. **近乎零运动下的历史寻址不稳定 —— 所发现 TAA 抖动的主要原因。** 以前
   bilinear 坐标的整数与小数部分是在把很大的像素编号与极小的 motion 相加
   之后计算的。在 GPU 上这会产生不一致的两部分并选中相邻 texel。现在先对
   motion 本身做分离：`base = p + ivec2(floor(motion))`、
   `fraction = motion - floor(motion)`。在像素 x=2010、运动约 −0.00003 px 的
   回归校验中复现了旧公式的错误：颜色为 0.250000 而非期望的 0.739985。
   修正后的公式通过校验。

修复 3–4 涉及 TAA/FSR 3.1/FSR 4/FSR 4.1.1 的公共输入。AMD 的算法与 ML 资产
未作更改。shader/pipeline 的 ABI 校验和 object motion 的诊断 dump 已随资源
格式一并更新。

## 校验与限制

通过 `shell.nix` 构建 `bbgpu`、`taa-shader-test`、`camera-motion-test`。
在 Vulkan GPU 上，production TAA shader 的 18 个用例全部通过：带 NaN 的
reset、unjitter、累积、clipping、表面显露、出画、NaN history、精确的远深度、
拒绝另一远表面、相机平移、历史部分有效的边缘、天空及天空/几何边界、两种
jitter 符号下的斜面深度、细轮廓、运动时降低 persistence，以及大坐标上极小的
负运动。production camera-motion shader 的 6 项校验通过：远处几何体、天空
旋转、天空无平移、jitter 排除、有效与过期的 object vector。日志：
`out/dev/temporal-review/tests.log`。附加的 unjitter 校验：
`out/dev/temporal-review/taa-final-tests.log`。旧公式回归的复现以及新公式的
18 项成功校验：`out/dev/temporal-review/regression-proof.log`。最终的游戏内
校验：`out/dev/temporal-review/subpixel-validation.log`。全部 64 个既有
Python 测试通过（`out/dev/temporal-review/python-tests.log`）。

Live smoke：存档副本被载入猎人的梦境。在同一进程内校验了
TAA → Off → FSR 3.1 → FSR 4 v07 → FSR 4.1.1 → TAA，输出 2560×1440，每个
模式六帧最终画面，切换之间有相机运动。FSR 以 1706×960 输入运行；TAA 以
原生 2560×1440 运行。AMD context 已由日志确认，未出现替换为 FSR 3.1 的情况。
进程正常退出；pipeline 编译完成后能维持 60 FPS 上限。日志与 raw 帧：
`out/dev/temporal-review/subpixel-validation.log`、
`out/dev/temporal-review/far-refined/`。测试配置中关闭了游戏内运动模糊和 AA。

在远处塔楼区段（x=2010…2099，y=400…779）相机静止时，TAA 六帧之间的 RGB
平均绝对变化按 0…255 量程从 1.892 降至 0.299，约 6.3 倍。切回 TAA 后为
0.279。最终运行中 FSR 3.1：0.328，FSR 4：0.278，FSR 4.1.1：0.364。这是所选
场景的结果，而非普遍的质量评价：FSR 的输入分辨率与 TAA 不同，且此处的
before/after 比较针对的正是最后一次历史寻址修改。数据：
`out/dev/temporal-review/far-refined/stability.json`；上一次运行：
`out/dev/temporal-review/final-validation.log`。

诊断方面新增 `BB_TAA_DIAGNOSTICS=1`：TAA 输出的 alpha 包含被接受历史的权重
而非 1。可在 `BB_DUMP_TRIGGER` 的 raw RGBA16F 中读取它。RGB 不变，历史的
alpha 仍包含 depth。常规模式保持不透明输出。附加模式 2 和 3 输出当前颜色或
被限制的历史；历史诊断图像可用于比较直接读取与重投影。这些比较即使在被接受
的历史和近乎零 motion 下也揭示出了寻址错误。

源码与 `out/gpu/libbbgpu.so` 已更新。重新构建了
`dist/Bloodborne-bbport-x86_64.AppImage`（832 MiB）；批处理的 `--vulkan-info`
通过 RADV 成功识别出 RX 7800 XT。日志：`out/dev/temporal-review/` 下的
`appimage-build.log` 和 `appimage-vulkan.log`。上述完整游戏运行使用的是本地
库；未单独执行 AppImage 的完整游戏运行。

这些测试确认了具体修复，但并不能证明游戏内所有闪烁均已消失。基于顶点历史的
物体运动、透明物、动画材质、LOD 选择以及大幅降分辨率下的亚像素几何，仍需在
单独场景中检验。

全精度的代价：4K 下两份 TAA 历史占用 253 MiB 而非 127 MiB；object motion 为
127 MiB 而非 4K render 下的 63 MiB。这也会提高内存带宽消耗。下一步可能的优化
是在保持颜色和 motion 为 FP16 的同时使用单独的 R32F depth，并以同样的正确性
测试验证。

回归测试：

```sh
nix-shell shell.nix --run 'ninja -C out/gpu taa-shader-test camera-motion-test && out/gpu/taa-shader-test && out/gpu/camera-motion-test'
```

为所有输入像素提供运动的方案与
[AMD FSR 3.1 integration](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)
的建议一致。
