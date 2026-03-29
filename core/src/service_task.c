#include "service_task.h"

static bool svc_stage_is_retryable(edl_error_t err)
{
    switch (err) {
    case EDL_ERR_TIMEOUT:
    case EDL_ERR_IO:
    case EDL_ERR_PORT_READ:
    case EDL_ERR_PORT_WRITE:
    case EDL_ERR_FH_NAK:
    case EDL_ERR_FH_XML_PARSE:
    case EDL_ERR_FH_READ:
    case EDL_ERR_GPT_SCAN_EMPTY:
        return true;
    default:
        return false;
    }
}

edl_error_t svc_run_stage(edl_service_t *svc, const svc_stage_runtime_t *runtime,
                          const svc_stage_spec_t *stage)
{
    if (!svc || !runtime || !stage || !stage->fn
        || !runtime->now_ms || !runtime->is_cancelled || !runtime->sleep_cancelable
        || !runtime->begin_timeout || !runtime->end_timeout || !runtime->normalize_error) {
        return EDL_ERR_INVALID_PARAM;
    }

    const int attempts = stage->retries > 0 ? (stage->retries + 1) : 1;
    const char *label = stage->label ? stage->label : "未命名阶段";

    for (int attempt = 1; attempt <= attempts; attempt++) {
        if (runtime->is_cancelled(svc))
            return runtime->normalize_error(svc, EDL_ERR_CANCELLED);

        if ((attempts > 1 || stage->timeout_ms > 0) && runtime->log_detail) {
            runtime->log_detail(svc, "阶段开始：%s（第 %d/%d 次，超时 %d ms）",
                                label, attempt, attempts, stage->timeout_ms);
        }

        const uint64_t start_ms = runtime->now_ms();
        runtime->begin_timeout(svc, stage->timeout_ms);

        edl_error_t err = stage->fn(svc, stage->ctx);
        err = runtime->normalize_error(svc, err);

        runtime->end_timeout(svc);

        if (err == EDL_OK) {
            if (runtime->log_detail) {
                runtime->log_detail(svc, "阶段完成：%s（%llu ms）",
                                    label,
                                    (unsigned long long)(runtime->now_ms() - start_ms));
            }
            return EDL_OK;
        }

        if (err == EDL_ERR_CANCELLED)
            return err;
        if (attempt >= attempts || !svc_stage_is_retryable(err))
            return err;

        if (runtime->log) {
            runtime->log(svc, "阶段重试：%s（第 %d/%d 次）原因：%s",
                         label, attempt + 1, attempts, edl_error_str(err));
        }

        if (runtime->sleep_cancelable(svc, stage->retry_delay_ms > 0 ? stage->retry_delay_ms : 120))
            return runtime->normalize_error(svc, EDL_ERR_CANCELLED);
    }

    return EDL_ERR_TIMEOUT;
}
