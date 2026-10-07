# bbport 发布包使用说明（Windows）

Bloodborne 1.09 的 PC 原生移植运行器。这个包**不含源码，也不需要 MSYS2 或编译器**——
解压、放置补丁数据库、指向你的游戏 dump，即可运行。

系统要求：64 位 Windows 10 1803+；支持 Vulkan 1.2 的显卡驱动；任意 Python 3.8+
（Windows 10/11 一般已自带 `py` 启动器；没有就装一次 python.org 的版本）。

---

## 一、开始（三步）

1. **解压** 到一个有写权限的目录（启动时会在 `out\` 下生成派生文件与缓存；
   不要放在 `C:\Program Files` 或只读网盘目录）。

2. **放置补丁数据库**：把 Bloodborne 1.09 的社区补丁集 `Bloodborne.xml` 放进
   `patches\`（没有就新建）。**本包不附带它**：那是社区作者的作品且上游无许可证，
   不能由我们再分发。来源（二选一）：
   - `tools\fetch_patches.sh`（需任意 bash，如 Git Bash；会校验内容并拒绝装错版本）
   - 手动下载 shadPS4 社区补丁仓库的 `PATCHES/Bloodborne.xml`，
     见 `tools\fetch_patches.sh` 头部注释。
   缺它会得到一条明确错误，按提示做即可。**必须是对应 1.09**（`Metadata AppVer="01.09"`）。

3. **双击 `run.bat`**。首次会询问游戏 dump 路径（含 `eboot.bin` 的目录），之后记住。
   启动链在本机从你的 dump 派生 boot 镜像、补丁与内容档（首次约 1 分钟，之后秒过），
   然后打开游戏窗口。

游戏内按 **Insert**（或 **L3+R3**）打开设置菜单；**F11** 切换全屏。

---

## 二、显卡特性（按需选装）

| 特性 | 适用显卡 | 需要做什么 |
|---|---|---|
| FSR 3.1（默认回退） | 全部 | 什么也不用做 |
| **DLSS**（画质最佳） | NVIDIA RTX | `nvngx_dlss.dll` 放到 `out\`（NVIDIA 许可限制，不随包；用 `tools\fetch_dlss.sh` 或官网下载），再在菜单选 DLSS |
| **FSR 4**（INT8） | AMD RDNA4 / 支持驱动 | 已随包（`fsr4_shaders\`，MIT 许可） |
| **FSR 4.1.1** | 支持 `shaderMixedFloatDotProduct` 的驱动 | 另需 `fsr4_411\` 着色器：运行 `tools\fetch_fsr4_assets.sh` |

不装 DLSS DLL 时自动回退 FSR 3.1，其余功能不受影响。

---

## 三、游戏内菜单

- **Insert / L3+R3** 开关；**Esc** 关闭。
- 页面：画面（超分/预设/锐度）、显示（分辨率/全屏/HDR）、游戏效果（各渲染效果开关）、
  作弊（GoldHEN 格式 JSON，按游戏序列号放 `cheats\` 目录）、高级（帧率上限、
  显存预算、低延迟、键位交换等）。
- 调色可存为具名风格槽位（用户风格保存在 `user-presets.json`）。
- 窗口几何、折叠区、选中的页都会记住；设置在关闭菜单时保存（`bbport.ini`）。
- 标注"重启生效"的项（分辨率预设、效果开关）会在菜单里提示"应用并重启游戏"。

---

## 四、常见问题

- **提示补丁数据库缺失**：见"一、第 2 步"。
- **启动后黑屏很久**：首次派生与着色器编译需要时间；看控制台输出的进度行。
- **"Upscaler: dlss unavailable; falling back to FSR 3.1"**：没放 `nvngx_dlss.dll`
  或非 RTX 显卡——见第二节。
- **显存不足/频繁卡顿**：菜单高级页调 `gc_budget_mb`（0 = 自动跟随驱动实时预算）。
- **手柄布局反了**：`bbport.ini` 的 `pad_swap=1`。
- **中文显示为 ?**：Windows 缺系统 CJK 字体（msyh/simhei/Deng 任一即可）。

完整文档（全部环境变量、ini 键、构建方法）见 `README.zh.md`。

---

## 五、文件去向与更新

| 内容 | 位置 |
|---|---|
| 设置 | `bbport.ini`、`bbport_ui.ini`、`user-presets.json` |
| 存档 | `user\` |
| 派生文件、着色器缓存、模组 overlay | `out\` |
| 补丁数据库 | `patches\Bloodborne.xml` |

**更新版本**：用新 zip 覆盖时保留 `bbport.ini`、`bbport_ui.ini`、`user-presets.json`、
`user\`、`patches\` 即可，其余文件可全部替换。若提示补丁校验失败，说明新版本
需要的补丁条目有变化——重新获取社区补丁集。

## 六、许可

本移植与所有第三方组件的许可清单见 `THIRD-PARTY-LICENSES.md`。bbport 以
GPL-2.0-or-later 发布，源码见仓库。
