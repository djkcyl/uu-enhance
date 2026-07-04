#pragma once
#include <vector>
#include <string>
#include <cstdint>

struct SessSnap { void* key; std::wstring name; bool viewOnly, clipSync, gamepadOff; };

std::vector<SessSnap> sessions_snapshot();
bool session_toggle(void* key, int field);

struct HookStat { std::string name; void* addr; std::string how; bool ok; };

struct DbgLine { const wchar_t* role; std::string name; std::string how; unsigned long long off; bool ok; };

struct DebugInfo {
    std::wstring gvVersion;
    bool gvKnown;
    uintptr_t devIdOff, vmwDevIdOff, vmwTitleOff;
    bool devIdAuto, vmwAuto;
    bool serverRunning;
    std::vector<DbgLine> hooks;
};
DebugInfo debug_snapshot();
