#include "edl/edl_service.h"
#include "service_internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define edl_sleep_ms(ms) Sleep(ms)
#else
#include <time.h>
#include <unistd.h>
#define edl_sleep_ms(ms) usleep((ms) * 1000)
#endif

void svc_log(edl_service_t *svc, const char *fmt, ...)
{
    if (!svc->cb.log)
        return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svc->cb.log(buf, svc->cb.user_data);
}

void svc_log_detail(edl_service_t *svc, const char *fmt, ...)
{
    if (!svc->cb.log_detail)
        return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svc->cb.log_detail(buf, svc->cb.user_data);
}

uint64_t svc_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

bool svc_is_cancelled(const edl_service_t *svc)
{
    if (!svc)
        return false;
    if (svc->stage_deadline_ms != 0 && svc_now_ms() >= svc->stage_deadline_ms) {
        ((edl_service_t *)svc)->stage_timeout_hit = true;
        return true;
    }
    return svc->cb.is_cancelled && svc->cb.is_cancelled(svc->cb.user_data);
}

bool svc_sleep_cancelable(edl_service_t *svc, int total_ms)
{
    while (total_ms > 0) {
        if (svc_is_cancelled(svc))
            return true;
        int slice = total_ms > 20 ? 20 : total_ms;
        edl_sleep_ms(slice);
        total_ms -= slice;
    }
    return false;
}

static void svc_progress_apply_state(edl_service_t *svc,
                                     const edl_service_progress_state_t *state)
{
    if (!svc || !state)
        return;

    svc->progress_state = *state;
}

static edl_service_progress_state_t svc_progress_capture_state(const edl_service_t *svc)
{
    edl_service_progress_state_t state;

    memset(&state, 0, sizeof(state));
    if (!svc)
        return state;

    return svc->progress_state;
}

void svc_progress_scope_reset(edl_service_t *svc)
{
    edl_service_progress_state_t state;

    if (!svc)
        return;

    memset(&state, 0, sizeof(state));
    svc->progress_scope_depth = 0;
    svc_progress_apply_state(svc, &state);
}

bool svc_progress_scope_push(edl_service_t *svc, bool suppress_core,
                             bool remap_active, int64_t base, int64_t span, int64_t total)
{
    edl_service_progress_state_t next_state;

    if (!svc)
        return false;

    if (svc->progress_scope_depth < EDL_SERVICE_PROGRESS_SCOPE_STACK_MAX) {
        svc->progress_scope_stack[svc->progress_scope_depth++] = svc_progress_capture_state(svc);
    }

    memset(&next_state, 0, sizeof(next_state));
    next_state.suppress_core = suppress_core;
    next_state.remap_active = remap_active && span > 0 && total > 0;

    if (next_state.remap_active) {
        if (base < 0)
            base = 0;
        if (base > total)
            base = total;
        if (span > total - base)
            span = total - base;
        if (span <= 0) {
            next_state.remap_active = false;
        } else {
            next_state.remap_base = base;
            next_state.remap_span = span;
            next_state.remap_total = total;
        }
    }

    svc_progress_apply_state(svc, &next_state);
    return true;
}

void svc_progress_scope_pop(edl_service_t *svc)
{
    if (!svc)
        return;

    if (svc->progress_scope_depth <= 0) {
        svc_progress_scope_reset(svc);
        return;
    }

    svc_progress_apply_state(svc,
                             &svc->progress_scope_stack[svc->progress_scope_depth - 1]);
    svc->progress_scope_depth--;
}

void svc_progress_remap_reset(edl_service_t *svc)
{
    svc_progress_scope_pop(svc);
}

void svc_progress_remap_begin(edl_service_t *svc, int64_t base, int64_t span, int64_t total)
{
    if (!svc || span <= 0 || total <= 0)
        return;

    (void)svc_progress_scope_push(svc, svc->progress_state.suppress_core, true, base, span, total);
}

void svc_progress_set_core_suppressed(edl_service_t *svc, bool suppressed)
{
    if (!svc)
        return;
    svc->progress_state.suppress_core = suppressed;
}

void svc_progress_report(edl_service_t *svc, int64_t current, int64_t total)
{
    if (!svc || !svc->cb.progress || total <= 0)
        return;

    int64_t bounded_current = current;
    if (bounded_current < 0)
        bounded_current = 0;
    if (bounded_current > total)
        bounded_current = total;

    if (svc->progress_state.remap_active
        && svc->progress_state.remap_total > 0
        && svc->progress_state.remap_span > 0) {
        const long double ratio =
            (long double)bounded_current / (long double)total;
        int64_t mapped = svc->progress_state.remap_base
            + (int64_t)(ratio * (long double)svc->progress_state.remap_span);
        const int64_t mapped_end =
            svc->progress_state.remap_base + svc->progress_state.remap_span;
        if (mapped < svc->progress_state.remap_base)
            mapped = svc->progress_state.remap_base;
        if (mapped > mapped_end)
            mapped = mapped_end;
        if (mapped > svc->progress_state.remap_total)
            mapped = svc->progress_state.remap_total;
        svc->cb.progress(mapped, svc->progress_state.remap_total, svc->cb.user_data);
        return;
    }

    svc->cb.progress(bounded_current, total, svc->cb.user_data);
}

static void svc_core_log(const char *msg, void *user_data)
{
    edl_service_t *svc = (edl_service_t *)user_data;
    if (svc && svc->cb.log)
        svc->cb.log(msg, svc->cb.user_data);
}

static void svc_core_log_detail(const char *msg, void *user_data)
{
    edl_service_t *svc = (edl_service_t *)user_data;
    if (svc && svc->cb.log_detail)
        svc->cb.log_detail(msg, svc->cb.user_data);
}

static void svc_core_progress(int64_t current, int64_t total, void *user_data)
{
    edl_service_t *svc = (edl_service_t *)user_data;
    if (!svc || !svc->cb.progress || svc->progress_state.suppress_core)
        return;
    svc_progress_report(svc, current, total);
}

static bool svc_core_is_cancelled(void *user_data)
{
    return svc_is_cancelled((const edl_service_t *)user_data);
}

void svc_init_core_callbacks(edl_service_t *svc)
{
    if (!svc)
        return;

    memset(&svc->core_cb, 0, sizeof(svc->core_cb));
    svc->core_cb.log = svc_core_log;
    svc->core_cb.log_detail = svc_core_log_detail;
    svc->core_cb.progress = svc_core_progress;
    svc->core_cb.is_cancelled = svc_core_is_cancelled;
    svc->core_cb.user_data = svc;
}

static void svc_begin_stage_timeout(edl_service_t *svc, int timeout_ms)
{
    if (!svc)
        return;
    svc->stage_timeout_hit = false;
    svc->stage_deadline_ms = timeout_ms > 0 ? svc_now_ms() + (uint64_t)timeout_ms : 0;
}

static void svc_end_stage_timeout(edl_service_t *svc)
{
    if (svc)
        svc->stage_deadline_ms = 0;
}

static edl_error_t svc_normalize_stage_error(edl_service_t *svc, edl_error_t err)
{
    if (svc && err == EDL_ERR_CANCELLED && svc->stage_timeout_hit)
        return EDL_ERR_TIMEOUT;
    return err;
}

static const svc_stage_runtime_t g_svc_stage_runtime = {
    .now_ms = svc_now_ms,
    .is_cancelled = svc_is_cancelled,
    .sleep_cancelable = svc_sleep_cancelable,
    .begin_timeout = svc_begin_stage_timeout,
    .end_timeout = svc_end_stage_timeout,
    .normalize_error = svc_normalize_stage_error,
    .log = svc_log,
    .log_detail = svc_log_detail,
};

const svc_stage_runtime_t *edl_service_stage_runtime(void)
{
    return &g_svc_stage_runtime;
}

void svc_log_elapsed(edl_service_t *svc, const char *stage, edl_error_t err, uint64_t start_ms)
{
    if (!svc || !stage || start_ms == 0)
        return;

    const uint64_t elapsed = svc_now_ms() - start_ms;
    if (err == EDL_OK) {
        svc_log(svc, "【耗时】%s：%llu ms", stage, (unsigned long long)elapsed);
    } else if (err == EDL_ERR_CANCELLED) {
        svc_log(svc, "【耗时】%s：%llu ms（已取消）", stage, (unsigned long long)elapsed);
    } else {
        svc_log(svc, "【耗时】%s：%llu ms（%s）",
                stage, (unsigned long long)elapsed, edl_error_str(err));
    }
}

void svc_clear_partition_cache(edl_service_t *svc)
{
    if (!svc)
        return;
    svc->partitions_loaded = false;
    svc->partition_count = 0;
    svc->gpt_scan_failed = false;
    svc->gpt_last_scan_err = EDL_OK;
}

edl_service_t *edl_service_create(const edl_callbacks_t *cb)
{
    edl_service_t *svc = (edl_service_t *)calloc(1, sizeof(edl_service_t));
    if (!svc)
        return NULL;
    if (cb)
        svc->cb = *cb;
    svc_init_core_callbacks(svc);
    svc_progress_scope_reset(svc);
    strcpy(svc->storage_type, "ufs");
    return svc;
}

void edl_service_destroy(edl_service_t *svc)
{
    if (!svc)
        return;
    edl_service_disconnect(svc);
    free(svc);
}

void edl_service_disconnect(edl_service_t *svc)
{
    if (!svc)
        return;

    if (svc->firehose) {
        edl_firehose_destroy(svc->firehose);
        svc->firehose = NULL;
    }
    if (svc->sahara) {
        edl_sahara_destroy(svc->sahara);
        svc->sahara = NULL;
    }
    if (svc->port) {
        edl_port_destroy(svc->port);
        svc->port = NULL;
    }

    svc_progress_scope_reset(svc);
    svc->connected = false;
    svc->port_name[0] = '\0';
    svc_clear_partition_cache(svc);
    svc->auth_mode = EDL_SVC_AUTH_NONE;
    svc->stage_deadline_ms = 0;
    svc->stage_timeout_hit = false;
    memset(&svc->loader_info, 0, sizeof(svc->loader_info));
}

bool edl_service_is_connected(const edl_service_t *svc)
{
    return svc && svc->connected;
}
