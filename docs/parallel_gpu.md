# 并行 GPU 命令处理 — 设计笔记

目标：消除模拟 GPU 命令处理器（`shadPS4:GpuCommandProcessor`）的单核瓶颈
（它占满一个核约 100%，而 CPU 其余部分闲置）。

## 测量（Bloodborne，猎人噩梦，约 70 FPS，`BB_DCB_STATS=1`）

| | |
|---|---|
| 提交的图形命令缓冲 | 4200–5800/s，**每帧约 65–80 个** |
| draw | 约 110 000/s，每帧约 1600 个 |
| 每个命令缓冲的 draw 数 | 多数10–99，最多约 200 |
|嵌套 IndirectBuffer | 无 |
| 首个 draw 之前的状态 | 写入约 118 个 context + 约 31 个 SH 寄存器 dword；仅 8% 以 ClearState 开始 |

单线程工作做完后的 GPU 线程剖析（perf，`cpu-clock:u`）：
纹理绑定约 25%，管线选择约 19%（StageSpecialization 构建/比较、
sharp fetch），缓冲绑定约 14%，渲染目标约 8%，PM4 解码约 3%。

## 约束

- 命令缓冲不是自包含的：寄存器状态是继承的，因此 worker 需要其缓冲起始处的
  寄存器状态（context + SH 范围约 6–8 KiB；70 FPS 下每缓冲一份快照约 40 MB/s）。
- draw 准备读取的不只是寄存器，还有客户内存：扩展用户数据（EUD）与 shader 代码。
  流中更早的包（DMA、WriteData、常量引擎 dump）可能改动那段内存，因此抢在 GPU
  线程之前完成的工作可能看到过期数据。
- 各缓存（buffer、texture、pipeline）与每 draw 的暂存状态（`Image` 里的标志位、
  共享 `Shader::Info` 内的用户数据）设计上就是单线程的。
- 图像布局与 barrier 跟踪假定只有一条有序流。

## 方案

1. **在worker 上做推测性 draw 准备，在 GPU 线程上验证。**
   一趟廉价串行处理记录每个命令缓冲起始处的寄存器状态。worker 重放自己缓冲的
   寄存器写入，并逐 draw 预计算纯函数部分：管线键与 stage 特化输入、sharp fetch、
   纹理与渲染目标描述、动态状态值、顶点缓冲范围。每个结果都带上它据以计算的
   扁平化用户数据。GPU 线程仍然解码流并刷新扁平化用户数据（廉价），与worker
   的副本比对，仅在匹配时使用预备好的 draw；否则照今天的方式自行计算。
   正确性从不依赖 worker。
   前置重构：sharp 消费方改为通过视图读取用户数据，而非共享的 `Shader::Info`
   成员；每 draw 暂存移入一个上下文结构。
2. **线程安全的缓存查找。** 查找已有 buffer/image/view 移到 worker；创建、上传、
   barrier 仍留在 GPU 线程。
3. **并行 Vulkan 录制。** 每个 worker 录制自己的命令缓冲；它们按提交顺序执行，
   接缝处放barrier。

每一步都保留一个 `BB_TOGGLE_FILE` 位，以便运行时关闭，并通过截图与帧率对比。

## 可移植性

必须能降级到 Steam Deck（4 核 / 8 线程）：worker 数跟随
`hardware_concurrency()`，核少时不忙等，不用 AVX-512。

## 结果

第1 步（draw 准备，4 个 worker，toggle 8192），猎人噩梦，同一视角：
启用预备 draw 为 71.5 FPS，禁用为 64.1 FPS（+11.5%），截图完全一致。
97–98% 的直接 draw 使用预备管线；每个 `bb:DrawPrep` worker 约占一个核的 10%。
GPU 线程仍约 90% 忙碌：纹理/缓冲绑定是下一个目标（第 2 步）。

客户写故障（同一视角，toggle 65536）：游戏顺序填充其每帧缓冲，每 4 KiB 页都
产生一次保护故障 —— 约 115k 故障/s，每个 GXWorker 与主线程约 20% 的时间耗在内核里。
把故障点周围对齐的 64 KiB 窗口解除保护后：16k 故障/s，内核时间约 8%，
**81.0 FPS 对 66.4 FPS**（+22%），画面完全一致。
GPU 命令线程回到约 100%：它再次成为瓶颈。

已否决：「热页」（对反复写入的页永不重新保护，每次绑定时上传）—— 该集合增长到
约 14k 页，重传把帧率压到 33 FPS，随后 GPU 环形缓冲超时。
保留在 `BB_HOT_PAGES=1` 后面作为可选项。

纹理描述缓存 2 路/4096 项、同目标快路径、LRU触碰跳过、无每纹理元数据查找：
另一处室外视角 93.9 FPS；关掉纹理 memo（mask 1056）为 65.6 FPS。
已接近 100 Hz 显示上限（受 vblank 节拍），因此进一步收益需要不限帧的测试。

## 流式加载卡顿（`BB_FRAME_STATS` 的「Stall:」行）

跑过新区域时出现 60–170 ms 的帧（shadPS4 中同样有）。GPU 线程整帧忙碌，
大部分在内核里，每帧上传 50–400 MB 的纹理与缓冲。按修复顺序列出发现：

1. 客户到staging 的拷贝原先跑在一个线程上（录制线程跑纹理，GPU 线程跑其余）。
   现在它们同时在拷贝线程上启动（`BbCopy::Async`，`bbport_copy.cpp`）；
   小拷贝按线程批量合并（每约 512 KiB 唤醒一次 —— 每次拷贝唤醒一次要付 25% FPS）。
   客户可见的fence 与队列提交会等它们（`Scheduler::WaitHostCopies`），
   这保留了对 UI 闪烁的修复（客户在延迟拷贝执行前就复用了缓冲）。
2. 拷贝随后每线程只跑到 0.2–0.4 GB/s，几乎全耗在内核：CPU 首次访问新的
   staging 块会让内核分配并清零它（每 16 MiB 约 2 ms），而 staging 池会在
   空闲 3 s 后释放块，正好卡在流式突发之间。现在保留 512 MiB
   （`BB_STAGING_KEEP_MB`），其余在 30 s 后释放，启动时预填 128 MiB。
3. 写故障：256 KiB 的解除保护窗口（`BB_FAULT_WINDOW`）再次把它们减半。
   `BB_UFFD=1` 用userfaultfd 写保护而非 mprotect 跟踪写入（无需地址空间写锁、
   无映射分裂）；回读用的读保护仍用 mprotect。它省掉了 mprotect 时间，
   但未可测量地改变卡顿；可选项。
4. 读文件到受写保护的客户页曾以 EFAULT 失败（内核拷贝不会进入缺页处理）；
   现在读取会先触碰每个目标页。

结果：卡顿大多降到 40–50 ms（GPU 线程约 30 ms draw 工作加约 12 ms 拷贝），
不再是 60–170 ms；区域加载那一帧 350 ms，而非 430–760 ms。

## 第 2 步第一刀：worker 上的资源 sharp（2026-09-30）

预备好的 stage 现在也携带其 worker 从扁平化用户数据读到的 sharp：
每个 T# 及其纹理描述哈希、每个 S# 与 V#（`PrepareResources`、
`PreparedStage::image_sharps` 及其同类，位于该次提交的 arena 中）。当
`PipelineCache::UsedPrepared()` 报告该 draw 的管线来自预备 draw 时，GPU 线程
就使用它们 —— 此时扁平化数据已逐字比对过，而 sharp 不依赖其他任何东西。
资源列表大小按 stage 检查。toggle 4096 可关闭。

一次运行内 A/B，静止，`BB_FPS_LIMIT=0`，FSR 4，各阶段 20 s：
启用预备 sharp 为 69–70 FPS，禁用为 66.5–67 FPS（+3%），画面相同。

在此之前已从 GPU 线程移除的超分开销（perf，DWARF 调用图）：`SceneTargets::Eligible`
中逐 draw 的驱动格式查询（约 13%）、物体运动与反应性 mask 中 `CommandBuffer()`
强制直接录制、物体运动的索引列表扫描（约 10%，现为 `Motion::IndexRangeCache`）。

GPU 线程上剩下的主要是共享缓存状态上的工作：纹理绑定约 23%
（FindView 约 5.6%、UpdateImage/Track/Touch 约 5%、barrier），缓冲绑定约 12%
（ObtainBuffer 约 11%），渲染目标约 8%。下一步是 draw 级拆分：GPU 线程保留解码、
缓存变更与 barrier；第二个有序 stage 从已解析出的 handle 构建描述符写入与顶点输入状态。

## 扩展到可用线程数（2026-09-30）

draw 准备原先在每个 worker 上重放整条命令流，并在硬件线程数低于 12 时被关闭
（Steam Deck：无 worker）。现在：

- 一个扫描器（`bb:DrawScan`）按序重放寄存器写入，并为每个缓冲记录其起始校验和
  以及它写入的 32 字寄存器块的增量（`AmdGpu::RegDirty/RegDelta`，由
  `ApplyGraphicsRegisterPacket` 记录）。
- worker（`bb:DrawPrepN`，亲和性掩码中一半的硬件线程，1..8）抢在 GPU 线程之前
  认领最近的已扫描缓冲，通过应用自上一个缓冲以来的增量（或从队列尾部状态起）
  到达其起始状态，然后准备其中的 draw。worker 变多意味着并行准备的缓冲变多，
  而非重放次数变多。
- 所有辅助线程都是 SCHED_IDLE（`bbport_threads.h`）：只占用空闲核。若扫描器
  饥饿（CPU 繁忙），GPU 线程在滞后超过 64 个缓冲时从自己的寄存器状态重新定基，
  而不是无界排队（统计中的 `scanner rebases`）。
- 拷贝线程使用同样的亲和线程数（关键路径，普通优先级）。

一次运行内 A/B（toggle 8192，静止，`BB_FPS_LIMIT=0`，FSR 4），约 98% 的直接
draw 已预备，无 rebase：

| CPU | 启用准备 | 禁用 |
|---|---|---|
| 16 线程，8 worker | 约 74 FPS | 约 57.7 FPS |
| `taskset -c 0-3,8-11`（4 核 / 8 线程，类 Deck），4 worker | 约 68 FPS | 约 53 FPS |

旧设计（4 个重放型 worker）在 16 线程的同一位置给约 69 FPS，在 8 线程上完全没有收益。

## worker 上的顶点输入（2026-09-30）

`PreparedDraw::vertex` 为动态顶点输入路径保存：属性与绑定描述
（`GetVertexInputs`）、每个流的 V#、合并为各流 range index 范围的流内存，
以及各流的 XXH3（物体运动）。GPU 线程只获取合并范围的缓冲并记录绑定。
当预备 draw 的管线被采用、属性数与管线的 fetch shader 匹配、且未运行帧捕获时
使用；toggle 4096 与 sharp 一同关闭它。

A/B（静止，`BB_FPS_LIMIT=0`，FSR 4，16 线程）：启用约 69.3 FPS，禁用约 65.7 FPS
（+5.5%；仅 sharp 已贡献 +3%）。截图一致。

渲染目标经考察后留在 GPU 线程：它们的描述已按 slot 做了memo（键拷贝 + 比较），
尺寸提示（`last_cb_extent`）是 GPU 线程状态而非寄存器状态，而 `BeginRendering`
的其余部分都是视图查找、barrier 与降分辨率代理 —— 全在共享可变状态上。

## 引擎短路径：调查（2026-09-30，进行中）

- eboot 无符号，但链接了 Sony 的 Gnmx（`sdk\target\src\gnmx\gfxcontext.cpp`、
  `lwgfxcontext.cpp`）；FromSoftware 的 Dantelion2 CoreGraphics2 架在其上；
  YEBIS 负责后处理。
- `BB_BUFFER_STATS=1`（buffer_cache.cpp）每 5 s 按客户区域打印缓冲绑定。
  几乎全部流量来自引擎的帧环形缓冲，位于 0x1042c00000 处 2.4 GB 直接分配内部的
  约 0x1043400000–0x1049xxxxxx：约 400k 次小常量拷贝/s（约 190 MB/s），
  以及 CPU 写入后约 850 MB/s 的 arena 重传（每帧约 13 MB）。
- 候选短路径：把该环形缓冲作为 Vulkan 外部内存导入（`VK_EXT_external_memory_host`），
  让 GPU 就地读取 —— 无拷贝、无页跟踪、GPU 线程操作大幅减少。
  需先解决的阻塞点：EOP fence 是在 GPU 线程录制包时置位的，而非 GPU 执行时，
  因此客户可能改写GPU 尚未读取的环形数据。环形约 100 MB、每帧约 13 MB，
  回绕约 7–8 帧；GPU 滞后由presenter 的帧池（`present_frames`、交换链图像数）
  限定。下一步：测量环形的每帧回绕周期与真实 GPU 滞后，然后在 toggle 后做原型。
- 
### 帧数据窗口的测量（`BB_BUFFER_STATS=1`）

- 0x104xxxxxxx 中 64 KiB 块的重用距离：绝大多数为 1 帧（偶尔 2 帧）。引擎每帧
  重写同一段内存；不存在长环形。
- GPU 线程开始一帧时的 GPU 滞后（`GPU lag at frame start`）：几乎总是 0 ——
  GPU 已完成上一帧。但引擎在 GPU 仍在执行当前帧时写入下一帧的数据，而 EOP fence
  是在录制时置位的。**因此就地读取这段内存（`VK_EXT_external_memory_host`）是不安全的**，
  除非 fence 等待真实的 GPU 完成，而这会让客户付出代价（它要等这些 fence）。已放弃。
- 热窗口内每秒：约 370k 次小常量拷贝（约 179 MB，每个约 480 B）与约 3.6k 次
  arena 绑定，覆盖约 13.8 GB（每次绑定约 3.8 MB），其中仅约 516 MB 被重传。
  拷贝绑定范围而非跟踪页会把流量放大约 27 倍（这正是「热页」尝试掉到 33 FPS 的原因）；
  在那里页跟踪才是正确机制。
- 剩余候选：常量区按次提交做快照（用更少更大的拷贝替代每帧约 6000 次），
  以及把顶点缓冲绑定裁到某个 draw 实际使用的索引范围（V# 覆盖整个顶点池，
  所以每次绑定要遍历约 950 个被跟踪的页）。

### 试过又回退：按 epoch 的常量块

小型只读常量由每块、每队列任务 epoch 一份快照提供（epoch 在每次任务进入与恢复时
都变，因此一个块只能服务其拷贝之前写入的数据）。一次运行内 A/B，静止：
32 KiB 块为 82/82/79 FPS，禁用为 85/84/84（更慢：多拷贝的字节数代价超过省下的操作）；
8 KiB 块为 85/85/85 对 85/83/85（无差异）。逐绑定的常量拷贝不是限制帧率的因素；
该改动已移除。`BB_BUFFER_STATS=1`（每区域绑定数、重用距离、GPU 滞后）保留。

做完这部分后的状态，静止，`BB_FPS_LIMIT=0`，FSR 4：82–85 FPS；GPU 命令线程约占
一个核的 90%，录制线程约 92%（主要是自旋），GPU 约 70% 忙碌。

### 纹理绑定：重复集合与 UpdateImage 快路径

- 测过并放弃：只有约 13% 的 draw 在所有 stage 上都精确绑定与上一次 draw 相同的
  纹理与采样器（跑过亚哈兰），因此跳过整组纹理最多省 GPU 线程约 3%。
- `TextureCache::UpdateImage` 对每次纹理绑定都会运行；对于本 GC 周期内已被跟踪
  且已触碰过的干净已注册图像，它只取纹理缓存互斥锁（与客户线程的缺页处理共享）。
  现在该情况下无锁返回，以原子方式读标志位（toggle 1073741824 = 1 << 30 恢复
  加锁路径）。A/B，8 个阶段各 20 s：均值 81.8 对 80.2 FPS（+2%），4 对中 3 对领先；
  运行期间游戏窗口被其他应用部分遮挡，故仅作参考。
- A/B 脚本现在会在每次阶段切换时记录日志行，并打印每阶段均值。

## LTO 与 PGO（2026-09-30）

`build.sh` 构建 `libbbgpu` 时启用 LTO（`-flto=auto`，对链接进去的 sirit 与
FSR-Vulkan 同样生效），并在 `pgo/` 下有profile 时启用 `-fprofile-use`
（`-fprofile-partial-training -fprofile-correction`；用于未加这两个选项编译后
又改过的函数）。不用 `-march`：同一份构建要能在 Steam Deck 上跑。

采集 profile：`BB_PGO=generate bash run.sh` 构建带仪表的库
（`-fprofile-generate -fprofile-update=atomic`），每 30 s 写一次 `pgo/`
（`bb:pgo` 线程：`__gcov_dump` + `__gcov_reset`，因为游戏常经`_exit` 结束）；
正常玩几分钟。下一次普通构建就会用它。`BB_PGO=off` / `BB_LTO=OFF` 可禁用。
代码有较大改动后重新生成 profile。

对比，游戏内三次运行（取游戏内 5 s 窗口的中位数，>800 draw/帧），用新增的
`Frame stats` 字段「GPU thread us/draw」（GPU 命令线程每 draw 的 CPU 时间，
比 FPS 更能容忍小的场景差异）：

| 构建 | FPS | GPU 线程 µs/draw |
|---|---|---|
| 无 LTO，无 PGO | 73.2 | 7.72 |
| LTO | 74.4 | 7.58（−1.8%） |
| LTO + PGO | 76.3 | 7.35（−4.8%） |

## 第二轮：GPU 线程的时间去向，以及一个纹理辅助线程（2026-09-30）

站在猎人噩梦，FSR 4，`BB_FPS_LIMIT=0`，约 85 FPS，每帧约 1610 draw。

**CPU侧剖析**（perf，直接子节点）：`Draw` 74% —— `BindResources` 32%
（纹理 16%、缓冲 13%），`BeginRendering` 8%，`GetGraphicsPipeline` 5%，
`BindVertexBuffers` 4.5%，`ResetBindings` 4%，动态状态 2%；`DispatchDirect` 6%；
PM4 解码与寄存器哈希约 9%。没有 Self time 超过约 5% 的函数：代价是分散在纹理、
缓冲与 barrier 结构上的 cache miss（`slot_images[id]`、图像描述、backing 状态、集合写入）。

**墙钟时间**（新增 `Frame stats` 字段）：GPU 线程 91% 的时间在 CPU 上，
等待客户提交 0.5%。约 9% 阻塞在 `Scheduler::WaitHostCopies`：每帧约 130 次
（EOP/EOS事件、`WriteData`、提交）等待录制流中排队的小客户拷贝，也就是等到录制
线程把它们之前的每条命令都录完，另有约 4% 花在拷贝线程（`BbCopy::WaitAsync`）。
录制线程自身约 40% 忙碌（其余是自旋）；它不是瓶颈。

### 辅助线程上的纹理绑定（可选项：`BB_TEXTURE_HELPER=1`，toggle 1 << 22）

`bb:TexBind` 在 GPU 线程绑定其缓冲并解析顶点/索引缓冲时，绑定该 draw 的纹理
（拆为 Resolve/Emit，toggle 1 << 23）；GPU 线程在 `BeginRendering` 之前汇合。
辅助线程只走 memo 化路径（FindImage memo 有效、图像干净、无场景目标代理、
无超分重定向、无 storage image、无 mip 数组、管线中无 storage buffer）；
其余都在汇合之后绑定。GPU 线程在改动图像状态之前汇合（`Transit` 中的
`Runtime::BeforeImageAccess`、`FlushBarriers`、`SetBackingSamples`，
以及 `SynchronizeMemoryFromImage` 中纹素缓冲与图像 alias 的情形）。
68% 的 draw 并行绑定完成，图像未变—— 帧率也一样：启用辅助线程 85.9 FPS，
禁用 85.8 FPS（A/B，6 × 16 s）。

用 `rdtsc` 测量：辅助线程的任务每次 fork 约 6200 周期，而同样的工作 在GPU 线程上
约 4600 周期，且 GPU 线程在汇合时每次 fork 仍要等约 2500 周期。辅助线程写的描述符
info、图像状态与绑定标志紧接着就被 GPU 线程读取，因此每个 draw 都在核间搬运它们；
GPU 线程自身的工作变慢的幅度约等于它交出去的部分。在这款 CPU 上，对约 7 µs 的
cache 密集工作做逐 draw fork/join 不划算；该辅助线程在其他 CPU 上保留为可选项。

### 其他尝试

| 改动 | A/B | 保留 |
|---|---|---|
| 图像描述缓存中的 FindView memo（1 << 21）+ 记录块中的写预取（1 << 20） | 81.5 对 80.8 FPS | 是 |
| 用查表替代 `magic_enum::enum_contains` 的格式检查（后者每 draw 每纹理一次线性扫描）；把热的 `Image` 字段（标志、绑定、跟踪范围、backing、ticks）移到 `ImageInfo` 之前 | 在噪声范围内 | 是（无副作用） |
| host 拷贝队列：小拷贝进无锁 MPMC 队列，由空闲的录制线程运行，其余在等待时由 GPU 线程处理 | 阻塞 9.5% → 4.8%，但 86.5 对 87.2 FPS：拷贝转移到了 GPU 线程上 | 否 |
| 只读流缓冲范围不做 barrier 跟踪 | 85.9 对 87.4 FPS，两种阶段顺序皆然（未找到原因） | 否 |
| 小 host 拷贝批量合并，每 32 个记录一条命令 | 86.4 对 86.2 FPS | 否 |

顺带修掉的问题：一个 draw 准备工作线程可能让进程停在
`SurfaceFormat: Unknown data_format=15` —— 它读了一个客户仍在写的 V#，
而 `SurfaceFormat` 会断言。worker 现在改用 `TrySurfaceFormat`，把这类 draw 留给
GPU 线程。它在更慢的 PGO 插桩构建下暴露出来。

为改动的代码重新生成了 PGO profile（仅相机旋转，与旧profile 合并）。
前后对比（按用户构建方式：37c8eac 及其 profile 对当前状态的新 profile），
三次交替重启 × 50 s：86.9 对 86.4 FPS，6.47 对 6.51 µs/draw ——
变化不超过运行间散布（±1.5 FPS）。

### 什么才能扩展

把工作按 draw 移到另一个核，代价约等于它节省的量，因为数据是共享的。
要扩展需要这样的拆分：每个线程长时间拥有自己的数据：

1. **两段式管线。** 第二个线程拥有纹理缓存与图像状态
   （`PrepareRenderState`、`BindTextures`、`BeginRendering`、barrier、描述符写入、
   动态状态、draw 录制），在 GPU 线程之后跑 draw；后者保留解码、管线与缓冲缓存。
   第二个线程不能读 `liverpool->regs`（GPU 线程已在处理更后面的包）：它从 draw
   扫描器已记录的每缓冲增量（`AmdGpu::RegDelta`）维护自己的寄存器副本。
   在 draw 之外触及内存或图像的包（DMA、`WriteData`、EOP/EOS、dispatch、快速清）
   会排空管线。估计：第二段承担今天 GPU 线程约 30% 的工作，减去排空部分。
   是一次大规模、精细的重构。
2. **每 draw 更少的工作。** 按预备的 T#/S# 哈希为 stage 已解析的纹理
   （image id、view、采样器）做帧到帧 memo，并用注册表代数、图像洁净度与布局验证，
   取代逐纹理查找。
3. **fence 之前的拷贝**（约 9% 阻塞）：让它们离开 GPU 线程的关键路径，
   例如由空闲的 draw 准备工作线程排空一个拷贝队列。

## 两段式 draw 管线（2026-09-30）

`vk_draw_pipe.h`。GPU 命令线程（stage A）保留 PM4 解码、寄存器文件、常量引擎
与管线选择。一个直接 draw 或 dispatch 变成 16 MiB 环形缓冲中的一个包：
自上一个包以来写入的 32 字寄存器块（`Liverpool::pipe_dirty`，由
`ApplyGraphicsRegisterPacket` 标记）、CB/DB 尺寸提示、每个 stage 的用户数据、
扁平化用户数据与程序基址，以及 draw 参数（dispatch 则是 `ComputeProgram`）。
draw 录制线程 `bb:DrawRec`（stage B）把这些块应用到自己的寄存器副本，并执行 draw
的其余部分（`DrawRecord`、`DispatchRecord`）：纹理、缓冲、渲染目标、barrier、
描述符、动态状态、录制。当包在飞行中时，stage B 拥有纹理/缓冲缓存、运行时、
调度器、场景目标、超分器与运动状态。

- stage B 通过 `Rasterizer::Regs()/CbExtent()/CsRegs()` 读寄存器（在它那里是自己的副本，
  其他地方是 Liverpool 的），通过 `Shader::Info::UserData()/FlatUserData()/ProgramBase()`
  读 shader 用户数据，后者返回 stage B 为当前 draw 的各 stage 安装的快照
  （`Info::ud_snapshots`，线程局部）。
- stage A 在 stage B 状态上做的其他一切，先要等它排空
  （`Rasterizer::DrainDrawPipe`）：GPU 线程上每个公开的 rasterizer 入口、除寄存器
  写入/draw/dispatch/fence 之外的 PM4 包（`PipelinedOpcode`）、待处理命令
  （`ProcessCommands`）、compute 队列包、`DumpConstRam`。
- 管末/着色器事件在 stage B 上按序执行（`Rasterizer::RunInOrder`）。交给 stage B
  的某个 fence 值上的 `WaitRegMem` 视为已满足（`Liverpool::pending_fences`）：
  stage B 本来就按流顺序执行一切。未满足的等待先排空 stage B，然后让出。
- EOP fence 由 Vulkan 录制线程在其之前排队的客户内存拷贝完成后置位
  （`Scheduler::SignalAfterHostCopies`），因此 stage B 不必等它们。
- 一次提交的预备 draw 保持存活直到 stage B 已越过它们
  （`RetireSubmission`），而不是在其结束时排空。
- 缺页：stage B 像 GPU 线程一样内联处理自己的（`IsGpuSideThread`）；
  GPU 线程在处理某个内联缺页前先排空 stage B。使用 userfaultfd 时，
  GPU 线程的缺页走客户线程的加锁路径。
- 到 0x3022C 的 `DmaData`（被处理程序跳过；约 70k/s）不触发排空。
- `BB_PIPE_VERIFY=N`：每第 N 个包还携带完整寄存器文件，stage B 报告其副本有差异的字
  （游戏内未见，菜单亦然）。
- `BB_DRAW_PIPE=0/1` 覆盖默认值（硬件线程 ≥ 8 时开启）。toggle掩码现在是 64 位
  （第 20–29 位是运动矢量与超分器的裸调试 toggle，下面最初的测量也翻转过它们）：
  1 << 37 整条管线、1 << 38 fence 在 stage B 上、1 << 39 pending fence 上的
  WaitRegMem、1 << 40 dispatch、1 << 41 由录制线程置位的 fence、
  1 << 42 WriteData/DmaData/特殊 draw/翻转 IRQ 在 stage B 上、1 << 36 常量环。
  `Frame stats` 新增 `Draw pipe` 行：draw 数、等待的排空及原因、stage A 等待时间、
  stage B 忙碌时间。

结果（猎人噩梦，静止，FSR 4，`BB_FPS_LIMIT=0`，一次运行内 A/B）：

| | 管线开 | 关 |
|---|---|---|
| 16 线程 | 96.1 FPS | 80.6 FPS（+19%） |
| 4 核 / 8 线程（`taskset -c 0-3,8-11`） | 83.8 FPS | 71.0 FPS（+18%） |

过程中的步骤（同一场景 FPS）：第一版在每个非 draw 包处排空 —— 79.8
（排空约 80k/s，几乎全是空操作的 `DmaData`）；跳过这些 —— 89；fence 放到 stage B
并惰性 `WaitRegMem` —— 92；dispatch 移交 —— 93；fence 由录制线程置位 —— 103.5
（单独 A/B 该步：103.5 对 94.9）。

现在 stage B 是瓶颈（约 85–90% 忙碌，profile 见上），stage A 大部分时间在等待，
GPU 在 FSR 4 下约 80% 忙碌。剩余排空：`WriteData`（每帧约 30 个：向帧缓冲写 192 个
零字节加一个 4 字节标签，其读取方不明）、非平凡 `DmaData`、间接 draw。
下一步：把工作从 stage B 移到 stage A —— draw 的缓冲侧（ObtainBuffer、上传）
需要 stage A 拥有缓冲缓存并把命令交给 stage B。

### 第二轮（2026-09-30）

- `WriteData`、`DmaData`、其后的翻转 IRQ（缓冲标签就是一个 `WriteData`）以及 draw
  `FilterDraw` 自行处理的那些（快速清消除、resolve、深度拷贝）在 stage B 上按序执行。
  排空：每帧约 7 次（DumpConstRam、间接 draw/dispatch）。
- **常量环**（`vk_constant_ring.h`）：stage A 把小型只读客户缓冲（`ObtainBuffer` 的
  流路径，以及扁平化用户数据）拷进自己的 32 MiB 环；stage B 只绑定它们。
  一个区域在 stage B 已录制其最后一个 draw 的那次提交完成后即可复用
  （stage B 用提交 tick 给包打戳）。与「已排队工作将写入的客户内存」重叠的缓冲
  （storage buffer、DMA、WriteData、fence：`NotePendingGpuWrite`）或被 GPU 修改过的
  内存仍留给 stage B。
- `BB_PIPE_VERIFY` 还会在 stage B 上重走每个 stage 的资源表，并与 stage A 的快照
  比对（对缺页做了防护：指针到那时可能已失效）。管线选择不会刷新其 `Info` 的
  像素着色器（无用户数据）被跳过。

一次运行内 A/B（16 线程，FSR 4 Ultra Performance，toggle 位清零）：
整条管线 110.9 对 83.9 FPS（+32%）；常量环 114.2 对 102.5 FPS（+11%）。

### 每帧 GPU 时间与超分预设

GPU（RX 7800 XT）现在比 CPU 更限制帧率。预设对此影响很小：

| 模式 | FPS | GPU 忙碌 | GPU ms/帧 |
|---|---|---|---|
| 关闭超分 | 132 | 76% | 5.8 |
| FSR 3 Native AA | 116 | 76% | 6.6 |
| FSR 3 Ultra Performance（场景 640x360） | 114 | 71% | 6.3 |
| FSR 4 Quality | 115 | 88% | 7.7 |
| FSR 4 Ultra Performance | 114 | 84% | 7.4 |

降尺寸的场景目标确实被使用（每帧约 1530 个场景 draw 中的 1160 个），但在这款 GPU 上
光栅化场景代价很低：像素数减到九分之一只省约 0.3 ms。其余部分不依赖预设
（阴影贴图、全分辨率后处理与 UI、FSR 本身 —— FSR 4 比 FSR 3 多约 1.1–1.4 ms ——
以及模拟开销：barrier、拷贝、重采样）。FSR 4 在 Native AA 下启动失败
（"no free provider frame"）。

### GPU profile（`BB_GPU_PROFILE=1`，`vk_gpu_profiler.h`）

在每个渲染 pass、dispatch、超分器运行与提交结束处打时间戳；到下一个的时间记在其上
（含中间的 barrier 与拷贝；「提交之间」那段大多是 GPU 在等 CPU）。每 5 s 打印，
按标签给出每帧 GPU ms。时间戳写在渲染 pass 之外（`radv_CmdWriteTimestamp2`
在部分 pass 内会崩），且只写入 rasterizer 的调度器（presenter 有自己的）。

猎人噩梦，FSR 4 Ultra Performance，约 110 FPS（8.8 ms/帧）：

| 标签 | ms/帧 |
|---|---|
| 提交之间的 GPU 空闲 | 2.1 |
| 客户拷贝着色器 `fefebf9f`，57 次 dispatch（HLE） | 2.1 |
| FSR 4 | 1.5 |
| 客户 compute `3d5ebf4e`，8 次 dispatch | 0.5 |
| 640x360 的 G-buffer pass | 约 0.8 |
| 其余（后处理、UI、更小的 pass） | 约 1.8 |

这就是预设几乎不改变 GPU 负载的原因：只有场景 pass 会随之缩放。

拷贝着色器被 HLE（`vk_shader_hle.cpp`）为带约 1024 个小区域的 `vkCmdCopyBuffer`。
`buffer_multi_copy.comp` 现在一次 dispatch 处理一批（toggle 1 << 43）：
拷贝本身 0.4 ms/帧；加上前置的缓冲准备，该标签从 2.1 降到 1.6 ms/帧。
其余大部分是 `ObtainBuffer` 遍历合并范围（几 KiB 的拷贝散布在最多 57 MiB 的目标上）：
只同步被拷贝的部分又省了约 0.75 ms GPU 时间，但 CPU 代价更高（每部分做页保护、
小源没有流路径：108.7 对 114.3 FPS，仅源时 105.7 对 111.9），故放弃。
多拷贝着色器并未改变帧率（此处是 CPU 限制）；它在 GPU 限制时有用。

### 渲染状态 memo（toggle 1 << 44）

约 90% 的 draw 延续上一次 draw 打开的渲染 pass，但 `BeginRendering` 仍为每个 draw
重做目标查找、transition、场景目标代理与超分重定向。现在一个 draw 在调度器仍持有
那个确切 pass 时复用上一次的渲染状态（没有东西打破它：barrier、拷贝与 dispatch
会结束 pass）、输入匹配（目标 id 与 view、管线 attachment 键、场景/光栅缩放与超分
重定向状态、图像注册表代数、深度控制）、未请求 clear、且没有目标同时被该 draw 采样。
带 clear 的状态不记忆（下一个 draw 的也不同）。命中率 90%；122.1 对 111.8 FPS（+9%）；
开/关截图的差异不超过同模式下连拍两张的差异（有动画的场景）。

### 纹理集合 memo（toggle 1 << 45）

一个 stage 已解析的纹理（深度重定向之后的 image、view、backing、子资源范围）
按预备的 T# 哈希记忆，8192 个 slot，每组最多 16 个被采样 image。命中要求：
相同的图像注册表代数、相同的 backing、无重绑定、image 已更新、无渲染目标反馈、
无超分重定向；命中时只重做逐 draw 的效果（found tick、绑定标志、绑定列表、
布局transition、用途）并写描述符。命中率 84%；128.6 对 121.8 FPS（+5.6%）；
开/关截图差异在场景自身噪声内。

### 拷贝着色器合并距离

HLE 会合并范围能装进 64 MiB 的拷贝，而每个合并后的批次要同步其整个源与目标范围。
改为 64 KiB（`BB_COPY_MERGE_KB`）后：拷贝着色器的 GPU 时间 1.5 -> 0.6 ms/帧
（profiler），GPU 忙碌 85% -> 约 77%，帧率未降低（重启间有波动，125–142 FPS；
16 KiB 与 256 KiB 相近）。

## 第三轮（2026-09-30 下午）

关卡入口处的帧率，16 线程，FSR 4 Ultra Performance：约 134 -> 约 145–150 FPS
（重启间相差几个 FPS；下面的 A/B 数字各来自一次运行）。

### stage A 在何处等待

`Frame stats` 现在在排空计数旁边，打印 stage A 的时间中各排空原因与各调用点
（`function:line`，`DrainDrawPipe` 记录 `__builtin_LINE`）各占多少。
stage A 约 25% 的时间在等待，几乎全在每帧一次的某次排空上：一长串 draw 之后
最先到来的那次排空，要等 stage B 把它们跑完。最初是 `DrawIndirect`（21%），
然后是 `DispatchIndirect`，那两者管线化之后轮到一次 `DumpConstRam`。
所以 stage A 比 stage B 快，去掉排空只在打破这种锁步时才有帮助；
stage B（85% 忙碌，每包约 2.8 µs，profile 平坦）才是瓶颈。

- 间接 draw 与 dispatch 已像直接 draw 一样管线化（toggle 1 << 46）：管线在 stage A
  上选择，参数缓冲查找与间接命令在 B 上记录。+1.4%。
- 待处理的 GPU 写入（常量环的守卫）合并重叠范围，并改为每 64 次检查修剪一次而非每次：
  `PendingWriteOverlaps` 占 stage A 的 7.5% -> 3.6%；合计约 +4%。
- 纹理集合 memo：32768 个 slot；四分之一的未命中是 slot 冲突（每 5 s 从 37k -> 11k）：
  +1.3%。
- 逐帧的运动历史表：改用开放寻址而非 `std::unordered_map`（每个存储的 draw 四次
  节点分配，每帧释放）。对 FPS 中性。
- 试过：在 `DrawPipe::Commit` 中用 release 存储替代顺序一致性存储，配带定时的
  futex 休眠（+0.4%，不值得冒可能丢失唤醒的风险；已回退）。

物体运动矢量约耗 10% FPS（155 对 141 FPS，`object_motion=0`）：stage B 每包多花
约 0.28 µs，分摊在 DrawRecord各处（骨骼调色板哈希、索引范围缓存、运动管线变体）。

### 场景分辨率与预设/ GPU 负载问题

每帧 GPU 忙碌时间（profile 总计减去提交之间的空闲）：

| 模式 | GPU 忙碌 |
|---|---|
| 关闭超分（原生 1080p） | 3.67 ms |
| FSR 4 Quality | 5.64 -> 5.50 ms |
| FSR 4 Ultra Performance | 5.11 -> 4.98 -> 4.86 ms |

在这款 GPU 上光栅化场景代价很低，所以更低的预设只省约 0.5 ms；而 FSR 4 本身在
1080p 输出下耗约 1.4 ms —— FSR 4 Ultra Performance 的 GPU 时间比不开超分的原生
渲染还多。过程中发现：

- 降尺寸的判定看了全部八个颜色 slot，但超出 mask 宽度的 slot 会保留早期 pass 的目标 ——
  在光照 pass 中就是它们所采样的 G-buffer image。因此光照累加 pass 在任何预设下都
  停在 1920x1080。已修（toggle 1 << 47 可恢复旧行为）。
- 每个采样降尺寸目标的 pass 此前都要先把代理重采样到原生尺寸
  （每帧约 25 次 resolve，0.4 ms；`BB_GPU_PROFILE` 现在给 resolve、fill、image
  上传与下载打标签）。重编译器会标记那些被非归一化采样（无 offset）读取的 image
  （`ImageResource::needs_native`）；其他被采样的绑定直接读代理
  （toggle 1 << 48）：GPU 忙碌 5.02 -> 4.86 ms，截图一致。这会改`Info` 布局：
  shader meta 版本 7，管线键版本 5（缓存需重建一次）。
- `BB_SCENE_DEBUG=<file>`：触碰该文件会打印下一帧的场景 pass 及各自保持原生尺寸的
  原因。超分之前仍保持原生的：半分辨率（960x540）pass，`SceneTargets::Eligible`
  尚未处理。

下一个 GPU 项（对 Steam Deck 最重要）：客户 compute `3d5ebf4e` 是一次 dword memcpy，
拷贝渲染目标内存（1080p 深度缓冲 12 MB，之后作为 R32F 被采样；一个 G-buffer 目标；
两个 960x540 目标）：8 次 dispatch，每次都需要把 image 下载进缓冲、把代理 resolve、
再把目标 image 上传回去。识别出源是 image 的拷贝并转为 image 拷贝（或按代理尺寸拷贝）
可以消掉其中大部分。跳过这些 dispatch 会让场景变黑，所以拷贝是必需的。

### 已修：draw 管线导致的客户堆损坏（2026-10-01）

原因：GPU 空闲（`IrqC GpuIdle`，它释放 `sceGnmSubmitDone`）在 stage A 解码完每个
提交后就被置位，而延迟到 Vulkan 录制线程的管末 fence（RecorderFences）仍未完成；
客户释放了持有那些标签的对象，而它自己的另一个线程还在更新它们。
现在 stage A 在GPU 空闲之前（以及在 compute 队列的 WriteData/ReleaseMem 之前）
先排空管线并等待延迟信号。用 `BB_WRITE_LOG=2` 定位（在解码时记录 fence 目标，
避开竞争路径）。以下是调查过程。

#### 调查笔记

在关卡中待2–15 分钟后（相机转动，无人移动），客户在客户偏移 `0x263b8e7` 处故障：
一个客户分配器的 freelist 弹出从已释放的块中读到下一个指针 `0x0000005300000000`，
说明有什么东西写进了游戏已释放的内存。15–20 分钟的浸泡运行：启用 draw 管线时
4 次中 3 次崩溃（分别在 863、844 s 与更早一次），`BB_DRAW_PIPE=0` 时 2 次中 0 次。
用 `BB_WRITE_LOG=1`（下载更慢）时有一次运行撑过 20 分钟。

已排除/迄今已做：
- 客户可见写入越过延迟 fence（现已排序：`WaitDeferredSignals`，toggle 1 << 49）
  —— 崩溃依旧；
- 场景代理与纹理集合的改动：崩溃在它们之前也发生过。

候选：管线进一步延迟的对客户内存的迟到写入 —— 异步 image 下载
（`TextureCache::DownloadImageMemory`，延迟到 GPU 完成，整幅 image）、
缺页时的缓冲下载，以及当 GPU 命令线程（stage A，现为常量环读取客户内存）
未被当作 GPU 侧线程处理时的缺页处理（`IsGpuSideThreadId` 只接受 stage B）。
下一步：一次带 `BB_WRITE_LOG=1` 的崩溃运行会打印哪次被记录的写入落在损坏块附近；
二分各管线 toggle（38 任务、39 pending fence 等待、41 录制器 fence、42 内存写入、
36 常量环）。
