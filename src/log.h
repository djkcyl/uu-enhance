#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "app.h"

inline void uu_log(const char* fmt, ...) {
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), "[" UURE_NAME "] ");
    if (n < 0 || n >= (int)sizeof(buf) - 2) n = 0;
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    std::strcat(buf, "\n");
    OutputDebugStringA(buf);  // DebugView 里看
}
