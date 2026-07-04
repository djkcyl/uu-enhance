# UU远程增强

给 [UU远程](https://uuyc.163.com/)（GameViewer）加官方没有的功能，主控、被控都有。DLL 代理（伪装 `version.dll`，和 ReShade 一个套路）：丢进安装目录生效，删掉恢复。

> 个人逆向研究，与网易无关，不涉及破解或绕过计费。风险自负。

## 功能

**主控**（右键托盘，按会话独立开关）

- 仅浏览 — 拦鼠标/键盘/手柄，含游戏里的指针锁定
- 剪贴板同步（默认关）
- 禁止手柄

**被控**（右键托盘「被控时仅浏览」，默认关）

本机被别人控制时，对方只能看画面、听声音，会改动本机的操作按类拦下：输入、终端、端口映射、文件、显示、虚拟屏、隐私屏/锁屏、麦克风、关机/重启/唤醒/自启、启动应用、文本注入。

## 安装

从 [Releases](https://github.com/djkcyl/uu-enhance/releases) 下载 `uu-enhance-installer-<版本>.exe`，双击安装/卸载，自动定位 GameViewer；DLL 被占用时会列出进程并可一键关闭/重启。也可手动把 `version.dll` 放进 `GameViewer\bin\`。

## 构建

Visual Studio 2022 + CMake：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物在 `build/Release/`：`version.dll` 和 `uu-enhance-installer-<版本>.exe`（DLL 已内嵌）。

## License

[MIT](LICENSE) · 依赖 [MinHook](https://github.com/TsudaKageyu/minhook)
