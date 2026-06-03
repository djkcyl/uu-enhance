# UU远程增强

给 [UU远程](https://uuyc.163.com/)（GameViewer）主控端加了几个一直想要但官方没给的功能。

做这个的原因很简单——我平时主控多开好几个远程窗口，经常只想看画面不想误操作，但 UU 没有按窗口独立控制的开关。于是逆了一下 GameViewer 客户端，写了个 hook 补上。

实现方式是 DLL 代理（和 ReShade 一个套路）：一个伪装的 `version.dll` 丢进安装目录就生效，不动原程序，删掉就恢复。

> **免责**：个人学习和自用的逆向研究，跟网易没有任何关系。不涉及破解、绕过计费或被控端功能。用不用自己判断，风险自负。

## 功能

右键系统托盘图标，按会话独立开关（多个远程窗口互不影响）：

- **仅浏览** — 画面照看，鼠标键盘手柄全拦，连游戏里吞鼠标的指针锁定也挡掉。默认开。
- **剪贴板同步** — 主控和被控之间的剪贴板双向同步。默认关。
- **禁止手柄** — 单独切断手柄转发，切换时被控端会正确断开/重连。

托盘菜单底部有个「调试信息」，能看到当前 GameViewer 版本、各 hook 点的定位方式和状态，排查问题的时候不用开 DebugView。

## 安装

### 用安装器（推荐）

从 [Releases](https://github.com/djkcyl/uu-enhance/releases) 下载 `uu-enhance-installer.exe`，双击就行：

1. 会自动找到 GameViewer 的安装位置
2. 点「一键安装」
3. 打开 GameViewer，托盘多出来一个图标就 OK 了

卸载也是同一个界面，点「卸载」会删掉补丁和配置文件。

> 安装/卸载前要先退出 GameViewer（包括托盘图标），不然 DLL 被占用写不进去。

### 手动

把 `version.dll` 复制到 `C:\Program Files\Netease\GameViewer\bin\`，跟 `GameViewer.exe` 放一起就行。删掉即卸载。

## 配置

DLL 同目录下的 `uu-enhance.ini`，改的是新会话连上时的默认值（连上之后可以在托盘单独改）：

```ini
[general]
view_only      = 1
clipboard_sync = 0
gamepad_off    = 0
```

## 原理

详细的逆向分析写在 [docs/TECHNICAL.md](docs/TECHNICAL.md)，这里说个大概：

- `version.dll` 不在 Windows 的 KnownDLLs 里，放进 bin 目录会被优先加载，17 个标准导出全部转发给系统真正的 version.dll
- 用 MinHook 做 inline hook，拦截输入发送、剪贴板同步、手柄转发等函数
- 为了抗更新，已知版本直接用精确地址，未知版本靠日志字符串 + `.pdata` 异常表定位函数，再不行用字节特征扫，都找不到就跳过（不会崩）

## 兼容性

- Windows x64
- 逆向基线是 GameViewer 4.26.0.8259，其他版本大多数情况能自动适配
- 个别 hook 挂不上的话托盘调试信息里能看到，对着 [TECHNICAL.md](docs/TECHNICAL.md) 的地址表更新 `src/offsets.h` 重新编一下就好

## 构建

Visual Studio 2022 + CMake，MinHook 已经在 `vendor/minhook/` 里了：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`build/Release/` 下会同时生成：

- `version.dll` — 补丁本体
- `uu-enhance-installer.exe` — 一键安装器（DLL 已经整个打包在里面了）

## 致谢

- [MinHook](https://github.com/TsudaKageyu/minhook)

## License

[MIT](LICENSE)
