#include "update.h"
#include <winhttp.h>
#include <string>
#include <mutex>
#include "app.h"
#include "config.h"
#include "log.h"

namespace {
std::mutex   g_mtx;
std::wstring g_latest;
bool         g_avail = false;
std::wstring g_notifiedVer;
HWND         g_wnd = nullptr;
UINT         g_msg = 0;

int cmpVer(const std::wstring& a, const std::wstring& b) {
    auto parse = [](const std::wstring& s, int o[4]) {
        o[0] = o[1] = o[2] = o[3] = 0;
        swscanf_s(s.c_str(), L"%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3]);
    };
    int va[4], vb[4];
    parse(a, va); parse(b, vb);
    for (int i = 0; i < 4; ++i) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return  1;
    }
    return 0;
}

bool fetch_latest_tag(std::wstring& out) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"uu-enhance/" UURE_VERSION_W,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
    HINTERNET con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (con) {
        HINTERNET req = WinHttpOpenRequest(con, L"GET",
            L"/repos/djkcyl/uu-enhance/releases/latest",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req) {
            if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                std::string body;
                char buf[4096]; DWORD n = 0;
                while (WinHttpReadData(req, buf, sizeof(buf), &n) && n > 0) { body.append(buf, n); n = 0; }
                auto pos = body.find("\"tag_name\"");
                if (pos != std::string::npos && (pos = body.find('"', pos + 10)) != std::string::npos) {
                    auto end = body.find('"', pos + 1);
                    if (end != std::string::npos) {
                        std::string tag(body, pos + 1, end - pos - 1);
                        if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
                        out.assign(tag.begin(), tag.end());
                        ok = !out.empty();
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

void do_check() {
    std::wstring tag;
    if (!fetch_latest_tag(tag)) { uu_log("update: check failed (offline/limited)"); return; }
    bool newer = cmpVer(tag, UURE_VERSION_W) > 0;
    HWND wnd = nullptr; UINT msg = 0; bool notify = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_avail  = newer;
        g_latest = newer ? tag : L"";
        wnd = g_wnd; msg = g_msg;
        if (newer && tag != g_notifiedVer) { g_notifiedVer = tag; notify = true; }
    }
    uu_log("update: local=%ls latest=%ls newer=%d", UURE_VERSION_W, tag.c_str(), (int)newer);
    if (notify && wnd && msg) PostMessageW(wnd, msg, 1, 0);
}

DWORD WINAPI oneshot(LPVOID) { do_check(); return 0; }

DWORD WINAPI worker(LPVOID) {
    Sleep(8000);
    for (;;) {
        if (cfg::g_autoUpdate.load()) do_check();
        Sleep(60u * 60u * 1000u);
    }
}
} // namespace

namespace update {

void start(HWND wnd, UINT msg) {
    { std::lock_guard<std::mutex> lk(g_mtx); g_wnd = wnd; g_msg = msg; }
    CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
}

void check_async() { CreateThread(nullptr, 0, oneshot, nullptr, 0, nullptr); }

bool available() { std::lock_guard<std::mutex> lk(g_mtx); return g_avail; }

std::wstring latest_version() { std::lock_guard<std::mutex> lk(g_mtx); return g_latest; }

}
