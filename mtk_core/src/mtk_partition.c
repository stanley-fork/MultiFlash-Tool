#include "mtk/mtk_partition.h"

#include <string.h>

mtk_error_t mtk_partition_read_table(mtk_da_session_t* session,
                                     mtk_partition_info_t* parts,
                                     size_t* inout_count) {
    if (!session || !inout_count) {
        return MTK_E_INVALID_ARG;
    }
    if (!parts || *inout_count == 0) {
        *inout_count = 1;
        return MTK_OK;
    }

    memset(parts, 0, sizeof(parts[0]));
    strncpy(parts[0].name, "unknown", sizeof(parts[0].name) - 1);
    parts[0].region = 0;
    parts[0].start_lba = 0;
    parts[0].lba_count = 0;
    parts[0].block_size = 512;
    parts[0].byte_size = 0;
    *inout_count = 1;
    return MTK_OK;
}

const mtk_partition_info_t* mtk_partition_find_by_name(const mtk_partition_info_t* parts,
                                                       size_t count,
                                                       const char* name) {
    size_t i;
    if (!parts || !name) {
        return NULL;
    }
    for (i = 0; i < count; ++i) {
        if (strncmp(parts[i].name, name, sizeof(parts[i].name)) == 0) {
            return &parts[i];
        }
    }
    return NULL;
}
