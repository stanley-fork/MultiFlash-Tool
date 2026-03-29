#ifndef MTK_SECURITY_H
#define MTK_SECURITY_H

#include "mtk_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

MTK_API mtk_error_t mtk_security_probe(mtk_transport_t* transport,
                                       const mtk_device_info_t* device_info,
                                       mtk_security_state_t* security_state,
                                       mtk_capabilities_t* capabilities);

#ifdef __cplusplus
}
#endif

#endif
