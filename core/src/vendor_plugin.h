#ifndef EDL_VENDOR_PLUGIN_H
#define EDL_VENDOR_PLUGIN_H

#include "edl/edl_service.h"
#include "edl/firehose.h"
#include "edl/sahara.h"
#include "edl/chip_db.h"

typedef enum {
    EDL_VENDOR_PLUGIN_PRE_CONFIGURE = 0,
    EDL_VENDOR_PLUGIN_POST_CONFIGURE
} edl_vendor_plugin_phase_t;

typedef struct {
    edl_vendor_plugin_phase_t phase;
    edl_port_t               *port;
    edl_sahara_t             *sahara;
    edl_firehose_t           *firehose;
    const edl_callbacks_t    *cb;
    const edl_svc_auth_options_t *auth;
    const edl_chip_info_t    *chip_info;
    const edl_platform_profile_t *profile;
} edl_vendor_plugin_ctx_t;

const char *edl_vendor_plugin_name(edl_svc_auth_mode_t mode);
edl_error_t edl_vendor_plugin_run(edl_svc_auth_mode_t mode,
                                  const edl_vendor_plugin_ctx_t *ctx);

#endif /* EDL_VENDOR_PLUGIN_H */
