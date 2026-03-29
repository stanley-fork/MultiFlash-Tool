#include "mtk/mtk_boot.h"
#include "mtk/mtk_port_detect.h"

#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define mtk_stricmp _stricmp
#else
#include <strings.h>
#define mtk_stricmp strcasecmp
#endif

static void mtk_boot_log(mtk_transport_t* transport, mtk_log_level_t level, const char* msg) {
    (void)transport;
    (void)level;
    (void)msg;
}

static mtk_boot_stage_t mtk_detect_stage_from_port_name(const char* port_name) {
    mtk_detected_port_t found[32];
    int count;
    int i;
    if (!port_name || !*port_name) return MTK_BOOT_STAGE_UNKNOWN;
    count = mtk_port_detect(found, 32);
    for (i = 0; i < count; ++i) {
        if (mtk_stricmp(found[i].port_name, port_name) != 0)
            continue;
        if (found[i].type == MTK_PORT_BROM)
            return MTK_BOOT_STAGE_BROM;
        if (found[i].type == MTK_PORT_PRELOADER)
            return MTK_BOOT_STAGE_PRELOADER;
    }
    return MTK_BOOT_STAGE_UNKNOWN;
}

static int mtk_boot_try_brom_sync(mtk_transport_t* transport) {
    static const uint8_t sync_bytes[4] = { 0xA0, 0x0A, 0x50, 0x05 };
    static const uint8_t sync_expect[4] = { 0x5F, 0xF5, 0xAF, 0xFA };
    uint8_t resp[4] = {0};
    int i;

    if (!transport || !mtk_transport_is_open(transport))
        return 0;

    mtk_transport_purge(transport);

    for (i = 0; i < 4; ++i) {
        int wr = mtk_transport_write(transport, &sync_bytes[i], 1);
        int rd;
        if (wr != 1)
            return 0;
        rd = mtk_transport_read_exact(transport, &resp[i], 1, 200);
        if (rd != 1)
            return 0;
        if (resp[i] != sync_expect[i])
            return 0;
    }

    return 1;
}

static int mtk_boot_try_read_hwcode_stub(mtk_transport_t* transport, uint32_t* out_hwcode) {
    (void)transport;
    if (!out_hwcode)
        return 0;
    *out_hwcode = MTK_HWCODE_UNKNOWN;
    return 0;
}

mtk_error_t mtk_boot_detect_stage(mtk_transport_t* transport,
                                  mtk_device_info_t* device_info) {
    const char* port_name;
    mtk_boot_stage_t stage;
    char logbuf[128];

    if (!transport || !device_info) {
        return MTK_E_INVALID_ARG;
    }
    if (!mtk_transport_is_open(transport)) {
        return MTK_E_NOT_CONNECTED;
    }

    memset(device_info, 0, sizeof(*device_info));
    port_name = mtk_transport_port_name(transport);
    if (port_name) {
        strncpy(device_info->port_name, port_name, sizeof(device_info->port_name) - 1);
    }

    stage = mtk_detect_stage_from_port_name(port_name);
    device_info->boot_stage = stage;
    device_info->hwcode = MTK_HWCODE_UNKNOWN;
    device_info->storage_type = MTK_STORAGE_UNKNOWN;

    if (stage == MTK_BOOT_STAGE_BROM || stage == MTK_BOOT_STAGE_UNKNOWN) {
        if (mtk_boot_try_brom_sync(transport)) {
            device_info->boot_stage = MTK_BOOT_STAGE_BROM;
            if (mtk_boot_try_read_hwcode_stub(transport, &device_info->hwcode)) {
                snprintf(logbuf, sizeof(logbuf), "mtk boot: brom sync ok, hwcode=0x%04X", (unsigned)device_info->hwcode);
            } else {
                snprintf(logbuf, sizeof(logbuf), "mtk boot: brom sync ok");
            }
            mtk_boot_log(transport, MTK_LOG_INFO, logbuf);
            return MTK_OK;
        }
    }

    if (device_info->boot_stage == MTK_BOOT_STAGE_PRELOADER) {
        mtk_boot_log(transport, MTK_LOG_INFO, "mtk boot: preloader stage inferred from port enumeration");
    } else if (device_info->boot_stage == MTK_BOOT_STAGE_BROM) {
        mtk_boot_log(transport, MTK_LOG_INFO, "mtk boot: brom stage inferred from port enumeration");
    } else {
        mtk_boot_log(transport, MTK_LOG_WARN, "mtk boot: stage unknown, brom sync probe failed");
    }

    return MTK_OK;
}
