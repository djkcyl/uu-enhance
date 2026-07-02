#include "resolver.h"
#include "log.h"
#include <cstring>
#include <vector>
#include <unordered_map>

namespace resolver {

bool get_ranges(HMODULE mod, ModRange& out) {
    auto base = (uint8_t*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    out = {};
    out.img_beg = (uintptr_t)base;
    out.img_end = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uintptr_t b = (uintptr_t)base + sec[i].VirtualAddress;
        uintptr_t e = b + sec[i].Misc.VirtualSize;
        const char* nm = (const char*)sec[i].Name;
        if (!std::strncmp(nm, ".text", 5))       { out.text_beg = b;  out.text_end = e; }
        else if (!std::strncmp(nm, ".rdata", 6)) { out.rdata_beg = b; out.rdata_end = e; }
        else if (!std::strncmp(nm, ".pdata", 6)) { out.pdata_beg = b; out.pdata_end = e; }
    }
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.VirtualAddress && dir.Size) {
        out.pdata_beg = (uintptr_t)base + dir.VirtualAddress;
        out.pdata_end = out.pdata_beg + dir.Size;
    }
    return out.text_beg && out.rdata_beg;
}

// .pdata(RUNTIME_FUNCTION[]，按 BeginAddress 升序) 把代码地址确定性映射到函数入口
static uintptr_t func_start_via_pdata(const ModRange& r, uintptr_t code_ea) {
    if (!r.pdata_beg) return 0;
    uint32_t rva = (uint32_t)(code_ea - r.img_beg);
    auto* tbl = (RUNTIME_FUNCTION*)r.pdata_beg;
    size_t n = (r.pdata_end - r.pdata_beg) / sizeof(RUNTIME_FUNCTION);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < tbl[mid].BeginAddress) hi = mid;
        else if (rva >= tbl[mid].EndAddress) lo = mid + 1;
        else return r.img_beg + tbl[mid].BeginAddress;
    }
    return 0;
}

// 一次性索引：.text 中所有"指向 .rdata 的 RIP-lea" → 其所属函数入口。
// 键=被引用字符串地址(VA)，值=该地址被各函数引用的 lea 次数。
static uintptr_t g_indexedBase = 0;
static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t, int>> g_idx;

static void build_index(const ModRange& r) {
    if (g_indexedBase == r.img_beg) return;
    g_idx.clear();
    auto t0 = (uint8_t*)r.text_beg, t1 = (uint8_t*)r.text_end;
    for (uint8_t* p = t0; p + 7 <= t1; ++p) {
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
            int32_t disp = *(int32_t*)(p + 3);
            uintptr_t tgt = (uintptr_t)(p + 7) + disp;
            if (tgt >= r.rdata_beg && tgt < r.rdata_end) {
                uintptr_t f = func_start_via_pdata(r, (uintptr_t)p);
                if (f) g_idx[tgt][f]++;
            }
        }
    }
    g_indexedBase = r.img_beg;
    uu_log("resolver index built: %zu string-refs", g_idx.size());
}

// 找完整字符串的所有出现位置(.rdata)。要求匹配后紧跟 \0，避免把某串当成更长串的前缀重复计票。
static void find_str_addrs(const ModRange& r, const char* s, std::vector<uintptr_t>& out) {
    size_t n = std::strlen(s);
    if (!n) return;
    uint8_t first = (uint8_t)s[0];
    for (uint8_t* p = (uint8_t*)r.rdata_beg; p + n < (uint8_t*)r.rdata_end; ++p)
        if (*p == first && p[n] == 0 && !std::memcmp(p, s, n)) {
            out.push_back((uintptr_t)p);
            if (out.size() >= 64) return;
        }
}

uintptr_t find_func(const ModRange& r, std::initializer_list<const char*> anchors) {
    build_index(r);
    std::unordered_map<uintptr_t, int> votes;  // func -> 命中的锚点数
    std::unordered_map<uintptr_t, int> leas;   // func -> 总 lea 次数(用于平票)
    for (const char* a : anchors) {
        std::vector<uintptr_t> sva;
        find_str_addrs(r, a, sva);
        std::unordered_map<uintptr_t, int> thisAnchor;  // 本锚点命中的函数
        for (uintptr_t s : sva) {
            auto it = g_idx.find(s);
            if (it == g_idx.end()) continue;
            for (auto& fc : it->second) { thisAnchor[fc.first] += fc.second; }
        }
        for (auto& fc : thisAnchor) { votes[fc.first]++; leas[fc.first] += fc.second; }
    }
    uintptr_t best = 0; int bv = 0, bl = 0;
    for (auto& v : votes) {
        int lc = leas[v.first];
        if (v.second > bv || (v.second == bv && lc > bl)) { best = v.first; bv = v.second; bl = lc; }
    }
    if (!best) uu_log("resolve: no anchor matched (first=%s)", anchors.size() ? *anchors.begin() : "?");
    return best;
}

uintptr_t find_func_by_logstr(const ModRange& r, const char* logstr) {
    return find_func(r, { logstr });
}

uintptr_t find_string(const ModRange& r, const char* s) {
    std::vector<uintptr_t> v;
    find_str_addrs(r, s, v);
    return v.empty() ? 0 : v.front();
}

uintptr_t find_func_by_aob(const ModRange& r, const char* pattern) {
    if (!pattern || !*pattern) return 0;
    std::vector<uint8_t> bytes; std::vector<bool> wild;
    for (const char* p = pattern; *p; ) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { wild.push_back(true); bytes.push_back(0); p += (p[1] == '?') ? 2 : 1; }
        else {
            auto hex = [](char c)->int { return (c>='0'&&c<='9')?c-'0':(c>='A'&&c<='F')?c-'A'+10:(c>='a'&&c<='f')?c-'a'+10:-1; };
            int hi = hex(p[0]), lo = (p[1]?hex(p[1]):-1);
            if (hi < 0 || lo < 0) { uu_log("aob pattern malformed, ignored"); return 0; }
            wild.push_back(false); bytes.push_back((uint8_t)(hi*16+lo)); p += 2;
        }
    }
    size_t m = bytes.size();
    if (m < 8) return 0;
    auto t0 = (uint8_t*)r.text_beg, t1 = (uint8_t*)r.text_end;
    uintptr_t found = 0; int cnt = 0;
    for (uint8_t* p = t0; p + m <= t1; ++p) {
        size_t j = 0;
        for (; j < m; ++j) if (!wild[j] && p[j] != bytes[j]) break;
        if (j == m) { found = (uintptr_t)p; if (++cnt > 1) return 0; }   // 多于一处匹配则放弃(不可靠)
    }
    return cnt == 1 ? found : 0;
}

} // namespace resolver
