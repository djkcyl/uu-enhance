#pragma once
#include <vector>
#include <string>
#include <cstdint>

// 会话快照(供托盘菜单)。key = ControlConnectionSession*
struct SessSnap { void* key; std::wstring name; bool viewOnly, clipSync, gamepadOff; };

std::vector<SessSnap> sessions_snapshot();
// field: 0=仅浏览 1=剪贴板同步 2=禁止手柄；返回切换后的值
bool session_toggle(void* key, int field);

// 单个 hook 点的定位结果。how: rva/str/aob，没挂上为空。
struct HookStat { std::string name; void* addr; std::string how; bool ok; };
// 调试面板(供托盘“调试信息”子菜单)
struct DebugInfo {
    std::wstring gvVersion;     // GameViewer.exe 版本，取不到为 "?"
    bool gvKnown;              // 版本号是否在 offsets 表里
    uintptr_t gvBase;         // GameViewer.exe 模块基址，算 RVA 用
    std::vector<HookStat> hooks;
};
DebugInfo debug_snapshot();
