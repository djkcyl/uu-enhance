# UU远程增强 开发指南

## 构建

需要 Visual Studio 2022（C++ 桌面开发 + CMake）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物在 `build/Release/`：
- `version.dll` — 补丁本体
- `uu-enhance-installer.exe` — 一键安装器（内嵌完整 DLL）

**注意**：源码含 UTF-8 中文注释，CMakeLists.txt 里的 `/utf-8` 不能去掉，否则 MSVC 按 936 码页误读会编译失败。`src/app.h` 只能写 ASCII（rc.exe 用系统 ANSI 码页读它），中文显示名放在 `tray.cpp` 和 `installer.cpp` 里。

## 版本号

版本号**唯一定义**在 `src/app.h`，所有地方从这里取：

```c
#define UURE_VERSION     "0.1.0"        // C 字符串
#define UURE_VERSION_W   L"0.1.0"       // 宽字符串
#define UURE_VERSION_RC  0,1,0,0        // VERSIONINFO 资源用的四段逗号格式
```

改版本号**只改这一个文件**，DLL（app.rc）和安装器（installer.rc）的 VERSIONINFO 都从这里 include。

GitHub 仓库地址也在这里（`UURE_GITHUB` / `UURE_GITHUB_W`），安装器的更新检查和托盘菜单的"项目主页"链接都用它。

## 发布流程

1. **改版本号**：编辑 `src/app.h`，同时改 `UURE_VERSION`、`UURE_VERSION_W`、`UURE_VERSION_RC` 三个宏。
2. **构建**：`cmake --build build --config Release`。确认 `build/Release/` 下 DLL 和安装器都更新了。
3. **测试**：把产出的 `version.dll` 丢进 GameViewer 的 `bin\` 目录，启动 GameViewer，确认托盘图标出现、版本号正确、各功能正常。用安装器测一遍安装/卸载/更新流程。
4. **提交 + 打 tag**：
   ```powershell
   git add -A
   git commit -m "release: v0.2.0"
   git tag v0.2.0
   git push origin main --tags
   ```
   tag 格式固定为 `vX.Y.Z`，安装器的更新检查从 GitHub Release 的 `tag_name` 取这个值，去掉 `v` 前缀后跟本地版本比较。
5. **创建 GitHub Release**（只建空壳，产物交给 Action）：
   ```powershell
   gh release create v0.2.0 --title "v0.2.0" --notes "改动说明"
   ```
   发布后 `.github/workflows/release.yml` 会自动在 windows-latest 上构建 Release，并把 `uu-enhance-installer.exe` 和 `version.dll` 传回这个 Release。**不用再本地附加附件**。
   - workflow 会先校验 tag（去 `v`）等于 `app.h` 的 `UURE_VERSION`，不一致直接失败——防止更新检查对不上。
   - 想手动重跑：Actions 页面选 `build-release` → Run workflow，填已存在的 tag。
   - 产物里 `uu-enhance-installer.exe` 是用户下载的主体（DLL 已内嵌），`version.dll` 附上方便手动安装的用户。

## 安装器更新检查

安装器启动时在后台线程用 WinHTTP 请求 `https://api.github.com/repos/djkcyl/uu-enhance/releases/latest`，从返回的 JSON 里取 `tag_name` 字段，去掉 `v` 前缀后和内嵌的 `UURE_VERSION` 比较。如果 Release 版本更新，在底部链接栏显示"新版本 vX.Y.Z 可用"并指向 Release 页面。请求失败（没网、API 限流、仓库不存在）静默忽略。

所以**发了 Release 就等于推送了更新通知**，不需要额外的更新服务器。

## 项目结构

```
src/           补丁本体
  app.h        版本号、GitHub URL（唯一定义处，ASCII only）
  app.rc       DLL 的 VERSIONINFO 资源
  dllmain.cpp  入口：代理转发 + 后台线程
  proxy.cpp    version.dll 17 个导出转发
  hooks.cpp    功能 hook + 按版本分流定位 + 会话管理
  resolver.cpp 抗更新定位器（字符串 + .pdata + AOB）
  tray.cpp     系统托盘菜单
  config.cpp   ini 读写
  offsets.h    各函数 RVA 表（已知版本）

installer/     一键安装器
  installer.cpp  GUI + 自动查找 + 释放/卸载 + 更新检查
  installer.rc   内嵌 version.dll + manifest + VERSIONINFO
  resource.h     资源和控件 ID
  installer.manifest  管理员权限 + 现代控件 + 高 DPI

vendor/minhook/ MinHook 源码
docs/TECHNICAL.md 逆向分析文档
```

## 适配 GameViewer 新版本

如果用户报告某个 hook 挂不上（托盘调试信息里显示"未定位"），需要：

1. 拿到新版 GameViewer.exe，用 IDA 打开
2. 按 `docs/TECHNICAL.md` 里的地址表，用日志字符串 xref 重新定位各函数
3. 在 `src/offsets.h` 的 `kVer[]` 最前面加一行新版本（新 RVA + 新 AOB），新版放最前是因为 `pick()` 对未知版本兜底用 `kVer[0]`
4. 确认 CCS 的 `device_id` 偏移（`VerSet::deviceIdOff`，十进制）有没有变：反编译 `setConnectInfo`，看 "startConnectOtherDevice, device_id: " 那行日志把 `this + N` 传给 ostream<<string，`N` 就是偏移。4.26=3984、4.29=4296，逐版本会变，必须核对
5. 重新构建、测试、发版

多数情况下字符串定位（resolver.cpp）会自动对上，不用改代码。加已知版本条目只是让它走精确 RVA 快路径、并把 `deviceIdOff` 更新到最新基线。只有 UU 改了函数名/日志内容/二进制结构时才必须手动更新。
