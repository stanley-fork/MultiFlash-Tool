#ifndef MTK_STORAGE_H
#define MTK_STORAGE_H

#include "mtk_da.h"

#ifdef __cplusplus
extern "C" {
#endif

MTK_API mtk_error_t mtk_storage_probe(mtk_da_session_t* session,
                                      mtk_device_info_t* device_info);

MTK_API mtk_error_t mtk_storage_read_blocks(mtk_da_session_t* session,
                                            uint32_t region,
                                            uint64_t start_lba,
                                            uint64_t block_count,
                                            const char* out_path);

MTK_API mtk_error_t mtk_storage_write_blocks(mtk_da_session_t* session,
                                             uint32_t region,
                                             uint64_t start_lba,
                                             uint64_t block_count,
                                             const char* image_path);

MTK_API mtk_error_t mtk_storage_erase_blocks(mtk_da_session_t* session,
                                             uint32_t region,
                                             uint64_t start_lba,
                                             uint64_t block_count);

#ifdef __cplusplus
}
#endif

#endif
