# 文件 Mod 与社区补丁

本仓库不附带任何 Mod、补丁 XML 或作弊文件；它们由用户自行获取，放入对应目录后由启动流程装载。原游戏文件永不改动。

## 文件 Mod（Windows）

Mod 是松散文件目录：启动时通过硬链接与目录 junction 合并出一棵只读的游戏文件覆盖树，游戏读到的替换文件，其余仍是原文件。存档目录不受影响。

目录约定：

- Mod 根目录默认是数据目录下的 `mods\`（环境变量 `BB_MODS_DIR` 可改；`BB_MODS_ENABLED=0` 整体关闭）。
- `mods\` 下每个子目录是一个独立 Mod。目录内接受的布局：
  - `dvdroot_ps4\` 本身；
  - `app0\dvdroot_ps4\` 或 `CUSA03173\dvdroot_ps4\`；
  - 解包产生的单层包装目录（如 `某个Mod v1.2\dvdroot_ps4\...`），旁边可放 readme 与图片；
  - 不含 `dvdroot_ps4`、直接是游戏目录的布局（`chr\`、`parts\`、`param\` 等）。
- 文件名大小写不敏感：Mod 里 `DVDROOT_PS4\Chr\C0000.chrbnd.dcx` 会正确替换游戏的 `dvdroot_ps4/chr/c0000.chrbnd.dcx`。
- ZIP/7z 压缩包需先解包；Mod 内的符号链接与 junction 不被接受。
- Mod 不能替换 `eboot.bin`、`sce_module`、`sce_sys`：可执行代码走补丁通道（见下节）。

与 BBLauncher 的互操作：BBLauncher（Nexus 的 shadPS4 专用启动器）的 Mod Manager 采用 Generic Mod Manager 模式——启用时把文件复制进游戏目录、原文件留 BACKUP 备份、停用时还原。其 `Mods` 目录布局与本节完全一致（每个 Mod 一个子文件夹，内含 `dvdroot_ps4` 或 `sfx`、`parts`、`map` 等原目录路径），因此可以直接 `BB_MODS_DIR` 指向 BBLauncher 的 `Mods` 目录复用同一批 Mod，无需转换。理念上的差别：BBLauncher 会写入游戏目录并依赖备份还原，bbport 从不修改游戏目录——合并产物在 `out\mod-game-<指纹>` 临时目录里，删除即回到原版，没有还原出错的风险。

启用与顺序：

- 启停与加载顺序记录在数据目录的 `mods.json`：`{"order": ["Mod A", "Mod B"], "disabled": ["Mod A"]}`。Windows 下手工编辑此文件即可启停或调序；新发现的 Mod 目录自动启用、按字母序追加到末尾。
- 列表中靠后的 Mod 优先级更高；两个 Mod 替换同一个文件时后者整体胜出（不做 `.dcx`/`.bnd` 内容级合并）。
- 诊断：`BB_MOD_TRACE=1` 输出前几次实际命中的文件读取；启动日志的 `Mods: 12 game files replaced, 0 added` 给出替换与新增计数，应为替换却显示 0 时先检查 Mod 内目录结构。

合并与缓存：

- 合并树固定名为 `out\mod-game-<指纹前 16 位>`，指纹由游戏目录、启用的 Mod 层、每个文件的路径/大小/mtime 与 mods.py 自身算出：输入不变则直接复用（约 2 秒），有变化才重建；超过 24 小时未命中的旧目录会被自动清扫。
- Windows 下不要求管理员权限：目录用 junction、文件用硬链接，跨卷时回退为复制。

## 文件 Mod（Linux 版）

Linux 版附带 GTK launcher（`launcher/`），其中 Mods 组提供图形管理：选择目录、总开关、逐个启停与排序，设置写入同一份 `mods.json`。数据目录在 `~/.local/share/bbport`。与游戏目录相邻的 `CUSA03173-mods\dvdroot_ps4\` 会被自动挂载（总开关同样控制它）。

## 社区补丁 XML（必装）

启动流程把 shadPS4/GoldHEN 格式的补丁 XML 编译为 `out\patches.bin` 交给加载器。帧率、渲染分辨率与画面开关都经由这一通道：

- 放置位置：数据目录的 `patches\`（环境变量 `BB_PATCHES_DIR` 可改，`BB_PATCHES_CONFIG` 改选择配置路径）。
- 只取 `TitleID` 匹配本游戏、`AppVer="01.09"`、目标 `eboot.bin` 的 `Metadata`；`mask` 模式补丁与 eboot 之外的地址整条跳过并在日志说明。
- 必装的原因：bbport 的帧率预设（uncap/60/90）、渲染分辨率预设与 `bbport.ini` 的所有特效开关（色差、DoF、动态模糊、SSAO、AA、动态阴影、SSR、跳过开场、调试相机、调试菜单、模型 LOD）都按补丁名从 XML 里取内容，例如 `Uncap FPS++`、`Resolution Patch 1280x720 (16:9)`。缺补丁名时启动直接失败（`patches not found for app version 01.09`）。请从社区补丁仓库下载适用于 1.09 的 Bloodborne 补丁 XML 放入 `patches\`（README 的 Mods and patches 一节列了来源）。
- 第三方 XML 在内置选择之后应用，与内置补丁同名冲突时第三方胜出。
- 指针表类补丁（60 FPS++、90 FPS++）会自动重定位到实际镜像地址。
- 启停选择保存在数据目录 `patches.json`：`{"enabled": ["文件.xml/补丁名"], "disabled": [...]}`。
- `debug_menu` 需要游戏目录 `dvdroot_ps4\font\` 下的 `DbgFont14h.ccm/.tpf` 为非空文件（见 Nexus Mods Bloodborne 253 号 Mod），缺失时报错指出安装方法。

## TAA 锐化

`bbport.ini` 的 `sharpen` 与 `sharpness`（0…1）对 TAA 与 FSR 通用，RCAS 应用于 HUD 之前的成品场景；关闭或 0 时无额外 pass。TAA 历史缓冲保存锐化前颜色，避免逐帧增强。
