cmake_minimum_required(VERSION 3.20)
project(uu_enhance CXX C RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码含中文注释(UTF-8)，让 MSVC 以 UTF-8 解析，避免代码页 936 误读
add_compile_options(/utf-8)

# 静态链接 CRT，使产物为零依赖的 drop-in
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# 从 app.h 取版本号(唯一定义处)，用于安装器产物名
file(READ ${CMAKE_SOURCE_DIR}/src/app.h _APP_H)
string(REGEX MATCH "UURE_VERSION[ \t]+\"([^\"]+)\"" _ "${_APP_H}")
set(UURE_VER "${CMAKE_MATCH_1}")

set(MINHOOK_DIR ${CMAKE_SOURCE_DIR}/vendor/minhook)

add_library(uu_version SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/hooks.cpp
    src/server.cpp
    src/config.cpp
    src/resolver.cpp
    src/xdisasm.cpp
    src/hookset.cpp
    src/tray.cpp
    src/update.cpp
    src/app.rc
    # MinHook
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)

target_include_directories(uu_version PRIVATE
    ${MINHOOK_DIR}/include
    ${MINHOOK_DIR}/src/hde   # HDE64 头文件(xdisasm.cpp 复用 MinHook 自带的长度反汇编器)
    src
)

# 产物名为 version.dll
set_target_properties(uu_version PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
)

target_link_libraries(uu_version PRIVATE user32 shell32 advapi32 winhttp)

# ---------------------------------------------------------------------------
# 一键安装器 (uu-enhance-installer.exe)
# 把刚构建好的 version.dll 以资源形式内嵌, 运行时自动找安装目录并释放/卸载。
# ---------------------------------------------------------------------------

# 1) 把 version.dll 拷到固定名字的位置, 供安装器的 .rc 以 RCDATA 内嵌。
#    放在 build 根的 payload/ 下(与配置无关), 安装器 rc 通过 include 目录找到它。
set(PAYLOAD_DIR ${CMAKE_BINARY_DIR}/payload)
add_custom_command(
    OUTPUT ${PAYLOAD_DIR}/version.dll
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PAYLOAD_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uu_version> ${PAYLOAD_DIR}/version.dll
    DEPENDS uu_version
    COMMENT "拷贝 version.dll 供安装器内嵌"
    VERBATIM)
add_custom_target(uu_payload DEPENDS ${PAYLOAD_DIR}/version.dll)

# 2) GUI 程序 (WIN32 = 不弹控制台窗口)
add_executable(uu_installer WIN32
    installer/installer.cpp
    installer/installer.rc
)
add_dependencies(uu_installer uu_payload)

# rc 在 PAYLOAD_DIR 里找 version.dll; src 里找 app.h(共用版本号/GitHub)
target_include_directories(uu_installer PRIVATE ${PAYLOAD_DIR} src)

set_target_properties(uu_installer PROPERTIES OUTPUT_NAME "uu-enhance-installer-${UURE_VER}")
target_link_libraries(uu_installer PRIVATE
    user32 shell32 ole32 advapi32 comctl32 version winhttp rstrtmgr)
# 我们在 installer.rc 里自带 manifest(申请管理员权限), 关掉链接器默认生成的那个避免冲突
target_link_options(uu_installer PRIVATE /MANIFEST:NO)

# UPX 最大压缩安装器(装了 upx 才压, 没有则跳过, 不阻断构建)
find_program(UPX_EXE upx)
if(UPX_EXE)
    add_custom_command(TARGET uu_installer POST_BUILD
        COMMAND ${UPX_EXE} --brute --best "$<TARGET_FILE:uu_installer>"
        COMMENT "UPX 压缩安装器"
        VERBATIM)
else()
    message(STATUS "未找到 upx, 跳过安装器压缩")
endif()
