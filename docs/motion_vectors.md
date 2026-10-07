# 运动矢量：2026-09-26 视频之后的改进

原始素材：`video_2026-09-26_04-19-53.mp4`，FSR 3.1 Performance，渲染
960×540 → 输出 1920×1080，以及 `out/dev/t71.log`。本文描述的是工作树的当前
改动，并非对游戏内质量的确认。

针对所有 G-buffer draw 的统一 pass 代价过高：Performance 开启时约 49 FPS，
关闭时 77–78 FPS。现在默认仅对带有姿态/骨骼缓冲的 shader 启用物体矢量
（大小 640–16384 字节，864 字节的公共场景缓冲除外；步长 16 字节）。在一次
采用更窄阈值 1024 字节的试运行中，这将处理的 draw 数量削减到每帧约 140 个，
消除了历史溢出，并带来约 81–87 FPS。其余 draw 保留廉价的相机矢量。
`BB_OBJECT_MOTION=0` 关闭整个功能，`BB_OBJECT_MOTION_ALL=1` 恢复统一 pass
以便 A/B。辅助目标的 alpha 通道保存深度：如果较晚的普通 draw 遮挡了物体，
则不使用其矢量。采样后运动中角色与武器的质量仍有待视觉检查。

## 发现的问题

- 原始日志末尾的 600 帧内：257400 次 G-buffer draw，256200 次被声明为运动，
  仅 30000 次获得了上一帧位置（约占全部 draw 的 ~11.7%）。这是对 draw 的
  覆盖率，不是像素百分比。旧计数器不区分每次跳过的原因。
- 为历史预留的是整个 vertex buffer 的大小乘以实例数，且连同 firstInstance
  一起计算。对公共缓冲的子网格而言这是多余的位置；预算为每帧 2097152 个
  位置。
- 历史在 vertex shader 的写入/读取之间没有单独的 barrier；host 参数环的
  复用不等待 GPU。
- 被禁用的所有顶点写入都进入同一个 scratch 元素。
- `CameraMotion::OnDisplayPass` 在计算 jitter phases 之前就重置了 Depth()：
  Performance 只得到 8 个相位而非 32 个。从深度恢复位置时也未考虑 viewport
  偏移。

## 改动

- 历史范围按实际索引的 min/max 确定，并考虑 baseVertex 和 primitive
  restart。FirstInstance 在 shader 中减去，而不消耗内存。
- 自首次出现起保存所有合适的直接 G-buffer draw。已移除常数变化启发式：它
  几乎把所有 draw 都当作运动的，并错过运动的开始。只有与上一帧匹配时才可
  读取。
- 键包含全部 vertex stream 描述、shader、索引内容的地址与哈希、顶点范围、
  实例数以及在同型 draw 中的序号。拓扑变化或跳帧会重置相应历史。
- 预算为每帧 4194304 个位置；两个数组合计占用 128 MiB 而非 64 MiB。内存
  不足时退回相机矢量。日志分别显示保存、匹配、无历史、内存不足和非法范围。
- 新增了 vertex-to-vertex barrier、复用 host 参数前等待 GPU、coherent host
  memory，以及对运动图像 layout 的独立跟踪。重复读取图像不会清除存在矢量
  的标志。
- 被禁用的访问通过分支绕过。对同一索引化顶点的多次 invocation 使用位置
  分量的原子写入。相机背后的位置以及非数值/无穷的矢量不予使用。
- Performance 在创建 FSR context 后获得 32 个 jitter 相位。相机矢量从
  unjittered 坐标恢复；物体矢量本就从 viewport 偏移前的 clip positions
  取得。

## 校验

构建与 CPU 测试：

```bash
cd native_probe
nix-shell shell.nix --run 'bash build.sh && ninja -C out/gpu motion-history-test motion-shader-test && out/gpu/motion-history-test && out/gpu/motion-shader-test out/motion-shaders'
```

`test_motion_history.cpp` 校验 u16/u32 范围、restart、负 baseVertex、
firstInstance、重复 draw、拓扑变化、跳帧、预算耗尽、溢出和相位数。也通过了
AddressSanitizer/UBSan 下的运行。

`test_motion_shaders.cpp` 使用真实的 SPIR-V backend，生成带矢量与不带矢量
的 VS/PS。四个结果全部通过 `spirv-val --target-env vulkan1.3`。要复现需要
SPIRV-Tools 中的 `spirv-val`：

```bash
for shader in out/motion-shaders/*.spv; do
    spirv-val --target-env vulkan1.3 "$shader" || exit
done
```

构建/测试日志：`out/dev/motion-validation.log`。校验环境中没有 `/dev/dri`
访问权限：改动之后在 RX 7800 XT 上的质量、FPS 和覆盖率百分比尚未检验。
SPIR-V 校验通过并不证明画面正确。

## 下一次游戏内运行

在 `game_files` 下：

```bash
BB_FRAME_STATS=1 BB_TOGGLE_FILE=out/dev/motion-toggles bash native_probe/run.sh 2>&1 | tee native_probe/out/dev/motion-run.log
```

临时 toggle 文件中的位 29 只关闭历史写入。要做正确的性能比较，需要分别以
`BB_OBJECT_MOTION=0` 和不带它启动游戏：管线选择和附加目标在进程启动时确定。
切换之后给 FSR 几帧时间更新历史。比较同一场景：奔跑时的角色/武器、相机
转动、战斗、静止相机。约 600 帧后检查 `Object motion:`：`with history`、
`capacity skips`、`unmatched`。

剩余限制：相同网格按 draw 顺序匹配，而该顺序可能随物体剔除而变化；动态
vertex buffer 地址变化会丢失匹配。Indirect draw、tessellation/geometry
shader、MSAA 以及 G-buffer 之外的 pass 仍使用相机矢量。透明物/粒子、发糊的
UI 以及把缩放移到渲染器层面的问题未被本次改动解决。索引扫描和原子写入的
开销需要在真实运行中测量。

## 9 月 29 日：武器与小型骨骼（旋转锯盘的 ghosting）

视频 `video_2026-09-29_23-08-26.mp4`：角色静止，锯盘旋转并留下拖尾。
`BB_MOTION_SELECT_LOG=1` 的日志给出了原因：vertex shader 的第二个缓冲是每块
48 字节（3x4）的骨骼调色板，武器/道具上为 96–384 字节（2–8 根骨骼）。
“从 640 字节起”的规则未把这些 draw 包含进来，锯盘只得到相机矢量，也就是说
对 FSR 而言它是静止的。

改动：

- `Motion::ClassifyBuffer`（`motion_history.h`）：864 —— 场景，≥640 ——
  角色骨骼，96..639 且为 48 的倍数 —— 小型骨骼。带有任意骨骼的管线都会
  获得带矢量的变体。
- 每帧约有上千个小型骨骼，大多数是地图的静态部分。因此它们会被“gating”：
  在扫描索引之前，先用廉价的键（`History::Moving`）把调色板哈希与上一帧
  比较。匹配则相机矢量是准确的，不写入任何内容。变化则保存位置，从下一帧
  起使用物体矢量。缓冲 416 字节的哈希不适用：它对几乎所有物体都随相机一起
  变化（已验证：历史溢出且 FPS 跌至 ~40）。
- 菜单：“显示运动矢量（调试）”。红/绿对应 |x|/|y|，蓝表示该像素获得了
  物体矢量。`BB_MOTION_SELECT_LOG=1` 会打印 G-buffer shader 及其缓冲大小
  和判定结果。

测量 `out/motion-gated.log`：每帧约 225 个保存的 draw 和约 600 个“静止”
draw，无溢出，游戏场景中 58–80 FPS。锯盘上拖尾消失在视觉上仍有待确认。
