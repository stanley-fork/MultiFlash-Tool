#include "edl/edl_service.h"

#include "service_internal.h"

#include <string.h>

typedef struct {
    char   *report;
    size_t  report_size;
} svc_storage_report_stage_t;

typedef struct {
    const edl_device_query_options_t *options;
    const edl_device_query_result_t  *result;
} svc_device_query_stage_t;

enum {
    SVC_QUERY_PROGRESS_STORAGE_REPORT = 1,
    SVC_QUERY_PROGRESS_GPT = 3,
    SVC_QUERY_PROGRESS_ANDROID_PROPS = 8
};

static edl_error_t svc_stage_storage_report(edl_service_t *svc, void *ctx)
{
    svc_storage_report_stage_t *stage = (svc_storage_report_stage_t *)ctx;
    if (!stage || !stage->report || stage->report_size == 0)
        return EDL_ERR_INVALID_PARAM;
    return edl_service_get_storage_device_report(svc, stage->report, stage->report_size);
}

static edl_error_t svc_stage_gpt(edl_service_t *svc, void *ctx)
{
    svc_device_query_stage_t *stage = (svc_device_query_stage_t *)ctx;
    if (!stage || !stage->options)
        return EDL_ERR_INVALID_PARAM;

    const edl_device_query_options_t *options = stage->options;
    const edl_device_query_result_t *result = stage->result;

    if (result && result->gpt_parts && result->gpt_count && *result->gpt_count > 0) {
        int max_lun = options->gpt_max_lun;
        if (max_lun <= 0)
            max_lun = edl_service_default_gpt_max_lun(svc);
        return edl_service_read_gpt_ex(svc,
                                       result->gpt_parts,
                                       result->gpt_count,
                                       max_lun,
                                       options->gpt_flags);
    }

    edl_error_t err = edl_service_ensure_gpt_cache_ex(svc, options->gpt_flags);
    if (err == EDL_OK && result && result->gpt_parts && result->gpt_count)
        err = edl_service_copy_cached_gpt(svc, result->gpt_parts, result->gpt_count);
    return err;
}

static edl_error_t svc_stage_android_props(edl_service_t *svc, void *ctx)
{
    svc_device_query_stage_t *stage = (svc_device_query_stage_t *)ctx;
    if (!stage || !stage->result || !stage->result->android_props)
        return EDL_ERR_INVALID_PARAM;

    memset(stage->result->android_props, 0, sizeof(*stage->result->android_props));
    return edl_service_probe_android_build_props(svc, stage->result->android_props);
}

static edl_error_t svc_run_storage_report_stage(edl_service_t *svc,
                                                const svc_stage_runtime_t *runtime,
                                                const edl_device_query_options_t *options,
                                                svc_storage_report_stage_t *ctx)
{
    svc_stage_spec_t stage = {
        .label = "读取字库设备信息",
        .timeout_ms = options->storage_report_timeout_ms,
        .retries = options->storage_report_retries,
        .retry_delay_ms = options->retry_delay_ms,
        .fn = svc_stage_storage_report,
        .ctx = ctx,
    };
    return svc_run_stage(svc, runtime, &stage);
}

static edl_error_t svc_run_gpt_stage(edl_service_t *svc,
                                     const svc_stage_runtime_t *runtime,
                                     const edl_device_query_options_t *options,
                                     svc_device_query_stage_t *ctx)
{
    svc_stage_spec_t stage = {
        .label = "读取分区表",
        .timeout_ms = options->gpt_timeout_ms,
        .retries = options->gpt_retries,
        .retry_delay_ms = options->retry_delay_ms,
        .fn = svc_stage_gpt,
        .ctx = ctx,
    };
    return svc_run_stage(svc, runtime, &stage);
}

static edl_error_t svc_run_android_props_stage(edl_service_t *svc,
                                               const svc_stage_runtime_t *runtime,
                                               const edl_device_query_options_t *options,
                                               svc_device_query_stage_t *ctx)
{
    svc_stage_spec_t stage = {
        .label = "解析 Android 属性",
        .timeout_ms = options->android_props_timeout_ms,
        .retries = options->android_props_retries,
        .retry_delay_ms = options->retry_delay_ms,
        .fn = svc_stage_android_props,
        .ctx = ctx,
    };
    return svc_run_stage(svc, runtime, &stage);
}

void edl_device_query_options_init(edl_device_query_options_t *options)
{
    if (!options)
        return;

    memset(options, 0, sizeof(*options));
    options->read_storage_report = true;
    options->ensure_gpt_cache = false;
    options->read_android_props = false;
    options->storage_report_timeout_ms = 8000;
    options->storage_report_retries = 1;
    options->gpt_timeout_ms = 18000;
    options->gpt_retries = 1;
    options->android_props_timeout_ms = 12000;
    options->android_props_retries = 0;
    options->retry_delay_ms = 180;
    options->gpt_max_lun = 0;
    options->gpt_flags = 0u;
}

edl_error_t edl_service_collect_device_query(edl_service_t *svc,
                                             const edl_device_query_options_t *options,
                                             const edl_device_query_result_t *result)
{
    if (!svc || !options)
        return EDL_ERR_INVALID_PARAM;
    if (!edl_service_is_connected(svc))
        return EDL_ERR_PORT_CLOSED;

    const svc_stage_runtime_t *runtime = edl_service_stage_runtime();
    if (!runtime)
        return EDL_ERR_INVALID_PARAM;

    svc_storage_report_stage_t storage_stage = {
        .report = result ? result->storage_report : NULL,
        .report_size = result ? result->storage_report_size : 0,
    };
    svc_device_query_stage_t query_stage = {
        .options = options,
        .result = result,
    };
    int64_t progress_total = 0;
    int64_t progress_cursor = 0;

    if (options->read_storage_report)
        progress_total += SVC_QUERY_PROGRESS_STORAGE_REPORT;
    if (options->ensure_gpt_cache || options->read_android_props)
        progress_total += SVC_QUERY_PROGRESS_GPT;
    if (options->read_android_props)
        progress_total += SVC_QUERY_PROGRESS_ANDROID_PROPS;

    if (progress_total > 0)
        svc_progress_report(svc, 0, progress_total);

    if (options->read_storage_report) {
        if (!result || !result->storage_report || result->storage_report_size == 0)
            return EDL_ERR_INVALID_PARAM;

        svc_progress_report(svc, progress_cursor, progress_total);
        (void)svc_progress_scope_push(svc, false, true,
                                      progress_cursor,
                                      SVC_QUERY_PROGRESS_STORAGE_REPORT,
                                      progress_total);
        edl_error_t err = svc_run_storage_report_stage(svc, runtime, options, &storage_stage);
        svc_progress_scope_pop(svc);
        if (err != EDL_OK)
            return err;

        progress_cursor += SVC_QUERY_PROGRESS_STORAGE_REPORT;
        svc_progress_report(svc, progress_cursor, progress_total);
    }

    if (options->ensure_gpt_cache || options->read_android_props) {
        svc_progress_report(svc, progress_cursor, progress_total);
        (void)svc_progress_scope_push(svc, false, true,
                                      progress_cursor, SVC_QUERY_PROGRESS_GPT, progress_total);
        edl_error_t err = svc_run_gpt_stage(svc, runtime, options, &query_stage);
        svc_progress_scope_pop(svc);
        if (err != EDL_OK)
            return err;

        progress_cursor += SVC_QUERY_PROGRESS_GPT;
        svc_progress_report(svc, progress_cursor, progress_total);
    }

    if (options->read_android_props) {
        if (!result || !result->android_props)
            return EDL_ERR_INVALID_PARAM;

        svc_progress_report(svc, progress_cursor, progress_total);
        (void)svc_progress_scope_push(svc, true, true,
                                      progress_cursor,
                                      SVC_QUERY_PROGRESS_ANDROID_PROPS,
                                      progress_total);
        edl_error_t err = svc_run_android_props_stage(svc, runtime, options, &query_stage);
        svc_progress_scope_pop(svc);
        if (err != EDL_OK)
            return err;

        progress_cursor += SVC_QUERY_PROGRESS_ANDROID_PROPS;
        svc_progress_report(svc, progress_cursor, progress_total);
    }

    if (progress_total > 0)
        svc_progress_report(svc, progress_total, progress_total);

    return EDL_OK;
}
