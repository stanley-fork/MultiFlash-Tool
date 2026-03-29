#ifndef MTK_BOOT_H
#define MTK_BOOT_H

#include "mtk_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

MTK_API mtk_error_t mtk_boot_detect_stage(mtk_transport_t* transport,
                                          mtk_device_info_t* device_info);

#ifdef __cplusplus
}
#endif

#endif
