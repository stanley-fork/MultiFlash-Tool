#include "mtk/mtk_security.h"

#include <string.h>

mtk_error_t mtk_security_probe(mtk_transport_t* transport,
                               const mtk_device_info_t* device_info,
                               mtk_security_state_t* security_state,
                               mtk_capabilities_t* capabilities) {
    (void)transport;
    if (!device_info || !security_state || !capabilities) {
        return MTK_E_INVALID_ARG;
    }

    memset(security_state, 0, sizeof(*security_state));
    memset(capabilities, 0, sizeof(*capabilities));

    capabilities->can_connect = 1;
    capabilities->can_detect_stage = 1;

    switch (device_info->boot_stage) {
    case MTK_BOOT_STAGE_BROM:
        security_state->brom_sync_ok = 1;
        security_state->can_upload_da = 1;
        capabilities->can_upload_da = 1;
        break;
    case MTK_BOOT_STAGE_PRELOADER:
        security_state->preloader_mode = 1;
        security_state->can_upload_da = 1;
        capabilities->can_upload_da = 1;
        break;
    case MTK_BOOT_STAGE_DA:
        security_state->can_upload_da = 1;
        security_state->can_read_flash = 1;
        security_state->can_write_flash = 1;
        security_state->can_erase_flash = 1;
        capabilities->can_upload_da = 1;
        capabilities->can_read_flash = 1;
        capabilities->can_write_flash = 1;
        capabilities->can_erase_flash = 1;
        capabilities->can_read_partitions = 1;
        capabilities->can_write_partitions = 1;
        capabilities->can_erase_partitions = 1;
        capabilities->can_reboot = 1;
        break;
    case MTK_BOOT_STAGE_UNKNOWN:
    default:
        break;
    }

    return MTK_OK;
}
