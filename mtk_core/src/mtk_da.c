#include "mtk/mtk_da.h"

#include <stdlib.h>

typedef struct mtk_da_session {
    int ready;
    mtk_da_mode_t mode;
    uint32_t hwcode;
} mtk_da_session_impl_t;

static mtk_da_mode_t mtk_da_choose_mode(const mtk_connect_options_t* options,
                                        const mtk_device_info_t* device_info,
                                        const mtk_security_state_t* security_state) {
    (void)device_info;
    if (options && options->preferred_da_mode != MTK_DA_MODE_UNKNOWN)
        return options->preferred_da_mode;
    if (security_state && security_state->preloader_mode)
        return MTK_DA_MODE_XFLASH;
    return MTK_DA_MODE_LEGACY;
}

mtk_error_t mtk_da_upload(mtk_transport_t* transport,
                          const mtk_connect_options_t* options,
                          const mtk_device_info_t* device_info,
                          const mtk_security_state_t* security_state,
                          mtk_da_session_t** out_session,
                          mtk_da_mode_t* out_mode) {
    mtk_da_session_impl_t* session;
    mtk_da_mode_t mode;
    if (!transport || !options || !device_info || !security_state || !out_session || !out_mode) {
        return MTK_E_INVALID_ARG;
    }
    if (!security_state->can_upload_da) {
        return MTK_E_SECURITY;
    }
    if (!options->da_path || !options->da_path[0]) {
        return MTK_E_INVALID_ARG;
    }

    mode = mtk_da_choose_mode(options, device_info, security_state);

    session = (mtk_da_session_impl_t*)calloc(1, sizeof(*session));
    if (!session) {
        return MTK_E_NO_MEMORY;
    }

    session->mode = mode;
    session->hwcode = device_info->hwcode;
    session->ready = 1;

    *out_session = (mtk_da_session_t*)session;
    *out_mode = session->mode;
    return MTK_OK;
}

void mtk_da_close(mtk_da_session_t* session) {
    free(session);
}

int mtk_da_is_ready(const mtk_da_session_t* session) {
    const mtk_da_session_impl_t* s = (const mtk_da_session_impl_t*)session;
    return s ? s->ready : 0;
}

mtk_da_mode_t mtk_da_mode(const mtk_da_session_t* session) {
    const mtk_da_session_impl_t* s = (const mtk_da_session_impl_t*)session;
    return s ? s->mode : MTK_DA_MODE_UNKNOWN;
}
