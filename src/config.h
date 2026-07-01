#pragma once
#include <atomic>
#include <string>

// 新会话的默认开关，从 ini 读；每个会话连上后可在托盘菜单单独改
namespace cfg {
    extern std::atomic<bool> g_viewOnly;     // 仅浏览（主控新会话默认）
    extern std::atomic<bool> g_clipSync;     // 剪贴板同步（主控新会话默认）
    extern std::atomic<bool> g_gamepadOff;   // 禁止手柄转发（主控新会话默认）
    extern std::atomic<bool> g_ctrlClip;     // 被控端剪贴板：允许控制方读/写本机剪贴板（全局，默认开）

    void load();   // 从 ini 读取(全局默认值)
    void save();

    std::wstring exe_version();   // 主程序(GameViewer.exe)版本号 "a.b.c.d"，取不到返回空
}
