# bbport 设计评审与增强方案
日期: 2026-10-06。性质: 研究+设计文档,不含代码改动。
范围: mod 配置原理、作弊码原生支持、参数调节界面决策、核心技术优化、Windows 适配缺陷、现代 C++ 工程设计。
依据: 项目源码精读(含本日两处崩溃级修复)、BB_Launcher 源码、上游 shadPS4 源码、业界与学术调研(来源见文末)。

## 一、现状盘点(项目已有机制)

配置与调参
- 环境变量族: BB_GAME_DIR、BB_MSYS2、BB_GC_BUDGET_MB、BB_DRAW_PIPE、BB_DLSS_DIR、BB_PREBUILT 等,零散无注册表。
- bbport.ini(由 BB_CONFIG 指向): 启动器与运行时的持久配置,读写集中在 run_windows.py 与 gpu/shim。
- in-game overlay 菜单(bbport_overlay.cpp/bbport_settings): Insert 或 L3+R3 呼出,自绘实现,窗口线程驱动,已有基础设置项。
- BbToggle 体系(bbport_toggles.h): runtime_disabled_optimizations 位掩码,约 20 个编译期固定的开关,运行期只读。
- 补丁: 启动期把 PS4 补丁 XML(如 Uncap FPS++)编译为 patches.bin,bb-probe 按重定位基址写入 guest 镜像。一次性、静态、无开关管理。
- mod: scripts/mods.py 把 game dump 与 Mods 目录合并为临时 overlay 目录(硬链接),--app0 传入。目录名 tempfile.mkdtemp 随机生成,是 Steam 快捷方式无法稳定指向 exe 的直接原因。
- 作弊: 无。但 D:\Games\bbconf\cheats\CUSA03023_01.09_shadPS4.json 已有现成的 shadPS4 格式 Bloodborne 作弊文件(进程名 eboot.bin,master+mods 的 offset/on-off 字节对)。

本次会话已修(背景,详见 git log)
- Scheduler record_chunk 跨线程 move 竞态(chunk_mutex): bb:DrawRec 空指针写 0x8 崩溃的根因。
- 纹理 GC aggressive 逐出窗口倒置 160→16: Bloodborne 全场景流送下逐出候选恒为 0 的根因。
- run.bat 注册表回退读取 BB_MSYS2: 修复旧环境进程启动"没反应"。

## 二、三方研究结论

BB_Launcher(Qt 源码, D:\Games\bloodborne_pc\BB_Launcher)
- mod 机制: ModManager.cpp 用 Mods → Mods-Active → 游戏 dvdroot_ps4 的三段式: 启用=校验目录结构后以 symlink/copy 放入 Active,原文件进 Mods-BACKUP。纯文件层,无进程注入。禁用=回迁。
- 设置: LauncherSettings.toml 单文件;UI 为 Qt widgets + WebView2 混合,依赖重。
- 作弊/补丁: ShadCheatsPatches.cpp 只做文件级管理(下载、列出、删除),数据结构 MemoryMod/Cheat/PatchInfo 证明 cheat 本质=内存补丁配置;生效依赖 shadPS4 运行时,launcher 自身不写内存。
- ModDownloader 集成 Nexus(API key、zip/7z/rar 解包)。解包与路径处理是供应链与路径穿越的风险面。

上游 shadPS4(D:\Games\bloodborne_pc\shadPS4)
- 运行时补丁引擎: common/memory_patcher.cpp 的 AddPatchToQueue/ApplyPendingPatches/PatchMemory。XML 与 JSON 双格式,支持 Address/Offset/Target/Size/mask/mask_jump32;serial 不匹配则挂队列,模块加载后再应用。
- 作弊 UI: qt_gui/cheats_patches.cpp,按 gameSerial+gameVersion 从 GoldHEN/shadPS4 两类仓库下载 JSON;checkbox 级开关,应用到 guest 内存。
- 配置: common/config.cpp 的 ConfigEntry 支持默认值/全局值/每游戏值三层覆盖;Qt 侧 settings_dialog 七个 tab 统一写 config.toml。
- 对 bbport 的关键差异: 上游 guest 是解释/转译执行,补丁经其内存层;bbport 的 guest 是真实 x86-64 代码运行在真实地址空间,内存补丁=直接写本进程内存,机制更简单但必须绕行 GPU 保护页跟踪与写日志(BbWriteLog),并受 Win32 占位符内存层约束。

业界与学术
- 配置分层: RPCS3(全局 config.yml + 每游戏 custom config)、Ryujinx(ConfigurationState 反应式订阅,版本化迁移)优于 shadPS4 现状。分层覆盖+不支持热应用的项显式标记"重启生效"是共识。
- 补丁/作弊格式生态: GoldHEN 仓库按 CUSA+版本+进程命名,JSON/SHN/MC4 三格式,mods 数组含 offset 与 on/off 字节对、master code 依赖;RPCS3 patch.yml 以 PPU-hash 为键(对 bbport 应换 eboot build-id);yuzu 目录式 mod 约定(load/<tid>/<mod>/{exefs,romfs});PCSX2 pnach 逐帧解释执行,对原生执行场景不适用。
- 缓存算法: PostgreSQL clock-sweep(环形扫描+usage_count 递减+pin 检查)规避 strict-LRU 的锁竞争与扫描污染,适合替换/增强当前纹理 GC 的精确 LRU。
- 回收与录制: C++ hazard pointer/RCU(P2530/P2540 系提案)在单写者场景可简化为 epoch 批量回收;Fossilize 把管线创建参数哈希后序列化,离线/后台重建管线,是消除运行时 shader stutter 的成熟方案。
- Feature toggle 分类(Fowler): release/experiment/ops/permission 四类,寿命管理入清单,过期即删;bbport 的 kill-switch 型 toggle 可长存,实验型应入注册表并定期清理。

## 三、调节界面决策

结论: 不引入 Qt,不做独立外部 GUI。理由:
1. bbport 的窗口线程与渲染线程模型是自管的(WindowSDL + 自绘 overlay),引 Qt 意味着事件循环整合、二进制体积、MSYS2 依赖面与崩溃面全面扩大;BB_Launcher 的 WebView2 混合方案依赖更重,是反面参考。
2. 全部参数(图层层级、GC 预算、toggle、补丁开关、DLSS/FSR、作弊)均可归约为"文件+overlay"两层: 持久层用分层 TOML,交互层用已有 in-game overlay 增强。
3. 与社区生态协作(用户已有 bbconf、BBLauncher 数据目录)走文件接口比嵌 GUI 更稳: 读兼容格式,写自有格式。

分四步落地(均不动渲染热路径):
1. 配置分层: defaults(编译期)→ bbport.ini(全局)→ <serial>.ini(每游戏,放 bbconf/custom_configs/)→ 环境变量/命令行(最高)。每游戏层由启动器按 serial 选择合并。
2. BbToggle 注册表化: 从裸位掩码改为具名注册表(枚举+名称+说明+默认值+类别单源声明),overlay 菜单与配置文件共用同一元数据,消灭 20 位魔法数;类别采用 Fowler 分类,实验型标注寿命。
3. overlay 增强: 在现有菜单加 Cheat/Patch 页,读第三节引擎暴露的运行时清单,checkbox 直接映射 on/off。
4. 可选后期: 本地只读状态面板(IPC/文件轮询,给外部工具看 FPS/缓存/逐出统计),不做可写外部 GUI,规避并发控制面。

## 四、作弊码支持设计(原生执行特化,已落地)

格式兼容(数据层)
- 主格式采用 shadPS4 JSON(bbconf 已有样例,生态成熟,上游仓库可下载);读取端兼容 GoldHEN JSON 的 master/mods 语义。字段: 进程名、master 列表、mods 列表(Offset、On/Off 字节、可选注释与类别)。
- 与静态补丁统一: patch 与 cheat 是同一引擎的两种生命周期——patch 启动期一次写,cheat 运行期可开关。

应用引擎(机制层,bbport 特化,2026-10-06 修订并落地)
- 原生执行关键结论(本节主要修订): guest 是真实 x86-64 代码原位执行(image 缓冲即 guest,重定位写 (uintptr_t)image+value,eboot vaddr 0 = image offset 0),GoldHEN 代码洞(cave code)与相对跳转字节可原样写入并被游戏自己执行,字节保真即行为保真。offset 语义已数学验证: 样例 0x18F78B1 处 E9 rel32=0x037E3F5A,0x18F78B6+0x037E3F5A=0x50DB810 恰为 master 洞落点。
- 帧钩子取消: 原设想"声明式 master 条目+host 帧钩子逐帧执行"不必要——注入洞代码由游戏自己执行,宿主侧逐帧机制在格式语义中不存在(GoldHEN 也是 toggle 时一次性写),引擎不含任何帧钩子。
- master opt-in: 上游 shadPS4 Qt 只解析 mods 数组、忽略顶层 master;mods 字节自包含,自动应用 master 反而引入文件作者未预期的行为变更(master 跳转会改游戏代码路径),故 master 以 opt-in 伪条目暴露于 overlay。
- 写入原语(probe.c runtime_cheat_write): 原设想经 runtime_memory_write_backing 写入不成立——该函数写 shadow backing(GPU flush 用),非 guest 可见页。实现为页感知三分支: NOACCESS 页故意裸 memcpy,触发 VEH 进 bbgpu_handle_fault 标脏+解保护+重试(保 GPU 失效语义);RX 页 VirtualProtect RWX+memcpy+恢复+FlushInstructionCache;RW 页直接 memcpy。写入前过段表 mapped() 校验,拒绝区间外目标。
- 应用时机与并发: boot 期在 apply_patches 之后、段保护之前(image 全 RW,GPU 已 init 无帧无页保护,全走直接路径);运行期开关由 overlay present 线程独占调用,时序上排除与 stage A/B 的撕裂。
- 状态持久化: JSON 同目录 state.txt(TSV: path 与 mod 名两列,M 代表 master),临时文件+rename 原子写,按 path+mod 名恢复,失败条目清除 enabled 并落盘。目录约定: BB_CHEATS_DIR > dir(BB_CONFIG)/cheats > cwd/cheats;按 process==eboot.bin 与 id==serial 静默过滤,version 不匹配打印跳过。

安全边界(硬约束)
- cheat 文件是数据,不是代码: 解析器白名单字段、字段类型与大小上限;所有地址/偏移经范围校验;禁止表达式求值、禁止指向 host 区间。
- 下载面: 从 GoldHEN/shadPS4 仓库拉取时仅接受 HTTPS、大小上限、字段白名单校验后才落盘;不自动执行任何仓库侧逻辑。
- 崩溃隔离: 应用失败的条目只影响自身(记录+跳过),不中断游戏;所有写入点已有 host fault 恢复(VEH BB_RECOVER)兜底。

## 五、mod 系统改进

现状问题: overlay 目录名随机(每次启动重建,成功路径由启动器 finally 清理,仅整个进程被杀时残留);随机名的实际代价是外部快捷方式无法稳定指向 exe(已由 run.bat 环境修复缓解启动问题,但 exe 直启仍不可行)。参考 BB_Launcher 的 Mods-Active/BACKUP 三段式与 yuzu 的目录约定,方案:
1. overlay 目录名固定为 out\mod-game(serial 语义),每次启动先清空再重建(硬链接廉价,重建成本可忽略),消除随机名与垃圾残留;Steam 即可稳定填 out\bb-probe.exe+参数,或继续填 run.bat。
2. mod 清单化: 每个mod 目录带 manifest.toml(name、version、target serial+version、优先级),启动器按优先级叠加,冲突时高优先级胜出并告警。
3. 兼容现状: 无 manifest 的目录按现行为(字母序)处理;BBLauncher 的 Mods 目录可直接作为来源之一。
4. 安全: 解包 zip/7z 时做路径穿越检查(条目不得包含 .. 与绝对路径、不得为符号链接逃逸),这是 ModDownloader 类功能最常见的供应链事故点。

## 六、核心技术优化(按引用价值排序)

1. 纹理 GC: 精确 LRU 改 clock-sweep 变体。现有 LRU+tick 分级已修出正确基线,但严格 LRU 在全场景流送下扫描成本高、逐出批小;clock-sweep(环形数组+usage_count+逐出时 pin 检查)更抗扫描污染,且与"按超额字节配额逐出"的目标天然契合。保留 tiled 脏图跳过为短期行为,中期用 tile_manager 转换写回(上游已有该能力)。
2. 帧 stutter: Fossilize 式管线序列化(已实现,先于本评审落地)——vk_pipeline_serialization.cpp 把管线创建状态与着色器元数据确定性序列化(ShaderBinary/ShaderMeta/PipelineKey 三层版本号),PipelineCache::WarmUp 启动期回放、Sync 落盘,Bloodborne 首次进入区域的长卡顿已由此消除;后续仅需随 Info 布局演进推进版本号。
3. 回收模型: chunk 池与 prepared-draw keepalive 统一为单写者 epoch 批量回收(简化版 RCU),替代逐对象生命周期推理;record_chunk 的 chunk_mutex 修复保留,槽位化(发布索引而非 move unique_ptr)作为无锁进阶,仅在锁成为热点时做。
4. stage A→B 投递统一: 所有 stage A 触碰共享态的路径(WaitHostCopies、WriteData、EOS)收敛到 RunInOrder 有序投递,消灭"口头契约+散点 Drain"模式;配合 DrawPipe 的 WaitForSpace/Drain 加超时退化 yield 与看门狗计数,消除 stage B 卡死导致的永久挂起。
5. 配置热生效: 借鉴 Ryujinx 反应式模型,但按 bbport 实际做减法——可热项(overlay 可见的运行期参数)用原子字段+订阅回调;不可热项标记重启生效;绝不引入通用配置变更总线。

## 七、Windows 适配缺陷清单

1. win32_memory.c 的 views 有序数组插入/删除为 O(n) memmove;高频 map/unmap 下应换侵入式平衡结构。线程契约(依赖 runtime_memory.c 写锁、release 的 NOACCESS 窗口)已注释固化(2026-10-06)。
2. 视图替换窗口: "unmap 触及区间后重映射剩余片段"窗口内并发 guest 访问会收到 NOACCESS 异常,依赖上层锁串行化;契约已注释固化,长期可改为按视图拆分 section 规避整段重映射。
3. host_sync.h HostRecursiveMutex 的重入快速路径: 已审读确认健全——活线程 id 唯一,owner==self 只能由持有者自身写出,属程序序保证而非 TOCTOU;契约已注释固化(2026-10-06)。
4. fault 路径: VEH(probe.c)→ GPU fault → BB_RECOVER → CrashRpt 的顺序正确,但 CrashRpt1500.dll 作为第三方崩溃报告器与 VEH 的过滤器优先级关系未固化,建议在文档中固定注册顺序与过滤策略。
5. 显存预算: GC 阈值来自 VK_EXT_memory_budget 单一来源;Windows 可选补 IDXGIAdapter3::QueryVideoMemoryInfo 交叉验证(驱动 596.36 上 NV 预算波动大),并对 critical 阈值 clamp 到不高于实际预算。
6. 系统下限: VirtualAlloc2 占位符 API 要求 Win10 1803+,probe 已有检查与明确报错(exit 21),保持。

## 八、路线图

P0(稳定性收尾,已完成或近完成): chunk 竞态修复、GC 窗口修复、run.bat 环境回退、overlay 目录固定名、mod manifest 与解包路径校验。
P1(能力面): 作弊引擎(格式解析+静态应用+overlay 开关页;帧钩子经论证不必要,见四)、配置分层与 BbToggle 注册表化、GC clock-sweep 与按量逐出。
P2(体验面): Fossilize 式管线预编译、tiled 脏图写回、epoch 回收统一、显存预算双源校验、状态面板。
每项独立可回退;toggle 类改动全部走注册表声明,保持"决策点与决策逻辑解耦"。

### 2026-10-06 进展(崩溃驱动)

实测闪退签名: 纹理用量爬过 critical(峰值 3796/3891 MiB)且每轮 0 evicted,submit 时 `vk_scheduler.cpp:456 Device lost during submit`(驱动重置)。根因分层:

1. GC 按量逐出已落地(texture_cache.cpp): 在 aggressive 窗口档之后新增强制档——仍超 critical 时按 tick 严格最旧优先逐出(`ForEachItemBelow(gc_tick+1)`),配额 64,GPU 写过的图先同步写回;不再依赖"图会变闲"的年龄窗口假设。同时修正 clean_up 配额语义: 只有真实逐出才消耗配额,被跳过的 tiled 脏图不再空耗配额导致整个 pass 无效。
2. DrawPipe 看门狗已落地(vk_draw_pipe.h): Drain 与 WaitForSpace 两个纯自旋点增加无进展检测(默认 120 s,BB_PIPE_TIMEOUT_S 覆盖,0 关闭),stage B 死亡从静默挂起变为明确 fatal 输出。
3. 显存预算双源校验已落地: BB_GC_BUDGET_MB 强制预算与 VK_EXT_memory_budget 实时预算取小者,驱动侧预算塌缩时阈值随之下降。
4. tiled 脏图写回(P2,已落地,未编译): BufferCache::WriteBackImageToGuest(buffer_cache.cpp)——GetArena+EnsureResident 备好覆盖图区间的 arena 并排队 sparse 绑定,TileImage 记录"GPU 图→DeviceLocal staging→tiling compute→arena(平铺 guest 布局)",runtime.CopyBuffer 入 HostCached staging,scheduler.Finish() 经提交回调先提交 sparse 绑定再执行并等待,staging Invalidate 后经 TryWriteBacking(backing 别名视图,同一物理页的第二映射,绕过图页只读保护且不触发 VEH)落 guest;全程同步,调用返回时无任何延后于 FreeImage 的工作,deferred 写回踩 guest 内存的前科不复现。DownloadImageMemory 按 props.is_tiled 分派(linear-general 走既有 pitch 拷贝,注意 IsTiled() 基于 tile_mode,在 buffer 构造路径与 props.is_tiled 可不一致),clean_up 的 tiled 跳过删除,强制档可回收这部分显存。页保护时序与线性路径同构: 写回期间图页保持只读,游戏并发写 fault 进 InvalidateMemory 由既有路径处理。

### 2026-10-06 进展(作弊引擎落地)

P1 作弊引擎全量落地(遵守约束,未编译未运行): src/runtime_cheats.h/.c 新增——白名单递归下降 JSON 解析(仅 name/id/version/process/credits/master/mods 字段,深度限 32,硬上限 16 文件/128 mod/64 写/4096 字节/4MB 文件),hex offset(可选 0x)与 on/off 字节串解析,无 off 即 one-way;state.txt 状态持久化(转义+原子写)。probe.c 增 runtime_cheat_write 页感知写入原语与 boot 接入(sfo TITLE_ID/APP_VER 过滤,apply_patches 后应用,cpu_only 跳过)。bbport_overlay.cpp 增 Cheats 页(文件折叠头、master opt-in 条目、mod 复选框、one-way 禁用态、写入错误显示)。build.sh 的 runtime glob 自动收录新文件,无构建脚本改动。下次编译验证要点: -Werror 干净、bbconf 样例解析、NOACCESS 页 VEH 重试路径、开关往返与重启恢复。

tiled 脏图写回同日落地(未编译): 见"进展(崩溃驱动)"第 4 条修订——clean_up 不再跳过 tiled GPU 写图,显存紧张时的强制档可回收这部分显存;验证观察点=逐出计数增长且无 Device lost、流送场景纹理用量受控。

### 2026-10-06 进展(DLSS/图形/驱动审查)

对 DLSS 集成、设备特性面、交换链与 present 链路做了一轮增强审查,结论与动作:

1. DLSS 集成核验通过,不改代码。vk_dlss.cpp 的 NGX 直连(注册表定位 _nvngx.dll、vtable 槽位固定+运行时回读探测)经 NVIDIA DLSS SDK 310.7.0 官方头(nvsdk_ngx_defs.h)交叉核验: MSVC 逆序 vtable 槽位全部正确;NVSDK_NGX_DLSS_Feature_Flags 位定义正确(IsHDR=1<<0、MVLowRes=1<<1、AutoExposure=1<<6);未设 DepthInverted 位与 PS4 standard depth 语义一致;MSpecVersion/PerfQuality/OutWidth 全套齐备。310.7 已废弃 Sharpness(现有 SetF 无害)。DLSS Preset(Hint_Render_Preset J/K/L/M)交由驱动 OTA 默认(transformer K 为 Quality/DLAA 默认),不引入固定 preset。
2. Fossilize 式管线序列化经代码审查确认已完整实现(§六.2 修订),P2 对应条目视为完成。
3. present 配置链补全: 交换链已支持 BB_PRESENT_MODE(Mailbox 默认/Fifo/Immediate),本次把 present_mode 键接入 run_windows.py 的 bbport.ini 映射,VRR 用户可配 Fifo(VSync+G-Sync)、测延迟可配 Immediate。
4. 设备特性面(vk_instance.cpp)评估: 扩展启用面已覆盖需求面(maintenance5/8、robustness2、extended dynamic state 3、NVX binary import+image view handle、memory budget 等),Vulkan 12/13 特性(timeline semaphore、bufferDeviceAddress、synchronization2、dynamicRendering)全开,无缺失项;descriptor buffer 类重构收益不抵成本,不做。
5. P2 新增候选(均为提案,未落地): VK_NV_low_latency2(Reflex 低延迟,需 acquire/present 信号量全链改造,收益=输入延迟);VK_EXT_present_timing(目标 present time 对齐 DWM,596 驱动支持);VK_EXT_depth_bias_control(NV 支持时深度偏移按目标格式精度设置,消除 24bit 深度换算误差);VK_EXT_external_memory_win32(guest section 直接导入 VK 内存,纹理路径从 staging 拷贝变零拷贝,架构级改动)。

## 九、参考来源

- 本项目源码: gpu/shadps4(重点 vk_scheduler、vk_draw_pipe、texture_cache、page_manager)、src(probe.c、win32_memory.c、host_sync.h、runtime_memory.c、runtime_cheats.c)、scripts(run_windows.py、mods.py)、gpu/shim(bbgpu.cpp、bbport_overlay/settings/toggles/copy)。
- BB_Launcher: modules/ModManager.cpp、modules/ModDownloader.cpp、settings/ShadCheatsPatches.cpp/.h;数据目录 D:\Games\Bloodborne\BBLauncher。
- 上游 shadPS4: src/common/memory_patcher.cpp、src/common/config.cpp、src/qt_gui/cheats_patches.cpp、src/qt_gui/settings_dialog.cpp。
- bbconf 样例: cheats/CUSA03023_01.09_shadPS4.json、patches/shadPS4/Bloodborne.xml。
- 业界与学术: RPCS3 patch 体系与 custom config、Ryujinx ConfigurationState、yuzu Game Modding、GoldHEN Cheat Repository 格式、PostgreSQL clock-sweep(boringsql;arXiv 2512.22995)、C++ hazard pointer/RCU 提案(P0566 等)、Fossilize 录制/回放、Martin Fowler Feature Toggles、shadPS4 v0.17 GC 更新说明。
