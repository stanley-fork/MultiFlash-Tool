#ifndef EDL_SERVICE_TASK_H
#define EDL_SERVICE_TASK_H

#include "edl/edl_service.h"

#include <stdbool.h>
#include <stdint.h>

typedef edl_error_t (*svc_stage_fn_t)(edl_service_t *svc, void *ctx);

typedef struct {
    const char    *label;
    int            timeout_ms;
    int            retries;
    int            retry_delay_ms;
    svc_stage_fn_t fn;
    void          *ctx;
} svc_stage_spec_t;

typedef struct {
    uint64_t    (*now_ms)(void);
    bool        (*is_cancelled)(const edl_service_t *svc);
    bool        (*sleep_cancelable)(edl_service_t *svc, int total_ms);
    void        (*begin_timeout)(edl_service_t *svc, int timeout_ms);
    void        (*end_timeout)(edl_service_t *svc);
    edl_error_t (*normalize_error)(edl_service_t *svc, edl_error_t err);
    void        (*log)(edl_service_t *svc, const char *fmt, ...);
    void        (*log_detail)(edl_service_t *svc, const char *fmt, ...);
} svc_stage_runtime_t;

edl_error_t svc_run_stage(edl_service_t *svc, const svc_stage_runtime_t *runtime,
                          const svc_stage_spec_t *stage);

#endif /* EDL_SERVICE_TASK_H */
