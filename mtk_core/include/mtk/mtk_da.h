#ifndef MTK_DA_H
#define MTK_DA_H

#include "mtk_security.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtk_da_session mtk_da_session_t;

MTK_API mtk_error_t mtk_da_upload(mtk_transport_t* transport,
                                  const mtk_connect_options_t* options,
                                  const mtk_device_info_t* device_info,
                                  const mtk_security_state_t* security_state,
                                  mtk_da_session_t** out_session,
                                  mtk_da_mode_t* out_mode);

MTK_API void mtk_da_close(mtk_da_session_t* session);
MTK_API int mtk_da_is_ready(const mtk_da_session_t* session);
MTK_API mtk_da_mode_t mtk_da_mode(const mtk_da_session_t* session);

#ifdef __cplusplus
}
#endif

#endif
