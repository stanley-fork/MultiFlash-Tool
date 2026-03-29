#include "service_internal.h"
#include "service_props.h"

#include "edl/slot_detect.h"

#include <string.h>

#ifndef _WIN32
#include <strings.h>
#endif

/* 读分区表成功后自动发 Firehose getddrtype（多数旧 Programmer 无应答、仅超时日志；需时再设为 1） */
#ifndef EDL_ENABLE_GETDDRTYPE_AFTER_GPT
#define EDL_ENABLE_GETDDRTYPE_AFTER_GPT 0
#endif

#if EDL_ENABLE_GETDDRTYPE_AFTER_GPT
static void svc_maybe_try_get_ddr_type_after_gpt(edl_service_t *svc)
{
    if (!svc || !svc->firehose)
        return;
    if (svc->auth_mode != EDL_SVC_AUTH_OPLUS_VIP &&
        svc->auth_mode != EDL_SVC_AUTH_REALME &&
        svc->auth_mode != EDL_SVC_AUTH_ONEPLUS)
        return;

    char ddr[256];
    edl_error_t err = edl_firehose_try_get_ddr_type(svc->firehose, ddr, sizeof(ddr));
    if (err == EDL_OK)
        svc_log(svc, "【getddrtype】%s", ddr);
    else if (err == EDL_ERR_FH_NAK)
        svc_log(svc, "【getddrtype】NAK（当前 Programmer 可能不支持此命令）");
    else
        svc_log(svc, "【getddrtype】跳过（%s）", edl_error_str(err));
}
#endif

static bool svc_storage_is_emmc(const edl_service_t *svc)
{
    if (!svc || !svc->storage_type[0])
        return false;
#ifdef _WIN32
    return _stricmp(svc->storage_type, "emmc") == 0;
#else
    return strcasecmp(svc->storage_type, "emmc") == 0;
#endif
}

static bool svc_partition_name_equals(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs)
        return false;
#ifdef _WIN32
    return _stricmp(lhs, rhs) == 0;
#else
    return strcasecmp(lhs, rhs) == 0;
#endif
}

static edl_error_t svc_erase_named_partition_group(edl_service_t *svc,
                                                   const char *const *candidates,
                                                   const char *target_name)
{
    if (!svc || !svc->connected)
        return EDL_ERR_PORT_CLOSED;

    if (!svc->partitions_loaded)
        svc_log(svc, "未加载分区表，正在读取 GPT 以定位 %s 分区...", target_name);
    else
        svc_log(svc, "已存在分区表缓存，直接查找 %s 分区...", target_name);

    svc_log(svc, "正在擦除 %s...", target_name);

    edl_error_t err = edl_service_ensure_gpt_cache_ex(svc, 0u);
    if (err != EDL_OK)
        return err;

    const edl_partition_info_t *part = NULL;
    for (int i = 0; candidates[i]; i++) {
        part = edl_service_find_partition(svc, candidates[i]);
        if (part)
            break;
    }
    if (!part) {
        svc_log(svc, "未找到 %s 分区", target_name);
        return EDL_ERR_FH_PARTITION_NOT_FOUND;
    }

    err = edl_service_erase_partition(svc, part);
    if (err == EDL_OK)
        svc_log(svc, "%s 擦除成功", target_name);
    else
        svc_log(svc, "%s 擦除失败: %s", target_name, edl_error_str(err));
    return err;
}

int edl_service_default_gpt_max_lun(const edl_service_t *svc)
{
    if (svc_storage_is_emmc(svc))
        return 1;
    if (svc && svc->firehose) {
        int hinted_max_lun = edl_firehose_max_lun_hint(svc->firehose);
        if (hinted_max_lun > 0 && hinted_max_lun < 24)
            return hinted_max_lun;
    }
    return 24;
}

edl_error_t edl_service_read_gpt_ex(edl_service_t *svc,
                                    edl_partition_info_t *parts, int *count, int max_lun,
                                    unsigned flags)
{
    if (!svc || !svc->connected || !svc->firehose || !parts || !count || *count <= 0)
        return EDL_ERR_INVALID_PARAM;

    const int capacity = *count;
    svc_clear_partition_cache(svc);
    *count = capacity;

    if (svc->port)
        edl_port_discard_rx(svc->port);

    (void)edl_firehose_ping(svc->firehose);

    const bool is_emmc = svc_storage_is_emmc(svc);
    int effective_max_lun = max_lun;
    const uint64_t total_start_ms = svc_now_ms();

    if (flags & EDL_SERVICE_GPT_READ_ALLOW_SETBOOTABLE_FALLBACK) {
        svc_log(svc,
                "【安全】已忽略过时的 GPT 兼容标志：不再在读 GPT 时自动发送 setbootablestoragedrive；"
                "启动分区仅允许手动激活或在 patch 后激活");
    }

    if (!is_emmc) {
        const uint64_t storage_hint_start_ms = svc_now_ms();
        edl_error_t hint_err = edl_firehose_get_storage_info(svc->firehose);
        svc_log_elapsed(svc, "GPT 读取前获取存储信息", hint_err, storage_hint_start_ms);
        if (hint_err == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;

        int hinted_max_lun = edl_firehose_max_lun_hint(svc->firehose);
        if (hinted_max_lun > 0 && hinted_max_lun < effective_max_lun) {
            effective_max_lun = hinted_max_lun;
            svc_log_detail(svc, "GPT 扫描范围已按 storage-info 限制为 LUN0-%d",
                           effective_max_lun - 1);
        }
    }

    const uint64_t read_gpt_start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_read_all_gpt(svc->firehose, parts, count, effective_max_lun);
    svc_log_elapsed(svc, "读取 GPT", err, read_gpt_start_ms);
    if (err != EDL_OK) {
        svc->gpt_scan_failed = true;
        svc->gpt_last_scan_err = err;
        if (err == EDL_ERR_GPT_SCAN_EMPTY) {
            svc_log(svc, "GPT：各 LUN 均未解析出分区（读取失败或无 EFI 签名），无法继续按名查找分区");
        }
        svc_log_elapsed(svc, "读 GPT 总流程", err, total_start_ms);
        return err;
    }

    int parsed_count = *count;
    if (parsed_count > EDL_SERVICE_MAX_PARTITIONS)
        parsed_count = EDL_SERVICE_MAX_PARTITIONS;
    if (parts != svc->partitions) {
        memcpy(svc->partitions, parts, (size_t)parsed_count * sizeof(edl_partition_info_t));
    }
    svc->partition_count = parsed_count;
    svc->partitions_loaded = true;
    svc->gpt_scan_failed = false;
    svc->gpt_last_scan_err = EDL_OK;

#if EDL_ENABLE_GETDDRTYPE_AFTER_GPT
    svc_maybe_try_get_ddr_type_after_gpt(svc);
#endif

    svc_log_elapsed(svc, "读 GPT 总流程", EDL_OK, total_start_ms);
    return EDL_OK;
}

edl_error_t edl_service_read_gpt(edl_service_t *svc,
                                 edl_partition_info_t *parts, int *count, int max_lun)
{
    return edl_service_read_gpt_ex(svc, parts, count, max_lun, 0u);
}

edl_error_t edl_service_ensure_gpt_cache_ex(edl_service_t *svc, unsigned flags)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;
    if (svc->partitions_loaded)
        return EDL_OK;

    int count = EDL_SERVICE_MAX_PARTITIONS;
    int max_lun = edl_service_default_gpt_max_lun(svc);
    return edl_service_read_gpt_ex(svc, svc->partitions, &count, max_lun, flags);
}

edl_error_t edl_service_ensure_gpt_cache(edl_service_t *svc)
{
    return edl_service_ensure_gpt_cache_ex(svc, 0u);
}

edl_error_t edl_service_copy_cached_gpt(edl_service_t *svc,
                                        edl_partition_info_t *parts, int *count)
{
    if (!svc || !parts || !count || *count <= 0)
        return EDL_ERR_INVALID_PARAM;
    if (!svc->partitions_loaded)
        return EDL_ERR_GPT_SCAN_EMPTY;

    int copy_count = svc->partition_count;
    if (copy_count > *count)
        copy_count = *count;
    memcpy(parts, svc->partitions, (size_t)copy_count * sizeof(edl_partition_info_t));
    *count = copy_count;
    return EDL_OK;
}

bool edl_service_is_gpt_cache_loaded(const edl_service_t *svc)
{
    return svc && svc->partitions_loaded;
}

const edl_partition_info_t *edl_service_find_partition(edl_service_t *svc, const char *name)
{
    if (!svc || !name)
        return NULL;
    if (!svc->partitions_loaded) {
        if (edl_service_ensure_gpt_cache_ex(svc, 0u) != EDL_OK)
            return NULL;
    }

    for (int i = 0; i < svc->partition_count; i++) {
        if (svc_partition_name_equals(svc->partitions[i].name, name))
            return &svc->partitions[i];
    }
    return NULL;
}

edl_error_t edl_service_probe_android_build_props(edl_service_t *svc, edl_android_props_t *out)
{
    static const svc_prop_probe_runtime_t runtime = {
        .now_ms = svc_now_ms,
        .is_cancelled = svc_is_cancelled,
        .log_detail = svc_log_detail,
        .progress = svc_progress_report,
        .log_elapsed = svc_log_elapsed,
    };

    return edl_service_probe_android_build_props_impl(svc, &runtime, out);
}

edl_error_t edl_service_erase_partition_by_name(edl_service_t *svc, const char *name)
{
    if (!svc || !name)
        return EDL_ERR_INVALID_PARAM;
    if (!svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    edl_error_t err = edl_service_ensure_gpt_cache_ex(svc, 0u);
    if (err != EDL_OK)
        return err;

    const edl_partition_info_t *part = edl_service_find_partition(svc, name);
    if (!part) {
        svc_log(svc, "未找到分区: %s", name);
        return EDL_ERR_FH_PARTITION_NOT_FOUND;
    }
    return edl_service_erase_partition(svc, part);
}

edl_error_t edl_service_erase_frp(edl_service_t *svc)
{
    static const char *const names[] = { "frp", "config", "FRP", NULL };
    return svc_erase_named_partition_group(svc, names, "FRP");
}

edl_error_t edl_service_erase_userdata(edl_service_t *svc)
{
    static const char *const names[] = { "userdata", "USERDATA", NULL };
    return svc_erase_named_partition_group(svc, names, "userdata");
}

edl_error_t edl_service_fix_gpt(edl_service_t *svc)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    edl_error_t ping_err = edl_firehose_ping(svc->firehose);
    if (ping_err != EDL_OK) {
        svc_log(svc, "修复 GPT 前 NOP 失败: %s", edl_error_str(ping_err));
        return ping_err;
    }

    edl_error_t err = edl_firehose_fix_gpt(svc->firehose, -1, true);
    if (err == EDL_OK)
        svc_clear_partition_cache(svc);
    return err;
}

void edl_service_try_set_bootable_storage_drive(edl_service_t *svc, int lun)
{
    if (!svc || !svc->connected || !svc->firehose)
        return;

    edl_firehose_try_set_bootable_storage_drive(svc->firehose, lun);
}

void edl_service_activate_boot_lun_sakura(edl_service_t *svc,
                                          const edl_partition_info_t *parts, int count,
                                          int wrote_a_count, int wrote_b_count)
{
    if (!svc || !svc->connected || !svc->firehose || !parts || count <= 0)
        return;

    char detail[256];
    int lun = edl_boot_lun_pick_sakura(edl_service_storage_type(svc), parts, count,
                                       wrote_a_count, wrote_b_count, detail, sizeof(detail));
    if (lun < 0) {
        svc_log(svc, "启动分区激活: 跳过（%s）", detail[0] ? detail : "无可用 LUN");
        return;
    }

    svc_log(svc, "启动分区激活: %s", detail[0] ? detail : "");
    edl_firehose_try_set_bootable_storage_drive(svc->firehose, lun);
}
