#pragma once
#include <windows.h>
#include <string>

namespace update {
    // 发现新版时 PostMessage(wnd, msg, 1, 0)，同一版本只通知一次。
    void start(HWND wnd, UINT msg);
    void check_async();
    bool available();
    std::wstring latest_version();
}
