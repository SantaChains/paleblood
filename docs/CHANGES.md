# 变更日志（2026-10-02 与 2026-10-03 合并）

原两份俄语快照 CHANGES_2026-10-02.md、CHANGES_2026-10-03.md 的中文压缩合并。测量环境：RX 7800 XT / RADV；"弱 CPU" 为同机限 4 核 8 线程；Steam Deck 项为 720p 输出 + FSR 3 Performance（场景 640×360）。

## 2026-10-06 / 10-07

- 游戏内菜单背景模糊：新增 bbport_menu_blur（Present 线程在 UI 渲染 pass 前录制，交换链图像经 blit 源布局降采样往返 + 高斯 compute bbpost_menu_blur.comp），菜单打开时背景虚化。
- 修显示页控件 ID 冲突：CollapsingHeader 与 BeginCombo 同名同作用域报 conflicting ID，header 加 ##hdr 后缀；新增 ui_scale 界面缩放（85..150 五档，就近吸附，bbport.ini 键 ui_scale）。
- exe 图标嵌入：src/probe.rc 声明，windres 编译 .res 链入（logo/bloodborne.ico），.rsrc 段已验证。
- 修作弊代码洞写入失败根因：eboot.elf 段表两段间存在 2KB 对齐缝隙，loader 的 mapped() 只认 PT_LOAD 范围，把缝隙判为未映射。
- GitHub 公开化准备：.gitignore 增加发布卫生块（patches/dumps/savedata/sys_modules/mods/dlss 等不入库），untrack 社区补丁 XML 与官方宣传图，删除俄语文档，README 双语重写（setup.bat 一键安装 + 单行 bash -lc 的低错手动流程），新增 docs/AI-SETUP.md，run.bat 首-run 询问并记住游戏目录，docs 技术文档全面中文化，两份俄语变更快照合并为本文件。
- 排查确认：无校验会话仍概率性退出码 23（DEVICE_LOST 受控退出），nvlddmkm 事件 153 时间戳对齐，纹理缓存用量 5050 MiB 越过临界水位。**2026-10-07 复核：此前的"驱动侧概率性复位"定性有误，真实根因是阈值计算缺陷，已修复（见下）**。
- **修显存预算导致 device lost（真根因）**：`GarbageCollectImages` 此前只对集成显卡与 `gc_budget_mb>0` 时改用驱动实时预算，独显走构造期预算——而构造期预算已在驱动保留量之外**再扣一次系统保留**（1/8 heap，上限 1 GB），推出的临界水位因此**低于游戏真实工作集**。实测 8 GB AMD 卡：critical 5031 MiB 对 5050 MiB 工作集，用量只涨不落（2917→5050 并贴住 5031），每轮逐出数百张、写回数十张，最终 `vk_scheduler.cpp:456` `eErrorDeviceLost` 断言、进程停止。改为**所有 GPU 一律使用 `VK_EXT_memory_budget` 的 heapBudget**（该值已扣除驱动自身保留，且会随他进程占用收缩，正是需要退让的信号），阈值随之 70/85/95%。`gc_budget_mb` 仍可强制，但不超过驱动实时值。
- 界面状态持久化：`io.IniFilename` 原为 `nullptr`（注释写"window positions are not kept"），窗口几何、11 个 CollapsingHeader 折叠态每次启动全部复位。现指向数据目录下 `bbport_ui.ini`（与 `state.txt`/`mods.json` 同一约定），`IniSavingRate` 5 秒，关闭菜单时额外 `SaveIniSettingsToDisk` 兜底。选中的 Tab 页不是 ImGui 自动保存的窗口属性，改为新增 `ui_page` 键（与 `ui_scale` 同构，关闭菜单时随其余改动一起落盘）。
- 用户自定义风格槽位：风格预设原为编译期 `constexpr` 5 条硬编码，只读不可增删，用户调出的参数只能显示"自定义"且无处保存。现改为「内置 5 条 + 用户槽位」统一列表，菜单可保存当前调色为具名槽位、右键删除；落盘到 `user-presets.json`（原子写：临时文件 + rename，名字做 JSON 转义）。`bbport.ini` 保持为启动期配置，不混入运行期产物。风格槽只含调色参数（暗部/对比/饱和/智能饱和/CDL/黑白场/颗粒/单色），去色带与去雾、锐化不参与——它们是校正而非风格。
- 统一 `gc_budget_mb` 上限：UI 滑杆 16384 与解析侧 `std::clamp` 65536 不一致，现共用 `BbSettings::GcBudgetMaxMB`（16384）。

## 2026-10-03

- live_resolution 默认改为关闭：非 1080p 输出走启动补丁（0.1 行为）；`auto`（按显卡判断）与 `1` 需显式选择，`BB_LIVE_RES` 覆盖。
- AppImage 不再打包 FSR 4.1.1 模型：`BB_PACKAGE_FSR411=1` 才打入，否则从数据目录的 `fsr4_411\` 取。
- 修 NixOS 上 AppImage 不启动：run.sh 去掉对 sed 的依赖。
- 核显纹理 GC 预算改为驱动实时预算（VK_EXT_memory_budget）的份额：70% 收集、85% 紧张、95% 危急；`BB_GC_BUDGET_MB` 在任意 GPU 上套用同一规则。修复核显上每次提交都激进逐出又回载的抖动。
- 逐出纹理的写回改为同步（页面仍在保护期内完成）：修延迟写回覆盖游戏新数据的损坏路径，对应 Steam Deck 装载地图时的 Guest fault 0x263b8e7；PC 上同签名崩溃 10 次中出现 1 次，原因未定性。
- 活动分辨率路径降本：960×540 的后期目标随场景同比缩小，半分辨率 mip 链改为逐级独立副本；GPU 占用 3.41 → 3.23 ms/帧。CPU 侧仍贵是游戏自身行为：活动路径下光照 pass 处理 335 光源、975 draws（补丁路径 42 / 679）。
- TAA 细线远距离闪烁修复：亚像素移动的帧保留被深度测试丢弃的历史并钳制到邻居色；闪烁指标（帧间亮度二阶差分 ×1e-3）均值 0.37 → 0.27，p99.9 23 → 12；bit 55 保留旧规则。
- 超分审计核对：mip bias 已实现（G-buffer 采样器 log2(渲染/输出)，Deck 为 -1，bit 57，日志 `Upscaler: scene texture LOD bias`）；未做：粒子 jitter 分类、换相机时历史重置。
- 弱 CPU 补丁模式测量（4c/8t，209 FPS）：总占用约 545%/800%，瓶颈是"命令流 → draw 录制 → Vulkan 录制"的串行链；并行化 Vulkan 录制属大型改造，未开始。
- 工具：`BB_PRESENT_DUMP_COUNT=N` 配合触发变量连续抓 N 帧。

## 2026-10-02

- Steam Deck 与弱 GPU 回到 0.1 的整帧预设分辨率路径：活动缩放曾把后期留在 1080p 并每帧来回拷贝，Deck/GTX 1060 上只有 7-8 FPS。无 VK_EXT_shader_stencil_export 的 GPU（Pascal）缩小目标深度/stencil 恢复全尺寸。
- NVIDIA 帧节奏两项修复：Present 移入独立线程 `bb:Present`（`BB_PRESENT_THREAD=0` 回退）；命令流领先上限 `BB_FRAMES_AHEAD`（默认 1）。4K 原生 ~78 FPS：帧散布 9.9 → 0.9 ms，p99 25.8 → 14.6 ms，超中位数 1.5 倍的帧 ~125 → 0；弱 CPU 上 present 线程 +3.5%。
- 修 shader 管线缓存永久失效：Profile 不匹配从"本会话整段关闭"改为清空重建；同路线二跑编译数 241 → 28。
- 弱 CPU 剖析：166 FPS 瓶颈在 `bb:DrawRec`（88-90%）；自旋时长按可用核数（`BB_PIPE_SPIN_US`）；Relaxed readbacks 实测零成本。
- 稳定性：修双阶段管道 Guest fault 0x263b8e7——`work_retired` 判定，管道排空且延迟标签写完才算 GPU 空闲；4 核 40 分钟无复现。
- 图像：锐度上限 2.0（>1 时对成品帧附加 RCAS pass）；菜单滑杆支持 Ctrl+点击输入数值；Pascal 顶点插值整数化修复（插值结果乘 1+2⁻¹⁶ 再取整，27 个 Bloodborne shader 命中该模式）；移植上游 shadPS4 recompiler 修复（V_CVT_PK_U8_F32、V_BFM_B32 等，清单 gpu/VENDOR.txt）；TAA 四种实验技术（tonemap 空间混合、YCoCg 裁剪、方差裁剪、3×3 恢复）均无增益，保留原算法（bits 51-54 留作实验）。
- Mod 与补丁：mods 实测可用，大小写不敏感、接受 CUSA03173 包装目录与无 dvdroot_ps4 布局，日志 `Mods: N game files replaced, M added`；60/90 FPS++ 指针表补丁自动重定位（不再报 patch overlaps a relocation）；launcher 英文化；帧率限制说明 0 = 按屏幕频率、上限 120 Hz。
- 内存：15 次"退出→继续"循环 RSS 前 7 次 +179 MB、后 5 次 +8 MB，增长衰减为缓存预热而非泄漏；40 分钟单场景 RSS ~2.64 GB、VRAM ~4.29 GB 平稳。
- 工具：`BB_PAD_RECORD` / `BB_PAD_REPLAY` 手柄路线录制与回放（游戏内 F9 起停），用于装载卡顿的重复测量。

## 参考与取舍

- 菜单 UI 液态玻璃风格参考过 [imgui-liquid-glass](https://github.com/SoyBeanMilkx/imgui-liquid-glass)，确认不采用：菜单视觉走自实现的背景模糊（bbport_menu_blur，交换链 blit 降采样 + 高斯）与 ImGui 原生绘制，不引入该仓库的实现。
