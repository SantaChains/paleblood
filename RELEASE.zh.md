# 发布包使用说明（Windows）

这个 zip 不含源码，也不需要 MSYS2 或编译器。解压到任意目录即可。

## 开始（三步）

1. **解压** 本 zip 到一个有写权限的目录（它会在 `out/` 下生成派生文件与缓存）。
2. **放置补丁数据库**：把 Bloodborne 1.09 的社区补丁集 `Bloodborne.xml` 放进
   `patches/`（没有就新建该目录）。本 zip 不附带它：那是社区作者的作品且上游无
   许可证，不能由我们再分发。来源见 `tools/fetch_patches.sh` 头部注释与运行错误提示。
   启动时缺它会得到一条明确错误，照做即可。
3. **双击 `run.bat`**。首次会询问游戏 dump 的路径（包含 `eboot.bin` 的目录），之后
   记住。启动链会在本机从你的 dump 派生 boot 镜像、补丁与内容档（数十秒），
   然后打开游戏窗口。

## 可选：DLSS（NVIDIA RTX）

`nvngx_dlss.dll` 受 NVIDIA 许可限制不能随包分发。RTX 用户运行 `tools/fetch_dlss.sh`
或自行下载后放到 `out/`（与 `bb-probe.exe` 同目录），再在游戏内菜单或 `bbport.ini`
选择 DLSS。没有它时自动回退 FSR 3.1，其余功能不受影响。

## 可选：FSR 4（AMD RDNA4）

`fsr4_shaders/` 已随包（MIT 许可的模型数据）。若你的驱动支持 FSR 4.1.1，还需要
`tools/fetch_fsr4_assets.sh` 拉取的 4.1.1 着色器放到 `fsr4_411/`——不装则 FSR 4
（非 4.1.1）可用。

## 文件去向

| 内容 | 位置 |
|---|---|
| 你的设置 | `bbport.ini`、`bbport_ui.ini` |
| 自定义调色风格 | `user-presets.json` |
| 存档 | `user\` |
| 启动派生文件与缓存 | `out\` |

## 排错

- 启动即报补丁数据库缺失：见上面第 2 步；错误信息会说明缺什么。
- `SDL3.dll` 找不到：它应在 `out\`，与 `bb-probe.exe` 同目录。
- 画面问题先试游戏内菜单的预设与超分切换（Insert 或 L3+R3 打开，设置即时生效，
  关闭菜单时保存）。
- 更多键位与环境变量见 `README.zh.md`（完整文档）。
