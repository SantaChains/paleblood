# bbport 开发者指南

本文面向要修改本仓库代码的开发者，记录构建方法、工程约定与已知坑。架构与方案见 design-review.zh.md，代码审查结论见 quality-review.zh.md，面向使用者的说明见仓库根目录 README.md。

## 环境搭建

- 工具链为 MSYS2 的 CLANG64 环境，安装位置任意，但必须设置系统环境变量 BB_MSYS2 指向安装目录；环境变量只对新启动的进程生效，旧终端或 IDE 里改完不重开是无效的。
- 国内网络下默认镜像不通，需在 MSYS2 的 mirrorlist.mingw 与 mirrorlist.msys 顶部前置 TUNA 等可用镜像。
- 依赖包清单见 README 快速开始一节，pacman 一条命令装齐。
- 宿主 Git for Windows 的全局配置常带 http.sslBackend=schannel，MSYS2 的 git 只支持 openssl。构建与拉取子模块前需设置：

  ```
  GIT_CONFIG_COUNT=1
  GIT_CONFIG_KEY_0=http.sslBackend
  GIT_CONFIG_VALUE_0=openssl
  ```

- 强杀 pacman 会留下 var\lib\pacman\db.lck，之后所有 pacman 操作报锁，删掉该文件即可。
- 后台跑 pacman 时缓存命中不产生 .part 文件，高 CPU 是在签名校验，不是卡死。

## 构建与测试

PowerShell 中的一条完整命令：

```powershell
$env:MSYSTEM='CLANG64'; $env:CHERE_INVOKING='1'
$env:GIT_CONFIG_COUNT='1'; $env:GIT_CONFIG_KEY_0='http.sslBackend'; $env:GIT_CONFIG_VALUE_0='openssl'
& $env:BB_MSYS2\usr\bin\bash.exe -lc 'bash build.sh'   # 或 C:\msys64\usr\bin\bash.exe
```

关键纪律：构建输出必须重定向到文件，例如 `bash build.sh > out/build.log 2>&1`。宿主 PowerShell 与 MSYS 子进程之间的 stdout 管道会间歇性报 Bad file descriptor，症状是 cmake 段静默 exit 1 且零输出，看起来像真实失败，实际是假象。自动化测试同理，一律文件重定向，不要把后台失败直接当 bug。

- 首次全量构建约 8 分钟，shadPS4 核心约 245 个目标；增量构建数分钟；Windows 末端 ThinLTO 链接占一分钟左右。
- 产物：out\bb-probe.exe、out\bb-gpu-capabilities.exe、out\gpu\libbbgpu.a。
- build.sh 自动收录 src\runtime*.c 与 Windows 下的 src\win32_*.c，新增运行时源文件不需要改脚本；GPU 库由 gpu\CMakeLists.txt 管理，新增文件才需要改。
- 构建按文件 mtime 判定 Up to date；run.bat 每次启动都会先构建，BB_PREBUILT=1 可跳过。
- 测试：bash build.sh --test 跑加载器、运行时、补丁与文件 mod 测试；python3 -m unittest discover -s tests；渲染侧单测用 ninja -C out/gpu motion-history-test 等目标。
- 双击 run.bat 会自动构建再启动，内部走 scripts\run_windows.py。

## 代码地图

- src\probe.c：加载器与 boot 流程。游戏映像映射、段保护、补丁应用、作弊写入原语 runtime_cheat_write、guest 启动都在这里；VEH 页错误处理也在链路上。
- src\runtime_*.c：PS4 系统库 HLE。内存、线程、同步、文件、音频含 ATRAC9、手柄 runtime_pad.c、存档、AppContent、作弊文件解析 runtime_cheats.c。
- src\win32_*.c 与 src\host_sync.h：Windows 专属。guest 地址空间用 section 视图映射，锁与线程直接用 Win32 原语，无 POSIX 层。
- gpu\shadps4\video_core：vendored 的 shadPS4 视频核。本项目的改动一律带 bbport: 注释前缀，上游 diff 可追踪。
- gpu\shadps4\video_core\renderer_vulkan：渲染器主体。texture_cache.cpp 纹理缓存与 GC、buffer_cache.cpp 含 tiled 写回 WriteBackImageToGuest、vk_draw_pipe.cpp/.h 双阶段绘制、vk_scheduler 调度、vk_dlss.cpp DLSS、vk_temporal_upscaler.cpp 超分框架、bbport_overlay.cpp 游戏内菜单。
- gpu\shim：C 与 C++ 两侧的桥，runtime_* 通过它调 GPU 库。
- scripts\：离线准备 prepare.py、模块链接 link_libc.py 与 link_modules.py、补丁编译 patches.py、mod 合并 mods.py、Windows 启动器 run_windows.py。
- documents\：中文文档；docs\：英文设计笔记与测量数据。

## 编辑与语言约定

- 代码与代码注释一律英文；中文文档用 .zh.md 后缀放在 documents\。
- CRLF 与 LF：gpu\shadps4 vendored 文件、texture_cache.cpp、README.md 是 CRLF，其余 bbport 自建源文件多为 LF。CRLF 文件上做多行文本替换的工具会失败，改法是单行替换，或用脚本按 \r\n 显式处理。改完用 git diff 确认没有整文件行尾翻转。
- C 侧 C11/GNU11，C++ 侧跟随 shadPS4；全量 -Wall -Wextra -Werror，不允许带警告提交。
- constexpr 上下文不能调用 getenv 等运行期函数。vk_draw_pipe.h 的教训：看门狗超时若要读环境变量，写成 static inline 运行期初始化，不能 constexpr；同时注意 lambda 无显式返回类型时以第一个 return 推导，各分支返回类型必须一致。
- 错误处理风格：系统边界（用户输入、外部数据、guest 内存访问）要兜住；内部代码信任既有保证，不为不可能路径加防御，保持简洁。

## 并发与锁约定

违反这些约定都出过真实崩溃，改动前先读相关代码。

- 调度器锁序：chunk_mutex → recorder_mutex，任何路径不得反向。AcquireChunk 在持有 chunk_mutex 时取 recorder_mutex，这是唯一许可的嵌套。
- record_chunk 与 full_chunks 的全部访问必须持 chunk_mutex，包括读取和 swap。KickRecording 在 stage A 线程 move chunk 时，stage B 的 Record 曾读到 null 直接崩溃。
- direct_mode 标志必须是 std::atomic<bool>，relaxed 序即可；普通 bool 曾让 stage A 与 stage B 并发录进同一 command buffer。
- stage A 是 Liverpool GPU 命令线程。它需要等宿主拷贝完成时，应通过 RunInOrder 把等待投递给 stage B 执行，不要在 stage A 内自旋。
- DrawPipe 的 SPSC ring 依赖 acquire/release 内存序，改动前读 docs\parallel_gpu.md；自旋点都有无进展看门狗，超时走 StallFatal。
- BbCopy 线程池用 epoch 回收；新增跨线程资源先想清楚生命周期再动手。

## 运行时行为红线

这些是有意设计或来之不易的行为，不要顺手"修"。

- 纹理缓存 clean_up 跳过 SafeToDownload 且非 pressure 的图，是上游语义，保留。
- pressure 下逐出 tiled GPU 脏图前必须先 WriteBackImageToGuest 同步写回 guest。写回的分派条件用 props.is_tiled，不要用 IsTiled()：两者在 DisplayLinearGeneral 构造路径上可能不一致。
- 纹理 arena 是 sparse 绑定，没有 mapped_data，CPU 不能直读；要拿平铺字节必须 GPU copy 进 staging 再 Invalidate。
- GC 强制档：用量超 critical 后按严格最旧逐出，配额每轮 64；BB_GC_BUDGET_MB 与驱动实时预算取小者。日志反复出现 0 images evicted 是工作集超预算的 OOM 前兆，不是 GC 在偷懒。
- TryWriteBacking 用 backing 别名视图落 guest，绕过图页只读保护，不经过 VEH；页保护与 VEH 标脏逻辑在 probe.c，改动前先读。
- runtime_cheat_write 的 Windows VEH 路径已正确；POSIX exec 段分支有已知缺陷：mprotect RWX 写回时若页当前是 GPU NOACCESS 会绕过 SIGSEGV 标脏，丢失脏标记。启用 Linux 侧作弊前必须先补。
- guest 可见宿主内存必须保持在 1TiB 以下，链接参数 --disable-dynamicbase 与 --disable-high-entropy-va 服务于此，勿动。
- mods.py 的 mod 合并目录是 out\ 下指纹命名的缓存(mod-game-<sha256 前 16 位>,输入含游戏目录、mod 层与每个文件的路径/大小/mtime 及 mods.py 本身):输入不变则直接复用(2928 个硬链接从每次约 28 秒变为约 2 秒),输入变化才重建,过期目录由 sweep_stale_overlays 在 24 小时后清扫。该目录名确定,Steam 快捷方式可写死 exe 参数。

## 调试

- out\bb-probe.exe 是 RelWithDebInfo 加 ThinLTO，内含 DWARF 调试信息。
- 崩溃地址符号化：取 RIP 减去基址 0x140000000 的偏移，执行 %BB_MSYS2%\clang64\bin\llvm-symbolizer.exe --obj out\bb-probe.exe 0x偏移，直接得到源码行；llvm-objdump -d 可看崩溃处指令。
- guest 页错误在 Windows 走 VEH 恢复，GPU NOACCESS 页的裸写会先进 VEH 标脏再解保护重试，这是正常路径不是 bug。
- 常用诊断变量：BB_FRAME_STATS=1 帧统计、BB_GPU_PROFILE=1 每 pass GPU 耗时、BB_PIPE_TIMEOUT_S 看门狗秒数、BB_DRAW_PIPE=0 关双阶段绘制隔离问题、BB_UPSCALER=none 排除超分。
- 游戏日志与运行目录都在 out\，补丁与 mod 合并产物由 run_windows.py 每次启动重建。

## 避坑清单

按踩坑频率排序，细节在前文。

1. MSYS stdout 管道间歇坏，一切构建测试输出重定向到文件再读。
2. BB_MSYS2 只对新进程生效，旧终端读不到。
3. GIT_CONFIG 三变量必须在构建环境里设置，否则 git 操作被 schannel 配置卡死。
4. CRLF 文件上的多行替换必失败，单行替换或脚本处理。
5. constexpr 里不能用 getenv；lambda 分支返回类型必须一致。
6. IsTiled() 与 props.is_tiled 语义不同，写回与下载分派只能用后者。
7. arena 无 mapped_data，CPU 不能直读，必须 GPU copy。
8. pacman db.lck 残留导致锁报错，删除即可。
9. state.txt 路径转义上限 600 字符，超长路径重启不恢复，属可忽略边界。
10. overlay 的 cheats 文件列表按 readdir 顺序展示，未排序，行为固定但顺序不美观。
11. 临时产物用完即删，不留垃圾在仓库。

## 提交

- 本地提交即可，不主动推送远端。
- 提交信息聚焦动机与影响，一两句话。
- 改动 vendored 文件时保持 bbport: 前缀注释，这是上游 diff 的追踪机制。
