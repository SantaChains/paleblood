# 深度自适应 TAA 改进

**日期：** 2026-10-01
**状态：** 历史实验，已被时间重投影修复取代。

下述阈值与权重已不再使用。远处降低历史权重会加剧闪烁；跨不同相机比较 device depth、以 FP16 存储深度，都无法正确区分远距离表面。当前的完整分析、改动与验证结果见 [TEMPORAL_REVIEW_2026-10-01.md](TEMPORAL_REVIEW_2026-10-01.md)。

## 问题

当时的 TAA 实现使用固定阈值：
1. 判定 disocclusion（新表面露出）：`0.0005`
2. 时间积累权重：随运动在 `0.9` → `0.65` 之间取值

这些参数不考虑像素深度，导致：
- **远距物体（depth → 1.0）**：时间不稳定，闪烁
- **近距物体（depth → 0.0）**：快速运动时过度模糊

## 方案：按深度自适应的阈值

### 1. 深度自适应的 Disocclusion 阈值

**文件：** `gpu/shadps4/video_core/host_shaders/taa.comp`

```glsl
float depthThreshold(float depth) {
    // Near: 0.001, Far (>0.95): 0.0002
    return mix(0.001, 0.0002, smoothstep(0.7, 0.95, depth));
}
```

**逻辑：**
- 近处表面（depth < 0.7）：阈值 `0.001`——允许少量偏差
- 远处表面（depth > 0.95）：阈值 `0.0002`——更严格，减少误判
- 经 `smoothstep(0.7, 0.95, depth)` 平滑过渡

**应用：**
```glsl
float depthDiff = abs(history.a - depth);
float threshold = depthThreshold(depth);
if (depthDiff < threshold && ...) {
    // 接受历史
}
```

### 2. 深度自适应的时间权重

```glsl
float temporalWeight(float depth, float motionLength) {
    // Base weight decreases for distant objects: 0.9 near, 0.8 far
    float baseWeight = mix(0.9, 0.8, smoothstep(0.7, 0.95, depth));
    // Motion reduces weight more aggressively for distant pixels
    float motionFactor = mix(0.65, 0.5, smoothstep(0.7, 0.95, depth));
    return mix(baseWeight, motionFactor, clamp(motionLength / 16.0, 0.0, 1.0));
}
```

**逻辑：**
- **无运动时的基础权重：**
  - 近处：`0.9`（积极积累历史）
  - 远处：`0.8`（更保守，减少闪烁）

- **快速运动时（motionLength > 16px）：**
  - 近处：降到 `0.65`
  - 远处：降到 `0.5`（更保守）

**应用：**
```glsl
float motionLength = length(motion);
float weight = temporalWeight(depth, motionLength);
result = mix(current, clipped, weight);
```

### 3. 运动矢量校验（camera_motion.comp）

**文件：** `gpu/shadps4/video_core/host_shaders/camera_motion.comp`

```glsl
// Depth-adaptive validation: distant surfaces need tighter depth matching.
float depth_threshold = mix(0.001, 0.0003, smoothstep(0.7, 0.95, depth));
if (object_motion.b > 0.99 && abs(object_motion.a - depth) < depth_threshold &&
    !any(isnan(object_motion.rg)) && !any(isinf(object_motion.rg))) {
    result = object_motion.rg;
}
```

**改动内容：**
- 旧阈值：固定 `0.001`
- 新阈值：近处 `0.001`，远处表面收至 `0.0003`
- 防止采用来自错误像素的 object motion vectors

### 4. debug 模式可视化改进

**文件：** `gpu/shadps4/video_core/host_shaders/camera_motion.comp`

`BB_DEBUG_MOTION=1` 的模式已结构化：
```glsl
if (mode == 1) {
    // Raw depth visualization
    imageStore(color_img, pixel, vec4(fract(depth * 4.0), depth, depth < 0.0 ? 1.0 : 0.0, 1.0));
} else if (mode == 2) {
    // View-space depth visualization (z / 50)
    const float v = z / 50.0;
    imageStore(color_img, pixel, vec4(fract(v * 4.0), v, v < 0.0 ? 1.0 : 0.0, 1.0));
}
```

## 预期改进

### 远距物体
- ✅ 远处墙面与天空的闪烁减少
- ✅ 小幅相机运动下历史更稳定
- ✅ 深度数值误差造成的假 disocclusion 减少

### 近距物体
- ✅ 快速运动（角色、武器）时模糊减少
- ✅ object motion vectors 更准确
- ✅ 积累与响应性之间平衡更好

### 自适应区（depth 0.7-0.95）
- 两种模式间平滑过渡
- 避免边界处出现突兀伪影

## 测试

### 目视检查
```bash
# TAA 原生分辨率
BB_UPSCALER=taa bash run.sh

# 检查：
# 1. 远处墙面的闪烁（Hunter's Dream、高层楼层）
# 2. 快速转身时武器的模糊
# 3. 缓慢移动相机时的稳定性
# 4. 绕过墙角时的 disocclusion
```

### debug 模式
```bash
# 深度可视化
BB_DEBUG_MOTION=1 bash run.sh
# Toggle: (1<<21) = raw depth, (1<<22) = view-space z

# 运动矢量检查
# Toggle: (1<<20) = blended motion, (1<<23) = reprojection, (1<<24) = difference
```

### A/B 对比
```bash
# 建一个运行时切换用的文件
echo "0" > /tmp/bb_toggle

# 游戏内切换位值做对比
# 需要一个临时机制来切换旧/新权重
```

## 潜在风险

1. **远距物体权重过于保守**
   - 可能加剧快速转身时的拖影
   - 对策：调整 `baseWeight`（0.8 → 0.85？）

2. **object motion 的深度阈值过严**
   - 可能把有效矢量一并丢弃
   - 对策：depth > 0.95 时 `0.0003` → `0.0005`

3. **smoothstep 区间 0.7-0.95 可能不是最优**
   - Bloodborne：near=0.05, far=3000
   - 大部分几何位于 0.1-0.9 区间
   - 对策：在典型场景剖析深度分布

## 后续步骤

1. ✅ 实现深度自适应阈值
2. ⏳ 构建并进游戏测试
3. ⏳ 收集深度分布统计
4. ⏳ 依据真实数据调整阈值
5. ⏳ 增加运行时开关以便 A/B 测试
6. ⏳ 记录最终参数

## 相关链接

- [upscaler.md](upscaler.md) — 超分器总体架构
- [motion_vectors.md](motion_vectors.md) — 运动矢量
- TAA shader: `gpu/shadps4/video_core/host_shaders/taa.comp`
- Camera motion: `gpu/shadps4/video_core/host_shaders/camera_motion.comp`
