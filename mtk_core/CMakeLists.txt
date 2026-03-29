cmake_minimum_required(VERSION 3.16)

project(mtk_core LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(MTK_CORE_SOURCES
    src/mtk_error.c
    src/mtk_transport.c
    src/mtk_boot.c
    src/mtk_security.c
    src/mtk_da.c
    src/mtk_storage.c
    src/mtk_partition.c
    src/mtk_service.c
)

if(WIN32)
    list(APPEND MTK_CORE_SOURCES
        src/mtk_port_detect_win.c
    )
else()
    list(APPEND MTK_CORE_SOURCES
        src/mtk_port_detect_stub.c
    )
endif()

add_library(mtk_core STATIC ${MTK_CORE_SOURCES})

target_include_directories(mtk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(mtk_core PUBLIC
        setupapi
        cfgmgr32
        bcrypt
    )
endif()
