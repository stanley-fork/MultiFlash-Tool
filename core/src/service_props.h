#ifndef EDL_SERVICE_PROPS_H
#define EDL_SERVICE_PROPS_H

#include "edl/edl_service.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t (*now_ms)(void);
    bool     (*is_cancelled)(const edl_service_t *svc);
    void     (*log_detail)(edl_service_t *svc, const char *fmt, ...);
    void     (*progress)(edl_service_t *svc, int64_t current, int64_t total);
    void     (*log_elapsed)(edl_service_t *svc, const char *stage, edl_error_t err,
                            uint64_t start_ms);
} svc_prop_probe_runtime_t;

edl_error_t edl_service_probe_android_build_props_impl(
    edl_service_t *svc,
    const svc_prop_probe_runtime_t *runtime,
    edl_android_props_t *out);

#endif /* EDL_SERVICE_PROPS_H */
