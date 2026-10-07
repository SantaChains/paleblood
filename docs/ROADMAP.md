# bbport 现状与路线（2026-10-07）

本文是当前状态的快照。历史过程见 git log 与 docs 下各专题文档（upscaler.md、parallel_gpu.md、motion_vectors.md、DEPTH_ADAPTIVE_TAA.md），面向使用者的说明见仓库根 README 与 README.zh.md。

## 已实现

执行与运行时

- 官方 1.09 eboot 原生执行：离线转平铺内存镜像，PS4 libc 与 libSceFios2 以本机代码链入；加载器与 HLE 运行时约五千行 C。
- Windows 与 Linux 双平台；Windows 走 section 视图 guest 地址空间与 Win32 原语。
- 手柄（含 Nintendo 布局交换）、存档、AppContent、ATRAC9 音频、简繁中文文本（1.09 自带官方中文）。

图形

- shadPS4 视频核 vendored，双阶段 draw pipeline（PM4 解码线程 + 资源绑定/draw 录制线程，16 线程 +19%），无进展看门狗。
- 时间超分框架，自算运动矢量（相机来自深度与场景矩阵，物体来自顶点上一帧位置重放），Halton(2,3) 八相 jitter；DLSS、FSR 3.1、FSR 4 INT8、FSR 4.1.1 INT8、原生 TAA 热切换，锐化滑杆共用。
- 输出分辨率 1440p/2160p：场景按预设降渲染，UI 原生输出分辨率光栅化。

游戏功能

- 帧率解锁与 30/60/90 预设、渲染分辨率预设、画面特效开关（经社区补丁 XML 编译）。
- 游戏内菜单（Insert 或 L3+R3）：超分、预设、锐化、输出分辨率、画面效果、自由相机、作弊页，改动可 Apply and restart。
- 作弊引擎直接读 GoldHEN/shadPS4 JSON，本机写内存，状态持久化；文件 Mod 硬链接合并装载（见 docs/MODS.md）。

工程

- setup.bat 图形化一键安装（内置 .NET csc 自编译），build.bat 构建，run.bat 首-run 引导；docs/AI-SETUP.md 供 AI 助手复现配置。

## 已知问题

- 概率性驱动复位（nvlddmkm 事件 153，退出码 23）：纹理缓存用量逼近临界水位（约 5.0 GiB）时快会话可复现；根因在驱动侧，根治方向是 DDU 换驱动分支。代码侧候选缓解为流送背压：水位近临界时暂停新的纹理上传，压制逐出-回传 churn，尚未实施。
- FSR 4 Ultra Performance 档角色仍有残影（ghosting）。
- Steam Deck 上纹理与缓冲并发预取不增益（数据在 draw 间跨核迁移），瓶颈在 GPU 命令线程。
- 场景内精灵（烛光、火光等 ≤6 索引全屏规则）不随 jitter 移动，静止画面边缘略有差异。
- state.txt 补丁路径转义超 600 字符时重启不恢复（超长路径边界）。
- Linux 侧作弊写回存在缺陷：POSIX exec 段 mprotect RWX 路径在页为 GPU NOACCESS 时绕过 SIGSEGV 标脏，启用 Linux 作弊前必须先修（Windows VEH 路径正确）。

## 待办方向

- 流送背压（对应上述驱动复位缓解）。
- 帧生成：游戏无运动缓冲，可行性建立在自算运动矢量之上，未开工。
- 粒子/雾等透明物体的反应性遮罩（当前用启发式 mask）。
- 游戏内菜单的作弊文件列表排序（现按 readdir 顺序）。
- overlay 目录 manifest（name/version/serial/优先级），兼容 BBLauncher 等社区目录来源——按 design-review 结论走文件接口，不引入外部 GUI。
