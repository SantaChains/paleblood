# bbport — AI 助手配置清单

本文写给在用户机器上配置 bbport 的 AI 助手：列出用户必须提供的每一项输入、放置位置与验证命令。不要虚构路径：清单之外的任何东西都向用户询问。

## 硬规则

- 绝不把游戏文件、dump、固件或厂商 DLL 放进仓库，绝不提交它们。
- 本项目不分发任何游戏内容。用户不拥有游戏就停止。
- 游戏必须是 **1.09 dump**（顶层有 `eboot.bin` + `sce_module`）。其他版本不支持。

## 需要向用户收集的输入

| # | 输入 | 必装？ | 默认位置 | 覆盖方式 | 说明 |
|---|---|---|---|---|---|
| 1 | MSYS2 安装目录 | 必装 | `C:\msys64` | `BB_MSYS2` | 只有新进程能看到（见步骤） |
| 2 | 游戏 dump 目录 | 必装 | 记录在 `out\game_dir.txt` | `--game-dir`、`BB_GAME_DIR` | 必须含 `eboot.bin` |
| 3 | 内置补丁库（shadPS4 格式） | 随仓库提供 | `patches\Bloodborne.xml` | `BB_PATCHES_DIR` | **无需任何获取动作**，clone 后即存在。缺失则启动失败（`patches not found for app version 01.09`）：帧率预设（`Uncap FPS++` / 60 / 90）、渲染分辨率预设与所有 `bbport.ini` 特效开关都按补丁名从该 XML 取内容。若确实缺失，从社区补丁仓库取 Bloodborne 1.09 XML（见 README "Mods and patches"） |
| 4 | Mod（可选） | 可选 | `mods\`（每子目录一个） | `BB_MODS_DIR` | `BB_MODS_ENABLED=0` 全关；单个启停与顺序在 `mods.json`（docs/MODS.md） |
| 5 | 作弊 JSON（可选） | 可选 | `cheats\` | `BB_CHEATS_DIR` | GoldHEN 或 shadPS4 Qt 格式 |
| 6 | DLSS DLL（可选） | 可选 | `out\bb-probe.exe` 旁 | `BB_DLSS_DIR` | 仅 NVIDIA RTX；没有则回退 FSR |
| 7 | FSR 4.1.1 资产（可选） | 可选 | `fsr4_411\` | `BB_FSR411_DIR` | 缺失或 Vulkan 特性不足时启动自动回退 FSR 3.1 |
| 8 | 设置 | 可选 | `bbport.ini` | `BB_CONFIG` | 所有键都有默认值，见下方键表 |

## 配置步骤

1. 安装 MSYS2（安装器默认值），再装 README "Quick start" 的包清单（单行粘贴，或用 `setup.bat` 图形化完成）。默认镜像不通的网络先把可用镜像（如 TUNA）放到 MSYS2 的 `etc\pacman.d\mirrorlist.mingw` 与 `mirrorlist.msys` 顶部。
2. 验证 `%BB_MSYS2%\clang64\bin\python.exe` 存在、显卡支持 Vulkan 1.3（`vulkaninfo --summary`）。
3. Git for Windows 常带全局 `http.sslBackend=schannel`，MSYS2 的 git 不支持。构建 shell 里设置：
   `GIT_CONFIG_COUNT=1`、`GIT_CONFIG_KEY_0=http.sslBackend`、`GIT_CONFIG_VALUE_0=openssl`——否则子模块/克隆步骤卡死。
4. 构建：跑一次 `build.bat`。首次全量约 8 分钟（245+ 目标），之后增量。成功标志 = `out\bb-probe.exe` 存在且 `out\build-launcher.log` 结尾干净。构建与测试输出必须重定向到文件——继承的管道会间歇性掐断 MSYS stdout 静默 exit 1（假失败）。
5. 登记游戏目录：`run.bat --game-dir <目录>` 跑一次（持久化到 `out\game_dir.txt`；直接 `run.bat` 会复用，首-run 会询问）。
6. 放置内容：`patches\Bloodborne.xml` 仓库自带无需动作；mod 进 `mods\`、作弊 JSON 进 `cheats\`、`nvngx_dlss.dll` 放可执行文件旁（可选）。
7. 用户要改默认值时编辑 `bbport.ini`。键：`language`（11 = 简体中文）、`pad_swap`、`gc_budget_mb`、`gc_writeback`、`present_mode`（`mailbox`/`fifo`/`immediate`）、`fps`（`uncap`/`60`/`90`/`30`）、`preset`（超分预设）、`output_res`、`sharpen`、`sharpness`、`model_lod`，特效开关 `effect_*`、`skip_intro`、`debug_camera`、`debug_menu`（全部经补丁 XML 应用，重启生效）。
8. 启动：`run.bat`。确认日志有 `Patches: ...` 行且无报错、用了 mod 时有 `Mods: ... game files replaced`、以及 `Overlay: menu ready`。

## 验证

- `out\run-*.log` 含 `Overlay: menu ready (Insert or L3+R3)`，无 `Fatal` / `Assertion Failed` / `patches failed` 行。
- 游戏窗口打开到标题画面；Insert 或 L3+R3 打开游戏内菜单。

## 已验证的排障顺序

1. 启动时补丁报错（`patches not found ...`）→ `patches\Bloodborne.xml` 缺失（clone 不完整或被误删）、`AppVer` 与游戏 dump 的版本不符（需 01.09），或缺所需补丁名。
2. 渲染异常 → `BB_DRAW_PIPE=0`。
3. 仍有问题 → `BB_UPSCALER=none`。
4. 提交时 device lost 且大量 `memory pressure` 日志 → 调高 `gc_budget_mb`（如 4096）。
5. 卡死 → 看门狗在 `BB_PIPE_TIMEOUT_S` 秒后终止进程；日志里查 StallFatal。
