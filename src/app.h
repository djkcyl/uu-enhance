#ifndef UU_APP_H
#define UU_APP_H

// Project identity. Preprocessor-only so the .rc and the .cpp files share one
// version number. ASCII only here: rc.exe reads this as the system ANSI code
// page, and non-ASCII comments can break the macro parsing. The Chinese
// display name lives in tray.cpp (compiled as UTF-8), not here.
// To bump the version, change UURE_VERSION and UURE_VERSION_RC together.
#define UURE_NAME        "uu-enhance"
#define UURE_VERSION     "1.0.3"
#define UURE_VERSION_W   L"1.0.3"
#define UURE_VERSION_RC  1,0,3,0
#define UURE_GITHUB      "https://github.com/djkcyl/uu-enhance"
#define UURE_GITHUB_W    L"https://github.com/djkcyl/uu-enhance"

#endif
