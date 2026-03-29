#ifndef EDL_SERVICE_INTERNAL_H
#define EDL_SERVICE_INTERNAL_H

#include "edl/edl_service.h"
#include "edl/serial_port.h"
#include "edl/sahara.h"
#include "edl/firehose.h"
#include "service_task.h"

#include <stdbool.h>
#include <stdint.h>

#define EDL_SERVICE_MAX_PARTITIONS 256
#define EDL_SERVICE_PROGRESS_SCOPE_STACK_MAX 8

typedef struct {
    bool    remap_active;
    bool    suppress_core;
    int64_t remap_base;
    int64_t remap_span;
    int64_t remap_total;
} edl_service_progress_state_t;

struct edl_service {
    edl_port_t      *port;
    edl_sahara_t    *sahara;
    edl_firehose_t  *firehose;
    edl_callbacks_t  cb;
    edl_callbacks_t  core_cb;
    edl_loader_info_t loader_info;

    bool connected;
    char port_name[32];
    char storage_type[16];

    edl_partition_info_t partitions[EDL_SERVICE_MAX_PARTITIONS];
    int partition_count;
    bool partitions_loaded;
    bool gpt_scan_failed;
    edl_error_t gpt_last_scan_err;

    edl_svc_auth_mode_t auth_mode;
    uint64_t            stage_deadline_ms;
    bool                stage_timeout_hit;
    edl_service_progress_state_t progress_state;
    edl_service_progress_state_t progress_scope_stack[EDL_SERVICE_PROGRESS_SCOPE_STACK_MAX];
    int                          progress_scope_depth;
};

void svc_log(edl_service_t *svc, const char *fmt, ...);
void svc_log_detail(edl_service_t *svc, const char *fmt, ...);
uint64_t svc_now_ms(void);
bool svc_is_cancelled(const edl_service_t *svc);
bool svc_sleep_cancelable(edl_service_t *svc, int total_ms);
void svc_init_core_callbacks(edl_service_t *svc);
void svc_progress_scope_reset(edl_service_t *svc);
bool svc_progress_scope_push(edl_service_t *svc, bool suppress_core,
                             bool remap_active, int64_t base, int64_t span, int64_t total);
void svc_progress_scope_pop(edl_service_t *svc);
void svc_progress_remap_reset(edl_service_t *svc);
void svc_progress_remap_begin(edl_service_t *svc, int64_t base, int64_t span, int64_t total);
void svc_progress_set_core_suppressed(edl_service_t *svc, bool suppressed);
void svc_progress_report(edl_service_t *svc, int64_t current, int64_t total);
void svc_log_elapsed(edl_service_t *svc, const char *stage, edl_error_t err,
                     uint64_t start_ms);
void svc_clear_partition_cache(edl_service_t *svc);

int edl_service_default_gpt_max_lun(const edl_service_t *svc);
const svc_stage_runtime_t *edl_service_stage_runtime(void);

#endif /* EDL_SERVICE_INTERNAL_H */
