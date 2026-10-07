[English](README.md) · 简体中文

# bbport — Bloodborne PS4 原生移植

bbport 将《血源诅咒》的 PlayStation 4 官方可执行文件直接运行在 x86-64 PC 上。它不是通用模拟器：游戏自身的 x86-64 代码原生执行，一个专为这一款游戏编写的小型运行时取代 PS4 系统库，GPU 部分由 [shadPS4](https://github.com/shadps4-emu/shadPS4) 的渲染核心派生并针对本作深度扩展。没有 CPU 模拟，没有逐指令翻译，游戏全速运行。

`windows-port` 分支为原生 Windows 版本，在 Linux 版基础上新增 NVIDIA DLSS 超分、游戏内作弊引擎、完整的手柄与中文支持。

## 免责声明

- 本项目仅供学习与技术研究，禁止用于商业用途。
- 仓库不分发任何游戏文件、游戏资产、美术、字体或解密密钥。使用前必须自备合法获得的《血源诅咒》数字版 dump，版本 1.09。
- 本项目与 Sony Interactive Entertainment、FromSoftware 无任何关联，Bloodborne 名称与相关商标归其权利人所有。
- 社区补丁与作弊文件**基本不随仓库分发**（唯一例外是启动必需的 patches\Bloodborne.xml）——其余请从原作者处获取（见"Mod 与补丁"），启用前自行确认其内容。
- 因安装、使用本项目产生的任何直接或间接后果由使用者自行承担。

## 状态

实验性但可玩。游戏可启动、可战斗、可存档，声音、手柄、存档均已验证，完整流程通关尚未验证。Windows 侧在 NVIDIA RTX 40 系显卡与 1080p/1440p 输出下验证；AMD 显卡走 FSR 路径，FSR 4.1.1 目前仅 RADV 驱动可用。

## 特性

- 原生执行。eboot 离线转换为平铺内存映像，PS4 libc 与 libSceFios2 以原生代码链接进来，加载器与运行时约五千行 C。
- 帧率解锁。社区补丁让模拟走真实帧时间，另有 30/60/90 FPS 定档模式。
- 专为 Bloodborne 设计的时域超分。本作没有速度缓冲，bbport 自行计算运动向量：相机运动来自深度与场景矩阵，物体运动来自上一帧顶点位置；场景按 Halton 序列亚像素抖动并以较低分辨率渲染，UI 在输出分辨率原生绘制。
  - DLSS，NVIDIA RTX 显卡的最佳选择，需要 nvngx_dlss.dll 放在可执行文件目录或 BB_DLSS_DIR 指向的目录。该 DLL 不随本仓库分发。
  - FSR 3.1（FireBurn/FSR-Vulkan）。
  - FSR 4 INT8 与 FSR 4.1.1 INT8，需要 shader Float16、Int8/Int16、整数点积等 Vulkan 特性，不满足时启动前自动回退 FSR 3.1。
  - TAA，原生分辨率时域抗锯齿，可与 FSR 实时互切，锐度控制同样生效。
- 多线程 GPU 命令处理。命令流在一个线程解码、另一个线程绑定与录制 draw，双阶段流水线随硬件线程数扩展，附带无进展看门狗。
- 游戏内菜单：Insert 或 L3+R3 打开，含超分、预设、锐度、输出分辨率、画面效果、免费相机与作弊页。菜单的窗口位置与各折叠区展开状态会记住（bbport_ui.ini），选中的页也存在 bbport.ini（ui_page 键）；调色风格可存为具名槽位并随时召回（user-presets.json）。
- 作弊引擎：直接读取 GoldHEN 与 shadPS4 格式的 JSON 补丁，原生内存写入，游戏的代码洞补丁原样生效。
- Mod 支持：松散文件目录按加载顺序覆盖原文件，原始游戏不被修改。

## 系统需求

- Windows 10 1803 或更新的 64 位系统，或 Linux x86-64。
- Vulkan 1.3 显卡。DLSS 需要 NVIDIA RTX 显卡；FSR 4/4.1.1 需要相应 shader 特性，FSR 4.1.1 另需 VK_VALVE_shader_mixed_float_dot_product 扩展。
- 游戏目录：任一区服的 1.09 版 dump，含 eboot.bin 与 sce_module，如 CUSA03173、CUSA03023、CUSA00900。
- 帧率解锁与渲染分辨率预设所需的社区补丁 XML（见"Mod 与补丁"）——必装，缺失时启动直接失败。

## 快速开始（Windows）

方式一，一键安装（推荐）。双击 setup.bat：弹出的窗口里选择游戏目录与设置，点 Install/Update 自动安装 MSYS2 与全部依赖包并构建，生成 bbport.ini 与快捷方式。全程无需命令行。之后可随时重跑修改设置；Save settings 只保存设置不构建。

方式二，手动，三条命令。

1. 用安装器默认选项安装 [MSYS2](https://www.msys2.org)（C:\msys64；若改位置需另设用户环境变量 BB_MSYS2）。
2. 把下面这一行整条粘贴进普通 cmd 或 PowerShell——不要自己打开任何 MSYS2 shell，脚本会选对环境：

   ```
   C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed git mingw-w64-clang-x86_64-{clang,lld,libc++,cmake,ninja,pkgconf,python,sdl3,boost,fmt,glslang,spirv-cross,spirv-headers,vulkan-headers,vulkan-loader,vulkan-memory-allocator,xxhash,zydis,robin-map,ffmpeg}"
   ```

3. 用任意 git 客户端克隆本仓库，然后双击运行：

   ```
   run.bat --game-dir D:\Games\CUSA03023
   ```

编译与运行分离：build.bat 专职构建（build_windows.py），run.bat 直接启动游戏、从不等待编译。首次运行 run.bat 会询问游戏目录并记住（out\game_dir.txt），之后双击即玩。改过源码后 run.bat 立即用现有程序启动，同时在后台低优先级重建，下次启动生效。

run.bat 内部调用 scripts\run_windows.py：准备游戏映像、链接模块、编译补丁与 mod 合并目录，然后启动游戏。存档在 user\，mod 在 mods\，补丁配置在 patches\，均为数据目录下的子目录。

AI 辅助安装（给 AI 助手照着做的配置清单）见 [docs/AI-SETUP.md](docs/AI-SETUP.md)。

## bbport.ini 设置

仓库根目录的 bbport.ini，格式为 key = value，# 开头为注释。同名环境变量优先于文件。

- language：告知游戏的 PS4 系统语言。11 简体中文，10 繁体中文，1 英语美国。1.09 dump 自带官方中文文本，无需额外 mod。
- pad_swap：1 交换 A/B 与 X/Y 面键，适配报告 Nintendo 布局的手柄，如 Switch 模式的飞智；0 保持标准映射。
- gc_budget_mb：纹理缓存预算，单位 MiB，0 为自动。自动模式取驱动的实时显存预算（`VK_EXT_memory_budget`，已扣除驱动自身保留，并会随其他程序占用收缩）；在驱动报告的预算上按 70% 开始回收、85% 加压、95% 激进。上限 16384。显存持续逼近临界线并逐出时，调高此项可换取更稳的帧时间。
- present_mode：呈现模式。mailbox 默认低延迟，fifo 强制垂直同步，immediate 无同步。对应环境变量 BB_PRESENT_MODE。
- fullscreen：1 为无边框全屏，与游戏内 F11 等效。
- output_res：输出分辨率，如 3840x2160；与 preset 配合决定渲染分辨率补丁。
- preset：超分预设，决定内部渲染分辨率。
- live_resolution：0、1 或 auto。1 时游戏保持 1080p 内部渲染、运行时缩放渲染目标，改输出与预设免重启，代价是负载更高；auto 仅对 8GB 以上独立显卡开启；1080p 输出与 TAA 恒走实时路径。

## 路径

所有用户自有内容只指定、不分发：

| 内容                                | 位置                              | 覆盖方式                |
|-------------------------------------|-----------------------------------|-------------------------|
| 游戏 dump（eboot.bin、sce_module）  | 任意位置，记住在 out\game_dir.txt | --game-dir、BB_GAME_DIR |
| 补丁 XML（shadPS4 格式）            | patches\                          | BB_PATCHES_DIR          |
| Mod（每子目录一个）                 | mods\                             | BB_MODS_DIR             |
| 作弊 JSON（GoldHEN / shadPS4 格式） | cheats\                           | BB_CHEATS_DIR           |
| 存档                                | user\                             | BB_USER_DIR             |
| 界面布局（窗口几何、折叠态）        | bbport_ui.ini                     | 随 BB_CONFIG            |
| 自定义调色风格                      | user-presets.json                 | 随 BB_CONFIG            |
| nvngx_dlss.dll                      | 可执行文件同目录                  | BB_DLSS_DIR             |
| FSR 4.1.1 资产                      | fsr4_411\                         | BB_FSR411_DIR           |
| 设置                                | bbport.ini                        | BB_CONFIG               |

## 游戏内操作

- Insert 或 L3+R3：bbport 设置菜单，含超分、锐度、效果开关与作弊页；部分改动需 Apply and restart 生效。
- F11：无边框全屏切换。
- 免费相机：菜单开启后重启生效，按住 Cross 再按 L3 切换模式，键盘为按住 Space 按 Z。与 Enemy Control 互斥。
- 游戏调试菜单：先将 DbgFont14h.ccm 与 DbgFont14h.tpf 放入游戏 dvdroot_ps4\font\，再在菜单启用并重启；左触摸板或 Tab 打开，Backspace 等效右触摸板。

## 作弊

直接使用 GoldHEN 与 shadPS4 Qt 的作弊 JSON 文件，字段含 name、id、version、process、mods，offset 为相对 eboot 模块基址的偏移。目录解析顺序：

1. 环境变量 BB_CHEATS_DIR。
2. bbport.ini 所在目录下的 cheats\。
3. 当前工作目录下的 cheats\。

规则：process 不是 eboot.bin 或 id 与游戏序列号不符的文件静默过滤；version 不匹配打印跳过原因；无关闭补丁的 one-way 条目在菜单中呈禁用态；开关状态持久化到 cheats 目录的 state.txt，重启恢复；master 是需手动勾选的 opt-in 总开关。仓库自带的样例 cheats/CUSA03023_01.09_shadPS4.json 来自 [GoldHEN Cheat Repository](https://github.com/GoldHEN/GoldHEN_Cheat_Repository)（GPL-3.0），署名见文件内 credits。

## Mod 与补丁

- Mod：mods\ 下每个子目录为一个 mod，可含 dvdroot_ps4\、一层包装目录或直接是 chr\ 等游戏目录；文件名大小写不敏感，后加载覆盖先加载。启停与顺序编辑数据目录的 mods.json（`{"order": [...], "disabled": [...]}`），新目录自动启用。详见 [docs/MODS.md](docs/MODS.md)。
- 补丁：patches\ 下放置 shadPS4 格式 XML，启动时编译进 patches.bin。帧率、渲染分辨率、画面效果开关均走此通道。
- **patches\Bloodborne.xml 随仓库分发，是启动必需**（缺失时 patches.py 直接失败）。它是移植的内置补丁库，条目源自 [ps4_cheats](https://github.com/shadps4-emu/ps4_cheats) 的 PATCHES/Bloodborne.xml 并含本地修正（如补齐 messengers 与加载画面的timestep），补丁名与 patches.py 的 FPS_PRESETS、EFFECTS 对应。
- **其余社区补丁不随仓库分发。**patches\GoldHEN\ 与 patches\shadPS4\ 两个子目录（以及你自己放入的任何文件）不在版本控制内，请从原作者或社区补丁库（如 [GoldHEN](https://github.com/GoldHEN) 补丁合集）获取。补丁版权归 Kyo、Lance McDonald、auser1337、illusion、emoose 等社区成员所有。

## 常用环境变量

环境变量优先于 bbport.ini，多数场景只需改文件。

- BB_GAME_DIR 游戏目录；BB_MSYS2 MSYS2 位置（也可设为用户环境变量，后台构建会读注册表）。
- BB_LANGUAGE、BB_PAD_SWAP、BB_GC_BUDGET_MB、BB_PRESENT_MODE：对应同名 ini 键。
- BB_DRAW_PIPE=0：关闭双阶段绘制，退回单线程 GPU 路径，用于排查渲染问题。
- BB_PIPE_TIMEOUT_S：draw 录制线程无进展看门狗秒数，默认 120，0 关闭。
- BB_UPSCALER=none：禁用时域超分。
- BB_FRAME_STATS=1 帧统计；BB_GPU_PROFILE=1 每 pass GPU 耗时；BB_FSR4_PROFILE=1 FSR 4 每 pass 耗时。
- BB_FRAMES_AHEAD：GPU 命令线程领先 GPU 的帧数，默认 1，0 不限。
- BB_LIVE_RES=1：实时分辨率切换。
- BB_DLSS_DIR：nvngx_dlss.dll 搜索目录，默认还搜索可执行文件目录。
- BB_CHEATS_DIR 作弊目录；BB_DATA_DIR、BB_CONFIG、BB_USER_DIR、BB_MODS_DIR、BB_PATCHES_DIR 重定向数据位置。
- BB_VK_VALIDATION=1：启用 Vulkan 核心校验层（开发期工具，有性能开销）。

## 故障排查

- 提交阶段报 Device lost，日志反复出现 memory pressure 且 0 images evicted：流送纹理工作集超出预算，调高 gc_budget_mb。
- 窗口冻结无响应：看门狗会在 BB_PIPE_TIMEOUT_S 秒后以错误终止而不是挂死，日志可见 StallFatal。
- run.bat 失败时窗口保持打开，错误直接可见；找不到 MSYS2 Python 时按提示装 MSYS2 或设 BB_MSYS2，注意环境变量只对新进程生效。
- 中文未生效：确认 language = 11 且 dump 为 1.09。
- 手柄按键与预期相反：pad_swap = 1。
- 渲染异常：BB_DRAW_PIPE=0 复测；仍异常再用 BB_UPSCALER=none 排除超分。

## Linux 版

原始 Linux 版继续受支持：

```
bash build.sh
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

GTK4 启动器、AppImage 打包与 Steam Deck 细节见 launcher\、packaging\ 与 docs\。

## 仓库结构

- src\：加载器 probe.c 与 HLE 运行时 runtime_*.c；Windows 专属实现在 win32_*.c 与 host_sync.h。
- scripts\：游戏映像离线准备、模块链接、补丁编译器、Windows 启动器 run_windows.py。
- gpu\：渲染库。vendored 的 shadPS4 视频核加本项目改动，含 ImGui 菜单、DLSS/FSR、双阶段绘制管线与帧捕获。
- patches\：内置的 Bloodborne.xml（启动必需，随仓库分发），以及放自己社区补丁 XML 的目录（GoldHEN\、shadPS4\ 两个子目录不在版本控制内）。
- tools\：开发与测量工具。
- tests\：加载器、运行时、补丁与渲染测试，bash build.sh --test 运行。
- documents\：中文文档，设计评审 design-review.zh.md、代码质量审查 quality-review.zh.md、开发者指南 dev-guide.zh.md。
- docs\：设计笔记与测量数据，含超分、并行 GPU、运动向量与更新日志。

## 许可证与致谢

bbport 以 GNU GPL v2 或更新版本授权，包含 shadPS4 的 GPL-2.0-or-later 代码。第三方组件保留各自许可证：shadPS4 视频核与 shader 重编译器、sirit（BSD-3-Clause）、half（MIT）、FireBurn 的 FSR-Vulkan（MIT）、AMD FidelityFX SDK（MIT）、LibAtrac9（MIT）、Dear ImGui（MIT）、DejaVu 字体（DejaVu 许可证）、dxil-spirv（MIT，用于构建 FSR 4.1.1 资产）。作弊样例源自 GoldHEN Cheat Repository（GPL-3.0）。AMD 的 FSR 4 DLL 与模型数据、NVIDIA 的 DLSS DLL、游戏补丁与一切游戏内容均不在本仓库分发。

Linux 原版与社区镜像仓库位于 [yumlevi/bloodborne_pc](https://github.com/yumlevi/bloodborne_pc) 与 [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)——感谢在两个仓库测试、调试与反馈的每一个人。GPU 侧的地基是 shadPS4 团队的渲染器；社区补丁与作弊仓库让帧率解锁与作弊成为可能。
