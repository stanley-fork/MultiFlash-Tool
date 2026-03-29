#ifndef MTK_PARTITION_H
#define MTK_PARTITION_H

#include "mtk_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

MTK_API mtk_error_t mtk_partition_read_table(mtk_da_session_t* session,
                                             mtk_partition_info_t* parts,
                                             size_t* inout_count);

MTK_API const mtk_partition_info_t* mtk_partition_find_by_name(const mtk_partition_info_t* parts,
                                                               size_t count,
                                                               const char* name);

#ifdef __cplusplus
}
#endif

#endif
