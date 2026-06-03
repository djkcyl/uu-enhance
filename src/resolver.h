#pragma once
#include <windows.h>
#include <cstdint>
#include <initializer_list>

// 抗版本更新的函数定位：用唯一日志字符串(debug 构建内嵌)反查函数入口。
// 锚点 → 引用它的 RIP-相对 lea → .pdata(异常展开表)确定性映射到函数入口。
// 多锚点投票：得票(=命中的锚点数)最多的函数胜出；单锚失效(改名/删除)不致命。
namespace resolver {
    struct ModRange {
        uintptr_t text_beg, text_end;
        uintptr_t rdata_beg, rdata_end;
        uintptr_t pdata_beg, pdata_end;   // RUNTIME_FUNCTION[]
        uintptr_t img_beg, img_end;       // img_beg = imagebase
    };

    bool get_ranges(HMODULE mod, ModRange& out);

    // 多锚点投票定位；找不到返回 0。anchors 应为该函数体内唯一的字符串(函数名+独特日志)。
    uintptr_t find_func(const ModRange& r, std::initializer_list<const char*> anchors);
    // 单锚便捷封装
    uintptr_t find_func_by_logstr(const ModRange& r, const char* logstr);
    // 字节特征(AOB)定位，pattern 形如 "48 89 ?? ?? E8"，?? 为通配；要求唯一匹配，否则返回 0。
    uintptr_t find_func_by_aob(const ModRange& r, const char* pattern);
}
