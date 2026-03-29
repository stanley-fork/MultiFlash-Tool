cmake_minimum_required(VERSION 3.16)

project(edl_core LANGUAGES C)

# Sahara: 默认先进入 command mode 读取芯片信息；仅传 Loader 的机型可按需关闭。
option(EDL_SAHARA_COMMAND_MODE "Sahara handshake: enter command mode for chip info before loader (default ON)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CORE_SOURCES
    src/edl_error.c
    src/crc32.c
    src/xml_helper.c
    src/sahara.c
    src/firehose.c
    src/gpt.c
    src/slot_detect.c
    src/rawprogram.c
    src/sparse.c
    src/edl_service.c
    src/service_device.c
    src/service_connect.c
    src/service_gpt.c
    src/service_flash.c
    src/service_task.c
    src/service_query.c
    src/service_props.c
    src/vendor_plugin.c
    src/chip_db.c
    src/storage_report.c
    src/vip_spoof.c
    src/realme_auth.c
    src/xiaomi_auth.c
    src/oneplus_auth.c
    src/ext4_parser.c
    src/erofs_parser.c
    src/fs_prop_probe.c
)

if(WIN32)
    list(APPEND CORE_SOURCES
        src/serial_port_win.c
        src/port_detect_win.c
    )
else()
    list(APPEND CORE_SOURCES src/port_detect_stub.c)
endif()

add_library(edl_core STATIC ${CORE_SOURCES})

if(EDL_SAHARA_COMMAND_MODE)
    target_compile_definitions(edl_core PRIVATE EDL_SAHARA_TRY_COMMAND_MODE=1)
else()
    target_compile_definitions(edl_core PRIVATE EDL_SAHARA_TRY_COMMAND_MODE=0)
endif()

target_include_directories(edl_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(edl_core PUBLIC
        setupapi
        cfgmgr32
        bcrypt
    )
endif()
