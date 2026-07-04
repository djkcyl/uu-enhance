#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace cfg {
    extern std::atomic<bool> g_viewOnly;
    extern std::atomic<bool> g_clipSync;
    extern std::atomic<bool> g_gamepadOff;
    extern std::atomic<bool> g_ctrlClip;
    extern std::atomic<bool> g_srvViewOnly;
    extern std::atomic<bool> g_autoUpdate;

    // Per-category blocking inside controlled view-only. Bit set = block that category.
    enum SrvFeat : uint32_t {
        SF_INPUT    = 1u << 0,
        SF_TERMINAL = 1u << 1,
        SF_PORTMAP  = 1u << 2,
        SF_FILE     = 1u << 3,
        SF_DISPLAY  = 1u << 4,
        SF_PRIVACY  = 1u << 5,
        SF_AUDIO    = 1u << 6,
        SF_POWER    = 1u << 7,
        SF_LAUNCH   = 1u << 8,
        SF_VDISPLAY = 1u << 9,
        SF_TEXT     = 1u << 10,
        SF_ALL      = (1u << 11) - 1,
    };
    extern std::atomic<uint32_t> g_srvBlockMask;

    inline bool srv_block(SrvFeat f) { return g_srvViewOnly.load() && (g_srvBlockMask.load() & f); }

    void load();
    void save();
    void refresh_srv_view_only();
    std::wstring config_path();

    std::wstring exe_version();
}
