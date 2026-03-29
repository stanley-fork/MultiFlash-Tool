#include "mtk/mtk_storage.h"

mtk_error_t mtk_storage_probe(mtk_da_session_t* session,
                              mtk_device_info_t* device_info) {
    if (!session || !device_info) {
        return MTK_E_INVALID_ARG;
    }
    if (!mtk_da_is_ready(session)) {
        return MTK_E_STATE;
    }
    device_info->boot_stage = MTK_BOOT_STAGE_DA;
    device_info->storage_type = MTK_STORAGE_EMMC;
    device_info->block_size = 512;
    device_info->total_size = 0;
    return MTK_OK;
}

mtk_error_t mtk_storage_read_blocks(mtk_da_session_t* session,
                                    uint32_t region,
                                    uint64_t start_lba,
                                    uint64_t block_count,
                                    const char* out_path) {
    (void)region; (void)start_lba; (void)block_count; (void)out_path;
    if (!session) {
        return MTK_E_INVALID_ARG;
    }
    return MTK_E_NOT_IMPLEMENTED;
}

mtk_error_t mtk_storage_write_blocks(mtk_da_session_t* session,
                                     uint32_t region,
                                     uint64_t start_lba,
                                     uint64_t block_count,
                                     const char* image_path) {
    (void)region; (void)start_lba; (void)block_count; (void)image_path;
    if (!session) {
        return MTK_E_INVALID_ARG;
    }
    return MTK_E_NOT_IMPLEMENTED;
}

mtk_error_t mtk_storage_erase_blocks(mtk_da_session_t* session,
                                     uint32_t region,
                                     uint64_t start_lba,
                                     uint64_t block_count) {
    (void)region; (void)start_lba; (void)block_count;
    if (!session) {
        return MTK_E_INVALID_ARG;
    }
    return MTK_E_NOT_IMPLEMENTED;
}
