cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    # MSVC 运行库由目标 MSVC_RUNTIME_LIBRARY 控制，与 vcpkg x64-windows-static-md (/MD) 一致
    cmake_policy(SET CMP0091 NEW)
endif()

project(EDL VERSION 4.0.0 LANGUAGES CXX C)

# Build libedl core (C static library)
add_subdirectory(core)

# Optional MTK low-level core. OFF by default so current app build is unchanged.
option(EDL_ENABLE_MTK_CORE "Build experimental MTK low-level core (no UI integration)" OFF)
if(EDL_ENABLE_MTK_CORE)
    add_subdirectory(mtk_core)
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/Zc:__cplusplus /utf-8)
endif()

# ---------------------------------------------------------------------------
# 静态链接 Qt（需使用「静态编译」的 Qt 套件，见 docs/BUILD_STATIC.md）
# 开启后：优先找静态库，MinGW 下为 exe 追加 -static 等（与静态 Qt 配套使用）
# ---------------------------------------------------------------------------
option(EDL_STATIC_QT "Link Qt statically (requires Qt built with -static; see docs/BUILD_STATIC.md)" OFF)
option(EDL_MSVC_STATIC_RUNTIME "MSVC: force /MT for EDL (only if your static Qt uses /MT; vcpkg *-static-md uses /MD — leave OFF)" OFF)

# vcpkg static-md 依赖 /MD：与 Qt/ICU 静态库一致（勿与 /MT 混用）
if(MSVC AND EDL_STATIC_QT AND NOT EDL_MSVC_STATIC_RUNTIME)
    set_target_properties(edl_core PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(EDL_STATIC_QT)
    set(QT_STATIC ON CACHE BOOL "Qt static linking" FORCE)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
        set(CMAKE_FIND_PACKAGE_PREFER_STATIC ON)
    endif()
    message(STATUS "EDL_STATIC_QT=ON — 请确认 CMAKE_PREFIX_PATH 指向「静态 Qt」安装前缀")
endif()

set(_edl_qt_components Widgets Svg SvgWidgets Network)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS ${_edl_qt_components})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${_edl_qt_components})

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    settingsdialog.cpp
    settingsdialog.h
    devicedialog.cpp
    devicedialog.h
    edl_api_client.cpp
    edl_api_client.h
    resources.qrc
)

# 可选：embedded/bundled/ 下三个文件齐全时生成 bundled_gen.qrc 并随 exe 分发（运行时自动释放到 AppData）
set(_edl_bundle_dir "${CMAKE_CURRENT_SOURCE_DIR}/bundled")
set(_edl_dwebp "${_edl_bundle_dir}/dwebp.exe")
set(_edl_misc_fb "${_edl_bundle_dir}/misc_tofastbootd.img")
set(_edl_misc_rec "${_edl_bundle_dir}/misc_torecovery.img")
if(EXISTS "${_edl_dwebp}" AND EXISTS "${_edl_misc_fb}" AND EXISTS "${_edl_misc_rec}")
    file(TO_CMAKE_PATH "${_edl_dwebp}" _p1)
    file(TO_CMAKE_PATH "${_edl_misc_fb}" _p2)
    file(TO_CMAKE_PATH "${_edl_misc_rec}" _p3)
    string(REPLACE "\\" "/" _p1 "${_p1}")
    string(REPLACE "\\" "/" _p2 "${_p2}")
    string(REPLACE "\\" "/" _p3 "${_p3}")
    set(_edl_bundle_qrc "${CMAKE_CURRENT_BINARY_DIR}/bundled_gen.qrc")
    file(WRITE "${_edl_bundle_qrc}"
"<RCC>
  <qresource prefix=\"/bundled\">
    <file alias=\"dwebp.exe\">${_p1}</file>
    <file alias=\"misc_tofastbootd.img\">${_p2}</file>
    <file alias=\"misc_torecovery.img\">${_p3}</file>
  </qresource>
</RCC>
")
    list(APPEND PROJECT_SOURCES "${_edl_bundle_qrc}")
    message(STATUS "bundled: embedding dwebp.exe + misc_tofastbootd.img + misc_torecovery.img")
else()
    message(WARNING "bundled/ 未集齐 dwebp.exe、misc_tofastbootd.img、misc_torecovery.img — 跳过资源嵌入，详见 bundled/README.md")
endif()

# 可选：bundled/misc_vendor/*.img → 嵌入 :/misc_vendor/（恢复出厂 / MISC 重启用）
set(_edl_misc_vendor_dir "${CMAKE_CURRENT_SOURCE_DIR}/bundled/misc_vendor")
file(GLOB _edl_misc_vendor_imgs "${_edl_misc_vendor_dir}/*.img")
if(_edl_misc_vendor_imgs)
    set(_mv_body "")
    foreach(img IN LISTS _edl_misc_vendor_imgs)
        get_filename_component(_alias "${img}" NAME)
        file(TO_CMAKE_PATH "${img}" _abs)
        string(REPLACE "\\" "/" _abs "${_abs}")
        string(APPEND _mv_body "    <file alias=\"${_alias}\">${_abs}</file>\n")
    endforeach()
    set(_edl_mv_qrc "${CMAKE_CURRENT_BINARY_DIR}/misc_vendor_gen.qrc")
    file(WRITE "${_edl_mv_qrc}" "<RCC>\n  <qresource prefix=\"/misc_vendor\">\n${_mv_body}  </qresource>\n</RCC>\n")
    list(APPEND PROJECT_SOURCES "${_edl_mv_qrc}")
    list(LENGTH _edl_misc_vendor_imgs _edl_mv_n)
    message(STATUS "misc_vendor: embedding ${_edl_mv_n} *.img from bundled/misc_vendor")
else()
    message(STATUS "misc_vendor: no *.img in bundled/misc_vendor (place imgs or beside exe)")
endif()

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(EDL
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
else()
    add_executable(EDL
        ${PROJECT_SOURCES}
    )
endif()

target_link_libraries(EDL PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Svg
    Qt${QT_VERSION_MAJOR}::SvgWidgets
    Qt${QT_VERSION_MAJOR}::Network
    edl_core
)

if(WIN32)
    target_link_libraries(EDL PRIVATE dwmapi)
endif()

if(MSVC AND EDL_STATIC_QT)
    if(EDL_MSVC_STATIC_RUNTIME)
        set_target_properties(EDL PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set_target_properties(EDL PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

# 静态 Qt：嵌入 SVG 等插件（动态 Qt 仍从 plugins 目录加载，勿重复导入）
if(EDL_STATIC_QT AND QT_VERSION_MAJOR EQUAL 6)
    if(TARGET Qt6::QSvgPlugin)
        qt_import_plugins(EDL INCLUDE Qt6::QSvgPlugin)
    else()
        message(WARNING "未找到 Qt6::QSvgPlugin，静态链接时 SVG 可能需额外 find_package(Qt6 Svg) 或检查 Qt6Svg 是否随静态包安装")
    endif()
endif()

# MinGW：与静态 Qt 一起使用时，把 libgcc/libstdc++/winpthread 等打进 exe，减少 DLL 依赖
if(EDL_STATIC_QT AND WIN32 AND MINGW)
    target_link_options(EDL PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()

# MSVC：仅当「自编译静态 Qt 使用 /MT」时打开 EDL_MSVC_STATIC_RUNTIME；vcpkg x64-windows-static-md 为 /MD，勿强开
if(EDL_STATIC_QT AND MSVC AND EDL_MSVC_STATIC_RUNTIME)
    set_property(TARGET EDL PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(EDL PROPERTIES
    OUTPUT_NAME "SAKURAEDL"
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS EDL
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(EDL)
endif()

# 静态 Qt + MSVC：LINK_LIBRARIES 在 Ninja 的 @rsp 里；msvcprt 若只出现在中间，Qt6Gui 等在后面才引入
# __std_* 未解析（LNK2019）。把 msvcprt.lib 放进 LINK_FLAGS（在 @rsp 之后），使链接器最后处理 STL。
if(MSVC AND EDL_STATIC_QT)
    # cl 位于 .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe → 工具集根为 .../<ver>
    get_filename_component(_edl_vc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_p1 "${_edl_vc_bin}" DIRECTORY)
    get_filename_component(_p2 "${_p1}" DIRECTORY)
    get_filename_component(_edl_msvc_toolset "${_p2}" DIRECTORY)
    set(_edl_msvcprt_lib "${_edl_msvc_toolset}/lib/x64/msvcprt.lib")
    target_link_options(EDL PRIVATE "${_edl_msvcprt_lib}")
endif()
