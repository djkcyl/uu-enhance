# 技术说明

记录 UU远程（GameViewer）主控端的逆向分析结果和这个补丁的实现思路。基线版本是 GameViewer.exe 4.26.0.8259，Qt 5.15 + MSVC 编译的 x64 程序，没加壳，imagebase `0x140000000`；音频相关的 streamer.dll imagebase 是 `0x180000000`。

下文写的地址都是 RVA（IDA 里的地址减去 imagebase）。补丁运行时拿到模块真实基址再加上 RVA。

## 目标程序

`C:\Program Files\Netease\GameViewer\bin\` 里有几个主要模块：

- GameViewer.exe，主客户端，主控这边的逻辑都在这里，是补丁主要动的对象。
- GameViewerServer.exe，被控端服务，补丁不碰。
- streamer.dll，串流核心，基于 WebRTC，音频在这里。

这个 build 是带调试信息的，PDB 路径泄露了源码结构（`repo_remote/src/uu_guest/...`、`.../modules/coreapp/control/...`），更有用的是很多函数名以日志字符串的形式被编进了二进制里——定位函数基本都靠这些字符串。

主控会话的核心类是 `ControlConnectionSession`（下面简称 CCS），在 `control_connection_session.cpp`。一次远程连接对应一个 CCS 实例，补丁就是用 CCS 来区分不同会话的。

## 怎么找函数

先把二进制里的字符串（ASCII 和 UTF-16）连同文件偏移导出来，搜函数名之类的独特串（比如 `ControlConnectionSession::sendMouseEvent`），看哪个函数引用了它，那个就是目标。然后在反汇编器里看伪代码确认参数和结构。这些都是一次性的分析工作，结果固化在 `src/offsets.h` 和下面的地址表里。

## 注入：伪装 version.dll

Windows 加载非 KnownDLL 时，优先在程序自己的目录找。`version.dll` 不在 KnownDLLs 列表里，GameViewer 和 Qt 都会用到它，所以把一个同名 dll 放进 `bin\` 就会被优先加载，这是 ReShade 一类工具的常见做法。

`src/proxy.cpp` 把 `version.dll` 的 17 个标准导出都导出来，运行时 LoadLibrary 系统真正的 `version.dll`，把调用转发过去（链接时用 `/EXPORT:名字=my_名字` 重定向）。这样宿主程序对 version.dll 的依赖照常工作。`DllMain` 里先把转发准备好，再起一个后台线程装 hook 和托盘。

## Hook 和抗更新

用 MinHook 做 inline hook。地址怎么定位是抗更新的关键。先读 GameViewer.exe 的版本号，再分两种情况（这段分流在 `hooks.cpp` 的 `mk`，具体查找方法在 `resolver.cpp`）：

版本号在表里（认识这个 build）：直接用 offsets.h 里我核对过的精确 RVA——它对这个 build 就是对的，没有比它更准的。同时还是会跑一遍字符串定位做交叉校验，结果跟 RVA 对不上就记一条日志（要么定位器有 bug，要么是同版本号不同 build），但仍以 RVA 为准。

版本号不认识（多半是更新过了）：写死的 RVA 是别的 build 的，**作废不用**——套到新二进制上就是个错地址，hook 下去会崩。改用下面两种与版本无关的办法，都定不到就跳过这个 hook（只记日志，不崩）：

1. 多锚点字符串 + `.pdata`。启动时先扫一遍 `.text`，把所有"指向 `.rdata` 的 RIP 相对 lea"建成索引：被引用的字符串地址 → 它所在的函数入口。函数入口用 `.pdata`（异常展开表 RUNTIME_FUNCTION 数组，系统栈回溯用的）二分查出来。每个目标函数配 2 到 3 个该函数体内唯一的字符串（函数名加几条独特日志），定位时它们各自投票，得票最多的函数胜出。这样单个字符串被改名或删掉不会一下子失效，剩下的锚还能定位；lambda 偶尔引用了同名字符串也不会误判，因为主函数被引用得更多。
2. 字节特征（AOB）。字符串也找不到时，用函数开头一段字节模式去 `.text` 里扫，相对地址那几个字节用通配符忽略，要求唯一匹配。

要点是：精确 RVA 只在"运行的就是它对应的那个 build"时才成立，它不是一种更弱的兜底，而是已知版本上的快捷方式；字符串和 AOB 直接读当前二进制、与版本无关，才是更新后还能自动对上的依靠。补丁加载时会把版本号和 `known=0/1` 打到日志里。

调用约定上 x64 统一是 caller 清栈，所以那些"满足条件就提前返回"的拦截 hook，哪怕按比原函数多的形参声明再转发也是安全的，多出来的寄存器/栈位原函数不读。

剩下唯一一个版本相关的结构体偏移是 CCS 里 device_id 那个字段（`+0x3984`），只用来去重和当回退显示名，读取时有 SEH 保护，读错了既不影响按 CCS 指针区分会话，也不会崩。

## 按会话区分

需求是每个窗口独立控制。难点在于各个 hook 拿到的 this 不一样：输入发送函数的第一个参数就是 CCS，所以仅浏览天然就能按会话分。剪贴板、手柄、光标这些函数拿到的是各自的对象（Clipboard、GamepadManager、VideoWidget），但它们本质上都是绑当前焦点会话的——本地就一份系统剪贴板、一个物理手柄、一个鼠标光标——所以按"当前活动会话"（最近有输入事件的那个 CCS）来判断是对的。

`src/hooks.cpp` 用一个 `map<CCS*, 状态>` 存每个会话的开关，新会话的默认值取自配置。

会话名取自被控设备名。CCS 里只有 device_id（一长串），设备名其实在 UU 账号的设备库里、按 id 索引，从 CCS 里直接拿不到。所以补丁退而求其次：UU 把视频窗口标题设成了设备名，用户操作某个会话时前台窗口必然是它，于是在输入 hook 里读一下前台窗口标题当作会话名，每秒最多读一次、持续跟随，偶尔读错或者设备改了名都会被下次操作纠正。

### 会话生命周期

| 时机 | 函数 | RVA | 处理 |
|---|---|---|---|
| 连接 | CCS::setConnectInfo | `0x867ce0` | 注册会话、置为活动、按 device_id 去重 |
| 断开 | CCS::closeControlConnect | `0x83a3f0` | 移除会话 |
| 退出 | CCS::exitRoom | `0x841b90` | 移除会话 |

这里有个坑：UU 断开连接时并不销毁 CCS 对象（留着给标签页和重连用），所以不能靠 hook 析构来移除会话，那样永远不会触发，旧会话会一直留在菜单里，连了新设备就变成两个。必须 hook 上面这几个关闭/退出的入口。

## 各功能

### 仅浏览：输入发送

主控发往远端的输入都经过 CCS 的几个发送函数，底层再走 streamer 的 SendControlData。这几个函数是通过一层包装类的虚表跳板调用的（跳板里偏移 112 是 CCS），上面没有更统一的单一入口，所以就在发送函数入口拦：

| 函数 | RVA |
|---|---|
| CCS::sendMouseEvent | `0x862080` |
| CCS::sendMouseWheel | `0x862bc0` |
| CCS::sendKeyboardEvent | `0x860650` |

仅浏览时直接返回、不发。`sendControlDataJson`（`0x860180`）是通用控制通道，里面还夹带切显示器、改画质这种"看"的时候也要用的操作，所以不拦。输入法文本（`sendInputMethodInfo`，`0x792e70`）严格说也算输入，目前没拦。

### 仅浏览：锁鼠标

`VideoWidget::enabledCaptureMouse(this, enable, ...)`（`0x6639e0`），enable 为真时用 GetWindowRect、ClipCursor、SetCursorPos 把鼠标锁进窗口并隐藏，这就是游戏里鼠标被吸走的来源；为假时 ClipCursor(0) 释放。仅浏览时强制把 enable 当 0，永远不锁，并释放已有的锁定。

### 仅浏览：光标样式

`VideoWidget::updateCursor(this, force)`（`0x66dba0`）是光标逻辑的中枢，远端光标样式（带 DPI 缩放）就是在这里应用到本地的。仅浏览时直接 SetCursor 成系统的禁用图标（IDC_NO）然后返回，既不应用远端光标也给个明确的"不可操作"反馈。

### 仅浏览 / 禁止手柄

`GamepadManager`（`gamepad_client.cpp`）轮询本地手柄转发到远端，XBox 和 PS5 共用。三个动作都通过 `*(this+6648)` 走 sendControlDataJson 发 `gpd_*` 消息。（原函数内部还有个 `*(BYTE*)this` 的启用标志，由 setSupportGamepad 设，补丁没用到它，自己用下面的 `active_gpBlock` 判断。）

| 函数 | RVA |
|---|---|
| GamepadManager::Connect | `0x9ea770` |
| GamepadManager::Disconnect | `0x9eac20` |
| GamepadManager::Update | `0x9eeeb0` |

仅浏览或禁止手柄时，Connect 和 Update 不发，被控端就看不到这个手柄。切换时还要处理已经连上的手柄：补丁记着已连的手柄索引，切到拦截状态时对每个调原始的 Disconnect（让被控端拔掉），切回时调原始的 Connect 重连，整个过程用一把锁和手柄线程串起来，避免竞争。

### 剪贴板

主控这边的剪贴板引擎是 `Clipboard`（`clipboard.cpp`）。

| 方向 | 函数 | RVA |
|---|---|---|
| 出（本地到远端） | Clipboard::on_clipboard_update | `0x8c5280` |
| 入（远端到本地） | Clipboard::handle_clipboard_request | `0x8bf810` |

`on_clipboard_update` 监听本地剪贴板变化往远端推；`handle_clipboard_request` 是个 switch 分发器，按消息类型路由格式表、数据块、文件传输等所有剪贴板协议消息。把这两个拦了就是双向都断。

## 没采用的两条路

### 每窗口音量

UI 上的音量滑块走的是 `Utils::setCurProcessVolume`（`0x892ee0`），最终是 ISimpleAudioVolume::SetMasterVolume，改的是整个进程的音量（作用域字面量就是 "global"），所以所有会话音量是连在一起的。

真正能分窗口的办法在 streamer.dll：它是 WebRTC，每一路远程音频是一个 ChannelReceive，`ChannelReceive::GetAudioFrameWithInfo`（streamer 里 `0x30227a`）里 `*(float*)(this+0x398)` 就是这一路的输出增益，WebRTC 在 AudioMixerImpl::Mix 混音之前逐路带饱和地应用它。hook 这个函数、给每个 this 写各自的增益就能实现逐窗口音量。这个功能实现过，后来按需求去掉了，定位留在这里备查。

### 改 UU 自己的菜单

会话内那个"控制中心"是原生 Qt 菜单（不是网页，网页只用在充值、应用中心那些子页面）。菜单结构在磁盘上的 `bin/txt/new_menu.xml`，文字在 `bin/lang/zh_CN/gdstrings.ini`，`VideoUi::showVideoMenu`（`0x65c4b0`）读 XML 再调一组 loadXxxConfig 动态建模型，点击之后通过 Qt 元对象按方法名分发到 NewMenuMethodObject 的槽。问题是要加一个能响应点击的新菜单项，方法名必须在 Qt 的元对象表里注册，而那个表每次重编译都会变，最不抗更新。所以最后没去改它的菜单，自己做了个托盘菜单，不依赖 UU 内部，更新也不怕。

托盘菜单除了按会话列开关，底部还有补丁自己的版本号、一个打开 GitHub 的"项目主页"链接，和一个"调试信息"子菜单。调试信息里列出当前 GameViewer 版本、版本号认不认识，以及每个 hook 点是用哪种方式定到的（精确地址 rva / 日志字符串 str / 字节特征 aob）、有没有挂上——就是 install_hooks 时每次 `mk()` 记下来的结果，排错时不开 DebugView 也能直接看。

## 地址表（GameViewer.exe，imagebase 0x140000000）

| 符号 | RVA | 用途 |
|---|---|---|
| CCS::sendMouseEvent | `0x862080` | 仅浏览 |
| CCS::sendMouseWheel | `0x862bc0` | 仅浏览 |
| CCS::sendKeyboardEvent | `0x860650` | 仅浏览 |
| VideoWidget::enabledCaptureMouse | `0x6639e0` | 防锁鼠标 |
| VideoWidget::updateCursor | `0x66dba0` | 光标禁用、防同步 |
| GamepadManager::Connect | `0x9ea770` | 手柄 |
| GamepadManager::Disconnect | `0x9eac20` | 手柄 |
| GamepadManager::Update | `0x9eeeb0` | 手柄 |
| Clipboard::on_clipboard_update | `0x8c5280` | 剪贴板出 |
| Clipboard::handle_clipboard_request | `0x8bf810` | 剪贴板入 |
| CCS::setConnectInfo | `0x867ce0` | 会话注册 |
| CCS::closeControlConnect | `0x83a3f0` | 会话移除 |
| CCS::exitRoom | `0x841b90` | 会话移除 |
| CCS device_id 偏移 | `+0x3984` | 去重、回退名 |
| Utils::setCurProcessVolume | `0x892ee0` | 音量（未用） |
| VideoUi::showVideoMenu | `0x65c4b0` | 菜单（未用） |
| streamer ChannelReceive::GetAudioFrameWithInfo | `0x30227a` | 音量（未用，imagebase 0x180000000） |

## 适配新版本

如果某次更新后日志里出现某个函数定位失败，需要在新版的 GameViewer.exe 里按这张表的字符串重新找一遍函数、更新 `src/offsets.h` 里的 RVA（顺便确认 device_id 的偏移有没有变），重新编译。多数情况下字符串定位会自动对上，不用手动改。
