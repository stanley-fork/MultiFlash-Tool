#include "service_props.h"
#include "service_internal.h"

#include "edl/chip_db.h"
#include "edl/fs_prop_probe.h"
#include "edl/ext4_parser.h"
#include "edl/erofs_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <strings.h>
#endif

#ifndef EDL_PROP_PROBE_MAX_BYTES
#define EDL_PROP_PROBE_MAX_BYTES (8 * 1024 * 1024)
#endif
#ifndef EDL_PROP_PROBE_FAST_BYTES
#define EDL_PROP_PROBE_FAST_BYTES (1 * 1024 * 1024)
#endif
#ifndef EDL_PROP_PROBE_MEDIUM_BYTES
#define EDL_PROP_PROBE_MEDIUM_BYTES (4 * 1024 * 1024)
#endif
#ifndef EDL_PROP_PROBE_FAST_SUPER_BYTES
#define EDL_PROP_PROBE_FAST_SUPER_BYTES (8 * 1024 * 1024)
#endif
#ifndef EDL_PROP_PROBE_MEDIUM_SUPER_BYTES
#define EDL_PROP_PROBE_MEDIUM_SUPER_BYTES (32 * 1024 * 1024)
#endif
#ifndef EDL_SYSTEM_VENDOR_PROBE_MAX_BYTES
#define EDL_SYSTEM_VENDOR_PROBE_MAX_BYTES (16 * 1024 * 1024)
#endif
#ifndef EDL_SUPER_PROBE_MAX_BYTES
#define EDL_SUPER_PROBE_MAX_BYTES (64 * 1024 * 1024)
#endif
#ifndef EDL_SUPER_DEEP_WINDOW_BYTES
#define EDL_SUPER_DEEP_WINDOW_BYTES (64 * 1024 * 1024)
#endif
#ifndef EDL_SUPER_DEEP_STEP_BYTES
#define EDL_SUPER_DEEP_STEP_BYTES (48 * 1024 * 1024)
#endif
#ifndef EDL_SUPER_DEEP_MAX_BYTES
#define EDL_SUPER_DEEP_MAX_BYTES (256 * 1024 * 1024)
#endif
#ifndef EDL_OPLUS_VENDOR_PREFIX_BYTES
#define EDL_OPLUS_VENDOR_PREFIX_BYTES (192 * 1024 * 1024)
#endif
#ifndef EDL_OPLUS_VENDOR_WINDOW1_OFFSET_BYTES
#define EDL_OPLUS_VENDOR_WINDOW1_OFFSET_BYTES (28 * 1024 * 1024)
#endif
#ifndef EDL_OPLUS_VENDOR_WINDOW2_OFFSET_BYTES
#define EDL_OPLUS_VENDOR_WINDOW2_OFFSET_BYTES (152 * 1024 * 1024)
#endif
#ifndef EDL_OPLUS_VENDOR_WINDOW1_BYTES
#define EDL_OPLUS_VENDOR_WINDOW1_BYTES (4 * 1024 * 1024)
#endif
#ifndef EDL_OPLUS_VENDOR_WINDOW2_BYTES
#define EDL_OPLUS_VENDOR_WINDOW2_BYTES (8 * 1024 * 1024)
#endif
#ifndef EDL_OPLUS_SYSTEM_PREFIX_BYTES
#define EDL_OPLUS_SYSTEM_PREFIX_BYTES (256 * 1024 * 1024)
#endif
#ifndef EDL_OPLUS_ODM_PREFIX_BYTES
#define EDL_OPLUS_ODM_PREFIX_BYTES (4 * 1024 * 1024)
#endif
#ifndef EDL_PROP_LIVE_FS_MAX_BYTES
#define EDL_PROP_LIVE_FS_MAX_BYTES ((int64_t)16 * 1024 * 1024 * 1024)
#endif
#ifndef EDL_PROP_LIVE_READ_WINDOW_BYTES
#define EDL_PROP_LIVE_READ_WINDOW_BYTES (1024 * 1024)
#endif
#ifndef SVC_PROP_PROGRESS_PER_CANDIDATE
#define SVC_PROP_PROGRESS_PER_CANDIDATE 100
#endif

typedef struct {
    edl_service_t                  *svc;
    const svc_prop_probe_runtime_t *runtime;
} svc_prop_log_bridge_t;

typedef struct {
    edl_service_t                  *svc;
    const svc_prop_probe_runtime_t *runtime;
    int                             candidate_index;
    int                             candidate_total;
    int                             phase_start_pct;
    int                             phase_end_pct;
} svc_prop_fs_progress_t;

typedef struct {
    edl_service_t              *svc;
    const edl_partition_info_t *part;
    const svc_prop_probe_runtime_t *runtime;
    int                         candidate_index;
    int                         candidate_total;
    int                         progress_phase_start_pct;
    int                         progress_phase_end_pct;
    int                         sector_size;
    int64_t                     total_bytes;
    int64_t                     progress_total_bytes;
    int64_t                     progress_reported_bytes;
    int64_t                     cache_offset;
    int                         cache_len;
    uint8_t                    *cache;
    edl_error_t                 last_error;
    uint64_t                    start_ms;
    uint64_t                    budget_ms;
    bool                        budget_exhausted;
} svc_prop_live_reader_t;

static bool svc_prop_probe_name_equals(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs)
        return false;
#ifdef _WIN32
    return _stricmp(lhs, rhs) == 0;
#else
    return strcasecmp(lhs, rhs) == 0;
#endif
}

static bool svc_prop_probe_name_in(const char *name, const char *const *items)
{
    if (!name || !items)
        return false;

    for (int i = 0; items[i]; i++) {
        if (svc_prop_probe_name_equals(name, items[i]))
            return true;
    }
    return false;
}

static bool svc_prop_probe_is_super(const char *name)
{
    static const char *const names[] = { "super", "super_a", "super_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_wants_large_prefix(const char *name)
{
    static const char *const names[] = {
        "system", "system_a", "system_b",
        "vendor", "vendor_a", "vendor_b",
        "product", "product_a", "product_b",
        "my_product", "my_product_a", "my_product_b",
        "system_ext", "system_ext_a", "system_ext_b",
        NULL
    };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_is_system_slot(const char *name)
{
    static const char *const names[] = { "system", "system_a", "system_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_is_vendor_slot(const char *name)
{
    static const char *const names[] = { "vendor", "vendor_a", "vendor_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_is_odm_slot(const char *name)
{
    static const char *const names[] = { "odm", "odm_a", "odm_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_is_product_slot(const char *name)
{
    static const char *const names[] = { "product", "product_a", "product_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_is_system_ext_slot(const char *name)
{
    static const char *const names[] = { "system_ext", "system_ext_a", "system_ext_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_is_my_product_slot(const char *name)
{
    static const char *const names[] = { "my_product", "my_product_a", "my_product_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static bool svc_prop_probe_is_cust_slot(const char *name)
{
    static const char *const names[] = { "cust", "cust_a", "cust_b", NULL };
    return svc_prop_probe_name_in(name, names);
}

static int svc_prop_partition_fallback_priority(const char *name);

static bool svc_prop_probe_prefers_early_live_fs(const char *name)
{
    return svc_prop_probe_is_system_slot(name)
        || svc_prop_probe_is_product_slot(name)
        || svc_prop_probe_is_system_ext_slot(name)
        || svc_prop_probe_is_vendor_slot(name)
        || svc_prop_probe_is_odm_slot(name)
        || svc_prop_probe_is_my_product_slot(name)
        || svc_prop_probe_is_cust_slot(name)
        || svc_prop_partition_fallback_priority(name) >= 180;
}

static int64_t svc_prop_probe_fast_bytes(const char *name)
{
    if (svc_prop_probe_is_super(name))
        return (int64_t)EDL_PROP_PROBE_FAST_SUPER_BYTES;
    if (svc_prop_probe_is_vendor_slot(name) || svc_prop_probe_is_odm_slot(name))
        return (int64_t)EDL_PROP_PROBE_FAST_BYTES;
    if (svc_prop_probe_wants_large_prefix(name))
        return (int64_t)(EDL_PROP_PROBE_FAST_BYTES * 2);
    return (int64_t)EDL_PROP_PROBE_FAST_BYTES;
}

static int64_t svc_prop_probe_medium_bytes(const char *name)
{
    if (svc_prop_probe_is_super(name))
        return (int64_t)EDL_PROP_PROBE_MEDIUM_SUPER_BYTES;
    if (svc_prop_probe_is_vendor_slot(name) || svc_prop_probe_is_odm_slot(name))
        return (int64_t)(EDL_PROP_PROBE_MEDIUM_BYTES / 2);
    return (int64_t)EDL_PROP_PROBE_MEDIUM_BYTES;
}

static int64_t svc_prop_probe_max_bytes(const char *name)
{
    if (svc_prop_probe_is_super(name))
        return (int64_t)EDL_SUPER_PROBE_MAX_BYTES;
    if (svc_prop_probe_is_vendor_slot(name) || svc_prop_probe_is_odm_slot(name))
        return (int64_t)(EDL_PROP_PROBE_MAX_BYTES / 2);
    if (svc_prop_probe_wants_large_prefix(name))
        return (int64_t)(EDL_SYSTEM_VENDOR_PROBE_MAX_BYTES / 2);
    return (int64_t)EDL_PROP_PROBE_MAX_BYTES;
}

static int svc_prop_probe_attempt_bytes(const char *name, int64_t *out_bytes, int capacity)
{
    if (!out_bytes || capacity <= 0)
        return 0;

    const int64_t candidates[] = {
        svc_prop_probe_fast_bytes(name),
        svc_prop_probe_medium_bytes(name),
        svc_prop_probe_max_bytes(name),
    };

    int count = 0;
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])) && count < capacity; i++) {
        const int64_t probe_bytes = candidates[i];
        if (probe_bytes <= 0)
            continue;

        bool duplicate = false;
        for (int k = 0; k < count; k++) {
            if (out_bytes[k] == probe_bytes) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            out_bytes[count++] = probe_bytes;
    }

    return count;
}

static bool svc_prop_starts_with_nocase(const char *value, const char *prefix)
{
    size_t plen = 0;

    if (!value || !prefix)
        return false;
    plen = strlen(prefix);
#ifdef _WIN32
    return _strnicmp(value, prefix, (int)plen) == 0;
#else
    return strncasecmp(value, prefix, plen) == 0;
#endif
}

static bool svc_prop_contains_nocase(const char *value, const char *needle)
{
    size_t nlen = 0;

    if (!value || !needle || !needle[0])
        return false;
    nlen = strlen(needle);
    for (const char *p = value; *p; p++) {
#ifdef _WIN32
        if (_strnicmp(p, needle, (int)nlen) == 0)
            return true;
#else
        if (strncasecmp(p, needle, nlen) == 0)
            return true;
#endif
    }
    return false;
}

static bool svc_prop_token_looks_soc_id(const char *value)
{
    char token[48];
    int tlen = 0;

    if (!value)
        return false;

    while (*value && !isalnum((unsigned char)*value))
        value++;
    while (*value && tlen < (int)sizeof(token) - 1) {
        unsigned char ch = (unsigned char)*value;
        if (!isalnum(ch) && ch != '_' && ch != '-')
            break;
        if (ch != '_' && ch != '-')
            token[tlen++] = (char)tolower(ch);
        value++;
    }
    token[tlen] = '\0';

    if (tlen < 4)
        return false;
    if ((strncmp(token, "sdm", 3) == 0
            || strncmp(token, "msm", 3) == 0
            || strncmp(token, "apq", 3) == 0
            || strncmp(token, "mdm", 3) == 0
            || strncmp(token, "qcs", 3) == 0)
        && isdigit((unsigned char)token[3])) {
        return true;
    }
    if (token[0] == 's' && token[1] == 'm'
        && isdigit((unsigned char)token[2])
        && isdigit((unsigned char)token[3])) {
        return true;
    }
    return false;
}

static bool svc_prop_value_looks_generic(const char *value)
{
    if (!value || !value[0])
        return true;
#ifdef _WIN32
    if (_stricmp(value, "unknown") == 0 || _stricmp(value, "android") == 0
        || _stricmp(value, "aosp") == 0)
        return true;
#else
    if (strcasecmp(value, "unknown") == 0 || strcasecmp(value, "android") == 0
        || strcasecmp(value, "aosp") == 0)
        return true;
#endif
    if (strstr(value, "generic") != NULL || strstr(value, "sdk_gphone") != NULL)
        return true;
    if (svc_prop_starts_with_nocase(value, "qti/")
        || svc_prop_starts_with_nocase(value, "qcom/")
        || svc_prop_starts_with_nocase(value, "qualcomm/"))
        return true;
    if (svc_prop_starts_with_nocase(value, "qti")
        || svc_prop_starts_with_nocase(value, "qcom")
        || svc_prop_starts_with_nocase(value, "qualcomm"))
        return true;
    if (svc_prop_token_looks_soc_id(value))
        return true;
    if (svc_prop_contains_nocase(value, " for arm")
        || svc_prop_contains_nocase(value, " for aarch64"))
        return true;
    return false;
}

static void svc_prop_merge_field(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src || !src[0])
        return;

    if (dst[0] != '\0') {
        if (!svc_prop_value_looks_generic(dst) || svc_prop_value_looks_generic(src))
            return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static void svc_prop_merge_into(edl_android_props_t *dst, const edl_android_props_t *src)
{
    if (!dst || !src)
        return;

    svc_prop_merge_field(dst->brand, sizeof(dst->brand), src->brand);
    svc_prop_merge_field(dst->manufacturer, sizeof(dst->manufacturer), src->manufacturer);
    svc_prop_merge_field(dst->market_name, sizeof(dst->market_name), src->market_name);
    svc_prop_merge_field(dst->locale, sizeof(dst->locale), src->locale);
    svc_prop_merge_field(dst->region_mark, sizeof(dst->region_mark), src->region_mark);
    svc_prop_merge_field(dst->region_type, sizeof(dst->region_type), src->region_type);
    svc_prop_merge_field(dst->model, sizeof(dst->model), src->model);
    svc_prop_merge_field(dst->device, sizeof(dst->device), src->device);
    svc_prop_merge_field(dst->product, sizeof(dst->product), src->product);
    svc_prop_merge_field(dst->android_release, sizeof(dst->android_release), src->android_release);
    svc_prop_merge_field(dst->fingerprint, sizeof(dst->fingerprint), src->fingerprint);
    svc_prop_merge_field(dst->security_patch, sizeof(dst->security_patch), src->security_patch);
    svc_prop_merge_field(dst->build_id, sizeof(dst->build_id), src->build_id);
    svc_prop_merge_field(dst->incremental, sizeof(dst->incremental), src->incremental);
    svc_prop_merge_field(dst->display_id, sizeof(dst->display_id), src->display_id);
    svc_prop_merge_field(dst->build_date, sizeof(dst->build_date), src->build_date);
    svc_prop_merge_field(dst->build_date_utc, sizeof(dst->build_date_utc), src->build_date_utc);
    svc_prop_merge_field(dst->build_type, sizeof(dst->build_type), src->build_type);
    svc_prop_merge_field(dst->build_tags, sizeof(dst->build_tags), src->build_tags);
    svc_prop_merge_field(dst->miui_version, sizeof(dst->miui_version), src->miui_version);
    svc_prop_merge_field(dst->sdk, sizeof(dst->sdk), src->sdk);
    svc_prop_merge_field(dst->ota_version, sizeof(dst->ota_version), src->ota_version);
    svc_prop_merge_field(dst->display_ota, sizeof(dst->display_ota), src->display_ota);
    svc_prop_merge_field(dst->display_full_id, sizeof(dst->display_full_id), src->display_full_id);
    svc_prop_merge_field(dst->common_ota, sizeof(dst->common_ota), src->common_ota);
    svc_prop_merge_field(dst->project_number, sizeof(dst->project_number), src->project_number);
    svc_prop_merge_field(dst->auth_project, sizeof(dst->auth_project), src->auth_project);
    svc_prop_merge_field(dst->hardware_code, sizeof(dst->hardware_code), src->hardware_code);
    svc_prop_merge_field(dst->nv_id, sizeof(dst->nv_id), src->nv_id);
    svc_prop_merge_field(dst->pipeline_key, sizeof(dst->pipeline_key), src->pipeline_key);
    svc_prop_merge_field(dst->base_version, sizeof(dst->base_version), src->base_version);
}

static int svc_prop_score(const edl_android_props_t *props)
{
    int score = 0;

    if (!props)
        return 0;

    if (props->brand[0] && !svc_prop_value_looks_generic(props->brand)) score += 1;
    if (props->manufacturer[0] && !svc_prop_value_looks_generic(props->manufacturer)) score += 1;
    if (props->market_name[0] && !svc_prop_value_looks_generic(props->market_name)) score += 2;
    if (props->model[0] && !svc_prop_value_looks_generic(props->model)) score += 2;
    if (props->device[0] && !svc_prop_value_looks_generic(props->device)) score += 1;
    if (props->product[0] && !svc_prop_value_looks_generic(props->product)) score += 1;
    if (props->android_release[0]) score += 3;
    if (props->fingerprint[0]) score += svc_prop_value_looks_generic(props->fingerprint) ? 1 : 4;
    if (props->security_patch[0]) score += 2;
    if (props->build_id[0]) score += 2;
    if (props->incremental[0]) score += 2;
    if (props->display_id[0]) score += 2;
    if (props->sdk[0]) score += 2;
    if (props->ota_version[0]) score += 2;
    if (props->display_ota[0]) score += 1;
    if (props->display_full_id[0]) score += 2;
    if (props->common_ota[0]) score += 1;
    if (props->project_number[0]) score += 1;
    if (props->auth_project[0]) score += 1;
    if (props->hardware_code[0]) score += 1;
    if (props->nv_id[0]) score += 1;
    if (props->pipeline_key[0]) score += 1;
    if (props->base_version[0]) score += 2;
    return score;
}

static bool svc_prop_core_complete(const edl_android_props_t *props)
{
    if (!props)
        return false;

    return props->brand[0] && !svc_prop_value_looks_generic(props->brand)
        && props->manufacturer[0] && !svc_prop_value_looks_generic(props->manufacturer)
        && props->model[0] && !svc_prop_value_looks_generic(props->model)
        && props->device[0] && !svc_prop_value_looks_generic(props->device)
        && props->product[0] && !svc_prop_value_looks_generic(props->product)
        && props->android_release[0]
        && props->fingerprint[0]
        && props->build_id[0]
        && props->incremental[0]
        && props->display_id[0]
        && props->sdk[0];
}

static bool svc_prop_identity_useful(const edl_android_props_t *props)
{
    if (!props)
        return false;

    return props->brand[0] && !svc_prop_value_looks_generic(props->brand)
        && props->manufacturer[0] && !svc_prop_value_looks_generic(props->manufacturer)
        && props->model[0] && !svc_prop_value_looks_generic(props->model)
        && props->device[0] && !svc_prop_value_looks_generic(props->device)
        && props->product[0] && !svc_prop_value_looks_generic(props->product)
        && (props->android_release[0] || props->sdk[0])
        && (props->fingerprint[0] || props->display_id[0]
            || props->build_id[0] || props->incremental[0]);
}

static bool svc_prop_has_build_context(const edl_android_props_t *props)
{
    if (!props)
        return false;

    return props->android_release[0]
        || props->fingerprint[0]
        || props->security_patch[0]
        || props->build_id[0]
        || props->incremental[0]
        || props->display_id[0]
        || props->sdk[0]
        || props->ota_version[0]
        || props->display_full_id[0]
        || props->base_version[0];
}

static bool svc_prop_oplus_props_complete(const edl_android_props_t *props)
{
    if (!props)
        return false;

    return props->project_number[0]
        && (props->ota_version[0] || props->display_ota[0] || props->display_full_id[0])
        && (props->nv_id[0] || props->base_version[0] || props->auth_project[0]
            || props->pipeline_key[0]);
}

static bool svc_prop_is_oplus_family(const edl_service_t *svc);

static bool svc_prop_candidate_complete(const edl_service_t *svc,
                                        const edl_android_props_t *props)
{
    if (!svc_prop_core_complete(props))
        return false;
    if (!svc_prop_is_oplus_family(svc))
        return true;
    return svc_prop_oplus_props_complete(props);
}

static bool svc_prop_needs_oplus_super_deep_scan(edl_service_t *svc,
                                                 const edl_android_props_t *props)
{
    if (!svc || !svc_prop_is_oplus_family(svc))
        return false;
    if (!props)
        return true;
    return !svc_prop_oplus_props_complete(props);
}

static int svc_prop_super_label_bonus(const char *label)
{
    if (!label || !label[0])
        return 0;
    if (svc_prop_probe_is_my_product_slot(label))
        return 240;
    if (svc_prop_probe_is_product_slot(label))
        return 160;
    if (svc_prop_probe_is_system_ext_slot(label))
        return 120;
    if (svc_prop_probe_is_odm_slot(label))
        return 100;
    if (svc_prop_probe_is_vendor_slot(label))
        return 80;
    if (svc_prop_probe_is_system_slot(label))
        return 40;
    return 0;
}

static void svc_prop_fill_brand_from_market_name(edl_android_props_t *props)
{
    if (!props || !props->market_name[0])
        return;

    if (svc_prop_starts_with_nocase(props->market_name, "realme ")) {
        if (!props->brand[0] || svc_prop_value_looks_generic(props->brand))
            snprintf(props->brand, sizeof(props->brand), "%s", "Realme");
        if (!props->manufacturer[0] || svc_prop_value_looks_generic(props->manufacturer))
            snprintf(props->manufacturer, sizeof(props->manufacturer), "%s", "Realme");
    } else if (svc_prop_starts_with_nocase(props->market_name, "oppo ")) {
        if (!props->brand[0] || svc_prop_value_looks_generic(props->brand))
            snprintf(props->brand, sizeof(props->brand), "%s", "OPPO");
        if (!props->manufacturer[0] || svc_prop_value_looks_generic(props->manufacturer))
            snprintf(props->manufacturer, sizeof(props->manufacturer), "%s", "OPPO");
    } else if (svc_prop_starts_with_nocase(props->market_name, "oneplus ")) {
        if (!props->brand[0] || svc_prop_value_looks_generic(props->brand))
            snprintf(props->brand, sizeof(props->brand), "%s", "OnePlus");
        if (!props->manufacturer[0] || svc_prop_value_looks_generic(props->manufacturer))
            snprintf(props->manufacturer, sizeof(props->manufacturer), "%s", "OnePlus");
    }
}

static void svc_prop_apply_device_knowledge(edl_android_props_t *props)
{
    const char *market_name = NULL;

    if (!props)
        return;

    if (!props->market_name[0] || svc_prop_value_looks_generic(props->market_name)) {
        market_name = edl_device_marketing_name_by_model(props->brand, props->model);
        if (!market_name)
            market_name = edl_device_marketing_name_by_model(props->manufacturer, props->model);
        if (!market_name && props->display_id[0])
            market_name = edl_device_marketing_name_by_model(props->brand, props->display_id);
        if (!market_name && props->display_id[0])
            market_name = edl_device_marketing_name_by_model(props->manufacturer, props->display_id);
        if (market_name)
            snprintf(props->market_name, sizeof(props->market_name), "%s", market_name);
    }

    svc_prop_fill_brand_from_market_name(props);
}

static uint64_t svc_prop_remaining_stage_ms(edl_service_t *svc,
                                            const svc_prop_probe_runtime_t *runtime)
{
    uint64_t now_ms = 0;

    if (!svc || !runtime || !runtime->now_ms || svc->stage_deadline_ms == 0)
        return UINT64_MAX;

    now_ms = runtime->now_ms();
    if (now_ms >= svc->stage_deadline_ms)
        return 0;
    return svc->stage_deadline_ms - now_ms;
}

static bool svc_prop_should_expand_after_hit(const edl_service_t *svc,
                                             const edl_android_props_t *props,
                                             bool is_super)
{
    const int score = svc_prop_score(props);

    if (!props)
        return false;
    if (svc_prop_candidate_complete(svc, props))
        return false;
    if (props->fs_type[0] == '\0')
        return true;
    if (strcmp(props->fs_type, "text_scan") == 0)
        return true;
    if ((!props->fingerprint[0] || !props->android_release[0]) && score < 12)
        return true;
    if ((!props->model[0] || svc_prop_value_looks_generic(props->model)) && score < 14)
        return true;
    if ((!props->brand[0] || !props->manufacturer[0]) && score < 14)
        return true;
    if (is_super && score < 16)
        return true;
    return false;
}

static bool svc_prop_should_expand_candidate_hit(const char *part_name,
                                                 const edl_service_t *svc,
                                                 const edl_android_props_t *props,
                                                 bool is_super)
{
    const int score = props ? svc_prop_score(props) : 0;

    if (!svc_prop_should_expand_after_hit(svc, props, is_super))
        return false;
    if (!part_name || !props)
        return false;

    if (props->fs_type[0]
#ifdef _WIN32
        && _stricmp(props->fs_type, "text_scan") == 0
#else
        && strcasecmp(props->fs_type, "text_scan") == 0
#endif
        && (svc_prop_probe_is_vendor_slot(part_name) || svc_prop_probe_is_odm_slot(part_name))) {
        if (svc_prop_identity_useful(props) || score >= 10)
            return false;
    }

    return true;
}

static void svc_prop_apply_best_source(edl_android_props_t *dst, const edl_android_props_t *best)
{
    if (!dst || !best)
        return;

    snprintf(dst->fs_type, sizeof(dst->fs_type), "%s", best->fs_type);
    snprintf(dst->volume_label, sizeof(dst->volume_label), "%s", best->volume_label);
    snprintf(dst->source_partition, sizeof(dst->source_partition), "%s", best->source_partition);
    dst->fs_embed_offset = best->fs_embed_offset;
}

static bool svc_prop_probe_is_fatal(edl_error_t err)
{
    switch (err) {
    case EDL_ERR_CANCELLED:
    case EDL_ERR_TIMEOUT:
    case EDL_ERR_IO:
    case EDL_ERR_FILE_IO:
    case EDL_ERR_PORT_CLOSED:
    case EDL_ERR_PORT_READ:
    case EDL_ERR_PORT_WRITE:
    case EDL_ERR_INVALID_PARAM:
    case EDL_ERR_NO_MEMORY:
    case EDL_ERR_FH_READ:
        return true;
    default:
        return false;
    }
}

static bool svc_prop_probe_should_keep_partial_on_error(edl_error_t err)
{
    return err == EDL_ERR_TIMEOUT;
}

static bool svc_prop_is_oplus_family(const edl_service_t *svc)
{
    const edl_chip_info_t *chip = edl_service_chip_info(svc);
    const char *brand = NULL;
    const char *oem = NULL;

    if (!chip)
        return false;

    brand = edl_brand_by_ids(chip->oem_id, chip->model_id, chip->pk_hash);
    oem = edl_vendor_by_oem(chip->oem_id);

    return (brand && (strcmp(brand, "Realme") == 0
                   || strcmp(brand, "OnePlus") == 0
                   || strcmp(brand, "OPPO") == 0))
        || (oem && (strcmp(oem, "OPLUS") == 0
                 || strcmp(oem, "OnePlus") == 0
                 || strcmp(oem, "OPPO") == 0
                 || strcmp(oem, "Realme") == 0));
}

static void svc_prop_probe_log_bridge(const char *msg, void *user)
{
    svc_prop_log_bridge_t *bridge = (svc_prop_log_bridge_t *)user;
    if (!bridge || !bridge->svc || !bridge->runtime || !bridge->runtime->log_detail)
        return;

    bridge->runtime->log_detail(bridge->svc, "%s", msg);
}

static void svc_prop_report_progress(edl_service_t *svc,
                                     const svc_prop_probe_runtime_t *runtime,
                                     int candidate_index,
                                     int candidate_total,
                                     int phase_percent)
{
    if (!svc || !runtime || !runtime->progress || candidate_total <= 0)
        return;

    if (candidate_index < 0)
        candidate_index = 0;
    if (candidate_index > candidate_total)
        candidate_index = candidate_total;
    if (phase_percent < 0)
        phase_percent = 0;
    if (phase_percent > SVC_PROP_PROGRESS_PER_CANDIDATE)
        phase_percent = SVC_PROP_PROGRESS_PER_CANDIDATE;

    const int64_t total_units =
        (int64_t)candidate_total * (int64_t)SVC_PROP_PROGRESS_PER_CANDIDATE;
    int64_t current_units =
        (int64_t)candidate_index * (int64_t)SVC_PROP_PROGRESS_PER_CANDIDATE;
    if (candidate_index >= candidate_total)
        current_units = total_units;
    else
        current_units += (int64_t)phase_percent;
    if (current_units > total_units)
        current_units = total_units;

    runtime->progress(svc, current_units, total_units);
}

static void svc_prop_fs_report_progress(int current, int total, void *user)
{
    svc_prop_fs_progress_t *ctx = (svc_prop_fs_progress_t *)user;
    int phase_pct = 0;
    const int phase_span = ctx ? (ctx->phase_end_pct - ctx->phase_start_pct) : 0;

    if (!ctx || !ctx->svc || !ctx->runtime || total <= 0 || phase_span <= 0)
        return;

    if (current < 0)
        current = 0;
    if (current > total)
        current = total;

    phase_pct = ctx->phase_start_pct + (current * phase_span) / total;
    if (phase_pct > ctx->phase_end_pct)
        phase_pct = ctx->phase_end_pct;

    svc_prop_report_progress(ctx->svc, ctx->runtime,
                             ctx->candidate_index, ctx->candidate_total,
                             phase_pct);
}

static int svc_prop_count_candidates(const char *const *candidates)
{
    int count = 0;

    if (!candidates)
        return 0;

    while (candidates[count])
        count++;
    return count;
}

static bool svc_prop_has_super_partition(edl_service_t *svc)
{
    static const char *const names[] = { "super_a", "super_b", "super", NULL };

    if (!svc)
        return false;

    for (int i = 0; names[i]; i++) {
        const edl_partition_info_t *part = edl_service_find_partition(svc, names[i]);
        if (part && part->num_sectors > 0)
            return true;
    }
    return false;
}

static int svc_prop_count_candidates_filtered(const char *const *candidates, bool has_super)
{
    int count = 0;

    if (!candidates)
        return 0;

    for (int i = 0; candidates[i]; i++) {
        if (!has_super && svc_prop_probe_is_super(candidates[i]))
            continue;
        count++;
    }
    return count;
}

static bool svc_prop_candidate_list_contains(const char *const *candidates, const char *name)
{
    if (!candidates || !name || !name[0])
        return false;

    for (int i = 0; candidates[i]; i++) {
        if (svc_prop_probe_name_equals(candidates[i], name))
            return true;
    }
    return false;
}

static bool svc_prop_partition_skip_fallback(const char *name)
{
    if (!name || !name[0])
        return true;

    static const char *const exact_skip[] = {
        "userdata", "metadata", "cache", "misc", "persist", "modem",
        "modemst1", "modemst2", "fsg", "fsc", "ssd", "frp", "devinfo",
        "keystore", "limits", "logfs", "toolsfv", "imagefv", "multiimgoem",
        NULL
    };
    if (svc_prop_probe_name_in(name, exact_skip))
        return true;

    static const char *const prefix_skip[] = {
        "boot", "recovery", "dtbo", "vbmeta", "xbl", "abl", "tz", "rpm",
        "dsp", "hyp", "pmic", "cmnlib", "cmnlib64", "keymaster", "aop",
        "qupfw", "uefisecapp", "sec", "cdt", "ddr", "splash", "logo",
        "storsec", "devcfg", "bluetooth",
        NULL
    };
    for (int i = 0; prefix_skip[i]; i++) {
        const size_t len = strlen(prefix_skip[i]);
#ifdef _WIN32
        if (_strnicmp(name, prefix_skip[i], (int)len) == 0)
            return true;
#else
        if (strncasecmp(name, prefix_skip[i], len) == 0)
            return true;
#endif
    }

    return false;
}

static int svc_prop_partition_fallback_priority(const char *name)
{
    if (!name || !name[0])
        return 0;
    if (svc_prop_partition_skip_fallback(name))
        return 0;

    if (svc_prop_contains_nocase(name, "my_"))
        return 260;
    if (svc_prop_contains_nocase(name, "oppo") || svc_prop_contains_nocase(name, "oplus"))
        return 240;
    if (svc_prop_contains_nocase(name, "reserve") || svc_prop_contains_nocase(name, "version"))
        return 200;
    if (svc_prop_contains_nocase(name, "oem"))
        return 180;
    if (svc_prop_contains_nocase(name, "preload")
        || svc_prop_contains_nocase(name, "custom")
        || svc_prop_contains_nocase(name, "region")
        || svc_prop_contains_nocase(name, "carrier")
        || svc_prop_contains_nocase(name, "engineer")
        || svc_prop_contains_nocase(name, "factory")) {
        return 140;
    }
    return 0;
}

static int svc_prop_collect_fallback_candidates(edl_service_t *svc,
                                                const char *const *primary_candidates,
                                                bool has_super,
                                                const char **out_names,
                                                int capacity)
{
    int count = 0;

    if (!svc || !out_names || capacity <= 0)
        return 0;

    for (int i = 0; i < svc->partition_count && count < capacity; i++) {
        const edl_partition_info_t *part = &svc->partitions[i];
        const char *name = part->name;
        const int priority = svc_prop_partition_fallback_priority(name);
        if (!name[0] || part->num_sectors <= 0)
            continue;
        if (!has_super && svc_prop_probe_is_super(name))
            continue;
        if (svc_prop_candidate_list_contains(primary_candidates, name))
            continue;
        if (priority <= 0)
            continue;

        int insert_at = count;
        if (insert_at > capacity)
            insert_at = capacity;
        for (int k = 0; k < count; k++) {
            const int existing_priority = svc_prop_partition_fallback_priority(out_names[k]);
            if (priority > existing_priority) {
                insert_at = k;
                break;
            }
        }

        if (insert_at >= capacity)
            continue;
        if (count < capacity)
            count++;
        for (int move = count - 1; move > insert_at; move--)
            out_names[move] = out_names[move - 1];
        out_names[insert_at] = name;
    }

    return count;
}

static void svc_prop_live_reader_report_progress(svc_prop_live_reader_t *reader,
                                                 int64_t covered_bytes)
{
    int phase_pct = 0;
    const int phase_span_pct = reader
        ? (reader->progress_phase_end_pct - reader->progress_phase_start_pct)
        : 0;

    if (!reader || !reader->runtime || reader->candidate_total <= 0 || phase_span_pct <= 0)
        return;

    if (covered_bytes < 0)
        covered_bytes = 0;
    if (reader->progress_total_bytes <= 0)
        return;
    if (covered_bytes > reader->progress_total_bytes)
        covered_bytes = reader->progress_total_bytes;
    if (covered_bytes <= reader->progress_reported_bytes)
        return;

    reader->progress_reported_bytes = covered_bytes;
    phase_pct = reader->progress_phase_start_pct
        + (int)((covered_bytes * (int64_t)phase_span_pct) / reader->progress_total_bytes);
    if (phase_pct > reader->progress_phase_end_pct)
        phase_pct = reader->progress_phase_end_pct;

    svc_prop_report_progress(reader->svc, reader->runtime,
                             reader->candidate_index, reader->candidate_total,
                             phase_pct);
}

static int svc_prop_live_reader_window_bytes(const svc_prop_live_reader_t *reader)
{
    int window = 0;

    if (!reader || !reader->svc)
        return EDL_PROP_LIVE_READ_WINDOW_BYTES;

    window = edl_service_max_payload_bytes(reader->svc);
    if (window <= 0 || window > EDL_PROP_LIVE_READ_WINDOW_BYTES)
        window = EDL_PROP_LIVE_READ_WINDOW_BYTES;
    if (reader->sector_size > 0 && window < reader->sector_size)
        window = reader->sector_size;
    return window;
}

static uint64_t svc_prop_live_reader_budget_ms(const char *part_name,
                                               const edl_partition_info_t *part,
                                               int sector_size)
{
    const int64_t total_bytes = (part && sector_size > 0)
        ? (int64_t)part->num_sectors * (int64_t)sector_size
        : 0;
    const bool top_priority_part = part_name
        && (svc_prop_probe_is_system_slot(part_name)
            || svc_prop_probe_is_vendor_slot(part_name)
            || svc_prop_probe_is_my_product_slot(part_name));
    const bool high_value_part = top_priority_part
        || (part_name
            && (svc_prop_probe_is_product_slot(part_name)
                || svc_prop_probe_is_system_ext_slot(part_name)
                || svc_prop_probe_is_odm_slot(part_name)
                || svc_prop_partition_fallback_priority(part_name) >= 180));

    if (total_bytes > 0 && total_bytes <= (int64_t)128 * 1024 * 1024)
        return top_priority_part ? 5000 : (high_value_part ? 3200 : 1100);
    if (total_bytes > 0 && total_bytes <= (int64_t)512 * 1024 * 1024)
        return top_priority_part ? 8000 : (high_value_part ? 4500 : 1500);
    if (total_bytes > 0 && total_bytes <= (int64_t)2 * 1024 * 1024 * 1024)
        return top_priority_part ? 12000 : (high_value_part ? 6500 : 1900);
    if (total_bytes > 0 && total_bytes <= (int64_t)8 * 1024 * 1024 * 1024)
        return top_priority_part ? 15000 : (high_value_part ? 9000 : 2400);
    return top_priority_part ? 18000 : (high_value_part ? 11000 : 2800);
}

static bool svc_prop_live_reader_budget_hit(svc_prop_live_reader_t *reader)
{
    uint64_t now_ms = 0;

    if (!reader || reader->budget_ms == 0 || !reader->runtime || !reader->runtime->now_ms)
        return false;

    now_ms = reader->runtime->now_ms();
    if (now_ms < reader->start_ms)
        return false;
    if ((now_ms - reader->start_ms) < reader->budget_ms)
        return false;

    reader->budget_exhausted = true;
    reader->last_error = EDL_ERR_FILE_NOT_FOUND;
    return true;
}

static void svc_prop_live_reader_close(svc_prop_live_reader_t *reader)
{
    if (!reader)
        return;

    free(reader->cache);
    reader->cache = NULL;
    reader->cache_len = 0;
    reader->cache_offset = 0;
}

static bool svc_prop_live_reader_fill(svc_prop_live_reader_t *reader, int64_t cache_offset)
{
    if (!reader || !reader->svc || !reader->svc->firehose || !reader->part
        || reader->sector_size <= 0 || cache_offset < 0 || cache_offset >= reader->total_bytes)
        return false;
    if (svc_prop_live_reader_budget_hit(reader))
        return false;

    const int window_bytes = svc_prop_live_reader_window_bytes(reader);
    int64_t remaining = reader->total_bytes - cache_offset;
    if (remaining <= 0)
        return false;

    int num_sectors = (int)((remaining + reader->sector_size - 1) / reader->sector_size);
    const int max_window_sectors = (window_bytes + reader->sector_size - 1) / reader->sector_size;
    if (num_sectors > max_window_sectors)
        num_sectors = max_window_sectors;
    if (num_sectors <= 0)
        num_sectors = 1;

    uint8_t *data = NULL;
    int data_len = 0;
    edl_error_t err = edl_firehose_read_sectors(reader->svc->firehose,
                                                reader->part->lun,
                                                reader->part->start_sector
                                                    + (cache_offset / reader->sector_size),
                                                num_sectors,
                                                &data,
                                                &data_len);
    if (err != EDL_OK || !data || data_len <= 0) {
        free(data);
        reader->last_error = (err != EDL_OK) ? err : EDL_ERR_FILE_IO;
        return false;
    }

    free(reader->cache);
    reader->cache = data;
    reader->cache_offset = cache_offset;
    reader->cache_len = data_len;
    if ((int64_t)reader->cache_len > remaining)
        reader->cache_len = (int)remaining;
    reader->last_error = EDL_OK;
    svc_prop_live_reader_report_progress(reader,
                                         reader->cache_offset + (int64_t)reader->cache_len);
    return true;
}

static int svc_prop_live_reader_read(int64_t offset, uint8_t *buf, int len, void *ctx)
{
    svc_prop_live_reader_t *reader = (svc_prop_live_reader_t *)ctx;
    int total_copied = 0;

    if (!reader || !buf || len <= 0)
        return -1;
    if (svc_is_cancelled(reader->svc)) {
        reader->last_error = (reader->svc && reader->svc->stage_timeout_hit)
            ? EDL_ERR_TIMEOUT
            : EDL_ERR_CANCELLED;
        return -1;
    }
    if (svc_prop_live_reader_budget_hit(reader))
        return -1;
    if (offset < 0 || offset >= reader->total_bytes)
        return 0;

    if ((int64_t)len > reader->total_bytes - offset)
        len = (int)(reader->total_bytes - offset);

    while (total_copied < len) {
        const int64_t current_offset = offset + total_copied;
        const int64_t cache_end = reader->cache_offset + reader->cache_len;
        if (svc_prop_live_reader_budget_hit(reader))
            return -1;
        if (!reader->cache || current_offset < reader->cache_offset || current_offset >= cache_end) {
            const int64_t aligned_offset =
                (current_offset / reader->sector_size) * (int64_t)reader->sector_size;
            if (!svc_prop_live_reader_fill(reader, aligned_offset))
                return -1;
            continue;
        }

        const int cache_pos = (int)(current_offset - reader->cache_offset);
        int chunk = reader->cache_len - cache_pos;
        if (chunk <= 0) {
            reader->last_error = EDL_ERR_FILE_IO;
            return -1;
        }
        if (chunk > len - total_copied)
            chunk = len - total_copied;
        memcpy(buf + total_copied, reader->cache + cache_pos, (size_t)chunk);
        total_copied += chunk;
    }

    return total_copied;
}

static int svc_prop_ext4_read_text(void *parser, const char *path, char *buf, int buf_size)
{
    return ext4_read_text((ext4_parser_t *)parser, path, buf, buf_size);
}

static int svc_prop_erofs_read_text(void *parser, const char *path, char *buf, int buf_size)
{
    return erofs_read_text((erofs_parser_t *)parser, path, buf, buf_size);
}

static edl_error_t svc_prop_probe_partition_live_fs(edl_service_t *svc,
                                                    const svc_prop_probe_runtime_t *runtime,
                                                    const char *part_name,
                                                    const edl_partition_info_t *part,
                                                    int candidate_index,
                                                    int candidate_total,
                                                    int phase_start_pct,
                                                    int phase_end_pct,
                                                    edl_android_props_t *out)
{
    if (!svc || !runtime || !part_name || !part || !out || !svc->firehose)
        return EDL_ERR_INVALID_PARAM;

    svc_prop_live_reader_t reader;
    svc_prop_log_bridge_t bridge = {
        .svc = svc,
        .runtime = runtime,
    };
    const int io_phase_end_pct =
        phase_start_pct + ((phase_end_pct - phase_start_pct) * 2) / 3;
    svc_prop_fs_progress_t fs_progress = {
        .svc = svc,
        .runtime = runtime,
        .candidate_index = candidate_index,
        .candidate_total = candidate_total,
        .phase_start_pct = io_phase_end_pct,
        .phase_end_pct = phase_end_pct,
    };

    memset(&reader, 0, sizeof(reader));
    reader.svc = svc;
    reader.part = part;
    reader.runtime = runtime;
    reader.candidate_index = candidate_index;
    reader.candidate_total = candidate_total;
    reader.progress_phase_start_pct = phase_start_pct;
    reader.progress_phase_end_pct = io_phase_end_pct;
    reader.sector_size = edl_service_sector_size(svc);
    reader.total_bytes = (int64_t)part->num_sectors * (int64_t)reader.sector_size;
    reader.progress_total_bytes = reader.total_bytes;
    reader.progress_reported_bytes = 0;
    reader.last_error = EDL_OK;
    reader.start_ms = runtime->now_ms ? runtime->now_ms() : 0;
    reader.budget_ms = svc_prop_live_reader_budget_ms(part_name, part, reader.sector_size);

    if (reader.sector_size <= 0 || reader.total_bytes <= 0)
        return EDL_ERR_INVALID_PARAM;

    svc_prop_live_reader_report_progress(&reader, 0);

    if (ext4_detect(svc_prop_live_reader_read, &reader)) {
        ext4_parser_t *parser = ext4_open(svc_prop_live_reader_read,
                                          &reader,
                                          svc_prop_probe_log_bridge,
                                          &bridge);
        if (parser) {
            snprintf(out->fs_type, sizeof(out->fs_type), "%s", "ext4");
            snprintf(out->volume_label, sizeof(out->volume_label), "%s", ext4_volume_name(parser));
            snprintf(out->source_partition, sizeof(out->source_partition), "%s", part_name);
            if (edl_probe_android_props_from_filesystem_ex(parser,
                                                           svc_prop_ext4_read_text,
                                                           "ext4",
                                                           out,
                                                           svc_prop_probe_log_bridge,
                                                           &bridge,
                                                           svc_prop_fs_report_progress,
                                                           &fs_progress)) {
                snprintf(out->source_partition, sizeof(out->source_partition), "%s", part_name);
                out->fs_embed_offset = 0;
                ext4_close(parser);
                svc_prop_live_reader_close(&reader);
                return EDL_OK;
            }
            ext4_close(parser);
            memset(out, 0, sizeof(*out));
        }
        if (reader.last_error != EDL_OK) {
            edl_error_t err = reader.last_error;
            svc_prop_live_reader_close(&reader);
            return err;
        }
    } else if (reader.last_error != EDL_OK) {
        edl_error_t err = reader.last_error;
        svc_prop_live_reader_close(&reader);
        return err;
    }

    reader.last_error = EDL_OK;
    if (erofs_detect(svc_prop_live_reader_read, &reader)) {
        erofs_parser_t *parser = erofs_open(svc_prop_live_reader_read,
                                            &reader,
                                            svc_prop_probe_log_bridge,
                                            &bridge);
        if (parser) {
            snprintf(out->fs_type, sizeof(out->fs_type), "%s", "erofs");
            snprintf(out->volume_label, sizeof(out->volume_label), "%s", erofs_volume_name(parser));
            snprintf(out->source_partition, sizeof(out->source_partition), "%s", part_name);
            if (edl_probe_android_props_from_filesystem_ex(parser,
                                                           svc_prop_erofs_read_text,
                                                           "erofs",
                                                           out,
                                                           svc_prop_probe_log_bridge,
                                                           &bridge,
                                                           svc_prop_fs_report_progress,
                                                           &fs_progress)) {
                snprintf(out->source_partition, sizeof(out->source_partition), "%s", part_name);
                out->fs_embed_offset = 0;
                erofs_close(parser);
                svc_prop_live_reader_close(&reader);
                return EDL_OK;
            }
            erofs_close(parser);
            memset(out, 0, sizeof(*out));
        }
        if (reader.last_error != EDL_OK) {
            edl_error_t err = reader.last_error;
            svc_prop_live_reader_close(&reader);
            return err;
        }
    } else if (reader.last_error != EDL_OK) {
        edl_error_t err = reader.last_error;
        svc_prop_live_reader_close(&reader);
        return err;
    }

    if (reader.budget_exhausted) {
        svc_prop_live_reader_close(&reader);
        return EDL_ERR_FILE_NOT_FOUND;
    }

    svc_prop_live_reader_close(&reader);
    return EDL_ERR_FILE_NOT_FOUND;
}

static edl_error_t svc_prop_probe_partition_prefix(edl_service_t *svc,
                                                   const svc_prop_probe_runtime_t *runtime,
                                                   const char *part_name,
                                                   const edl_partition_info_t *part,
                                                   int64_t probe_bytes,
                                                   bool scan_embedded,
                                                   edl_android_props_t *out)
{
    if (!svc || !runtime || !part_name || !part || !out || probe_bytes <= 0)
        return EDL_ERR_INVALID_PARAM;

    const int sector_size = edl_service_sector_size(svc);
    if (sector_size <= 0)
        return EDL_ERR_INVALID_PARAM;

    int64_t max_sec = probe_bytes / (int64_t)sector_size;
    if (max_sec < 1)
        max_sec = 1;

    edl_partition_info_t partial = *part;
    if (partial.num_sectors > max_sec)
        partial.num_sectors = max_sec;

    uint8_t *buf = NULL;
    int blen = 0;
    edl_error_t err = edl_service_read_partition_mem(svc, &partial, &buf, &blen);
    if (err != EDL_OK || !buf || blen < 1024) {
        if (runtime->log_detail && err != EDL_OK) {
            runtime->log_detail(svc, "读取 %s 前缀失败：%s", part_name, edl_error_str(err));
        }
        free(buf);
        return err != EDL_OK ? err : EDL_ERR_FILE_NOT_FOUND;
    }

    if (runtime->log_detail) {
        runtime->log_detail(svc, "正在探测 %s：读取前缀 %u 字节%s",
                            part_name,
                            (unsigned)blen,
                            scan_embedded ? "，允许扫描内嵌文件系统" : "");
    }

    int64_t fs_off = 0;
    svc_prop_log_bridge_t bridge = {
        .svc = svc,
        .runtime = runtime,
    };
    const bool ok = edl_probe_android_props_from_buffer_scanned(
        buf, blen, out, &fs_off, scan_embedded, svc_prop_probe_log_bridge, &bridge);
    free(buf);

    if (!ok)
        return EDL_ERR_FILE_NOT_FOUND;

    snprintf(out->source_partition, sizeof(out->source_partition), "%s", part_name);
    out->fs_embed_offset = fs_off;

    if (runtime->log_detail) {
        if (fs_off != 0) {
            runtime->log_detail(svc,
                                "已命中 build.prop：%s | 文件系统=%s | 卷标=%s | 内嵌偏移=%lld",
                                out->source_partition,
                                out->fs_type,
                                out->volume_label,
                                (long long)fs_off);
        } else {
            runtime->log_detail(svc,
                                "已命中 build.prop：%s | 文件系统=%s | 卷标=%s",
                                out->source_partition,
                                out->fs_type,
                                out->volume_label);
        }
    }

    return EDL_OK;
}

static edl_error_t svc_prop_probe_super_window(edl_service_t *svc,
                                               const svc_prop_probe_runtime_t *runtime,
                                               const char *part_name,
                                               const edl_partition_info_t *part,
                                               int64_t offset_bytes,
                                               int64_t window_bytes,
                                               edl_android_props_t *out)
{
    if (!svc || !runtime || !part_name || !part || !out || window_bytes <= 0)
        return EDL_ERR_INVALID_PARAM;

    const int sector_size = edl_service_sector_size(svc);
    if (sector_size <= 0)
        return EDL_ERR_INVALID_PARAM;

    const int64_t part_bytes = (int64_t)part->num_sectors * (int64_t)sector_size;
    if (offset_bytes < 0 || offset_bytes >= part_bytes)
        return EDL_ERR_FILE_NOT_FOUND;

    const int64_t aligned_offset_bytes =
        (offset_bytes / (int64_t)sector_size) * (int64_t)sector_size;
    const int64_t remaining_bytes = part_bytes - aligned_offset_bytes;
    if (remaining_bytes <= 0)
        return EDL_ERR_FILE_NOT_FOUND;

    int64_t read_bytes = window_bytes;
    if (read_bytes > remaining_bytes)
        read_bytes = remaining_bytes;

    edl_partition_info_t partial = *part;
    partial.start_sector += aligned_offset_bytes / (int64_t)sector_size;
    partial.num_sectors = (read_bytes + sector_size - 1) / sector_size;

    uint8_t *buf = NULL;
    int blen = 0;
    edl_error_t err = edl_service_read_partition_mem(svc, &partial, &buf, &blen);
    if (err != EDL_OK || !buf || blen < 1024) {
        free(buf);
        return err != EDL_OK ? err : EDL_ERR_FILE_NOT_FOUND;
    }

    svc_prop_log_bridge_t bridge = {
        .svc = svc,
        .runtime = runtime,
    };
    int64_t fs_off = 0;
    const bool ok = edl_probe_android_props_from_buffer_scanned(
        buf, blen, out, &fs_off, true, svc_prop_probe_log_bridge, &bridge);
    free(buf);

    if (!ok)
        return EDL_ERR_FILE_NOT_FOUND;

    snprintf(out->source_partition, sizeof(out->source_partition), "%s", part_name);
    out->fs_embed_offset = aligned_offset_bytes + fs_off;
    return EDL_OK;
}

static edl_error_t svc_prop_probe_partition_window_raw(edl_service_t *svc,
                                                       const svc_prop_probe_runtime_t *runtime,
                                                       const char *part_name,
                                                       const edl_partition_info_t *part,
                                                       int64_t offset_bytes,
                                                       int64_t window_bytes,
                                                       edl_android_props_t *out)
{
    if (!svc || !runtime || !part_name || !part || !out || window_bytes <= 0)
        return EDL_ERR_INVALID_PARAM;

    const int sector_size = edl_service_sector_size(svc);
    if (sector_size <= 0)
        return EDL_ERR_INVALID_PARAM;

    const int64_t part_bytes = (int64_t)part->num_sectors * (int64_t)sector_size;
    if (offset_bytes < 0 || offset_bytes >= part_bytes)
        return EDL_ERR_FILE_NOT_FOUND;

    const int64_t aligned_offset_bytes =
        (offset_bytes / (int64_t)sector_size) * (int64_t)sector_size;
    const int64_t remaining_bytes = part_bytes - aligned_offset_bytes;
    if (remaining_bytes <= 0)
        return EDL_ERR_FILE_NOT_FOUND;

    int64_t read_bytes = window_bytes;
    if (read_bytes > remaining_bytes)
        read_bytes = remaining_bytes;

    edl_partition_info_t partial = *part;
    partial.start_sector += aligned_offset_bytes / (int64_t)sector_size;
    partial.num_sectors = (read_bytes + sector_size - 1) / sector_size;

    uint8_t *buf = NULL;
    int blen = 0;
    edl_error_t err = edl_service_read_partition_mem(svc, &partial, &buf, &blen);
    if (err != EDL_OK || !buf || blen < 1024) {
        free(buf);
        return err != EDL_OK ? err : EDL_ERR_FILE_NOT_FOUND;
    }

    svc_prop_log_bridge_t bridge = {
        .svc = svc,
        .runtime = runtime,
    };
    int64_t fs_off = 0;
    const bool ok = edl_probe_android_props_from_buffer_scanned(
        buf, blen, out, &fs_off, false, svc_prop_probe_log_bridge, &bridge);
    free(buf);

    if (!ok)
        return EDL_ERR_FILE_NOT_FOUND;

    snprintf(out->source_partition, sizeof(out->source_partition), "%s", part_name);
    out->fs_embed_offset = aligned_offset_bytes + fs_off;
    return EDL_OK;
}

static edl_error_t svc_prop_probe_super_deep(edl_service_t *svc,
                                             const svc_prop_probe_runtime_t *runtime,
                                             const char *part_name,
                                             const edl_partition_info_t *part,
                                             int candidate_index,
                                             int candidate_total,
                                             const edl_android_props_t *seed,
                                             edl_android_props_t *out)
{
    if (!svc || !runtime || !part_name || !part || !out)
        return EDL_ERR_INVALID_PARAM;

    const int sector_size = edl_service_sector_size(svc);
    if (sector_size <= 0)
        return EDL_ERR_INVALID_PARAM;

    const int64_t part_bytes = (int64_t)part->num_sectors * (int64_t)sector_size;
    if (part_bytes <= 0)
        return EDL_ERR_FILE_NOT_FOUND;

    int64_t max_scan_bytes = part_bytes;
    if (max_scan_bytes > (int64_t)EDL_SUPER_DEEP_MAX_BYTES)
        max_scan_bytes = (int64_t)EDL_SUPER_DEEP_MAX_BYTES;

    const int64_t window_bytes = (int64_t)EDL_SUPER_DEEP_WINDOW_BYTES;
    const int64_t step_bytes = (int64_t)EDL_SUPER_DEEP_STEP_BYTES;

    edl_android_props_t best;
    bool found = false;
    int best_score = 0;
    memset(&best, 0, sizeof(best));

    for (int64_t offset_bytes = 0; offset_bytes < max_scan_bytes; offset_bytes += step_bytes) {
        const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
        if (remaining_ms != UINT64_MAX) {
            if (!found && remaining_ms <= 800)
                break;
            if (found && remaining_ms <= 1400)
                break;
        }

        int phase_pct = 82;
        if (max_scan_bytes > 0) {
            phase_pct += (int)((offset_bytes * 16) / max_scan_bytes);
            if (phase_pct > 98)
                phase_pct = 98;
        }
        svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, phase_pct);

        edl_android_props_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        edl_error_t err = svc_prop_probe_super_window(svc, runtime, part_name, part,
                                                      offset_bytes, window_bytes, &candidate);
        if (err == EDL_OK) {
            edl_android_props_t merged = candidate;
            if (seed)
                svc_prop_merge_into(&merged, seed);

            const int score = svc_prop_score(&merged)
                + svc_prop_super_label_bonus(candidate.volume_label);
            if (!found || score > best_score) {
                best = merged;
                best_score = score;
                found = true;
            }

            if (runtime->log_detail) {
                runtime->log_detail(svc,
                                    "super 深扫命中：%s @ 0x%llX | 分区=%s | 文件系统=%s",
                                    candidate.volume_label[0] ? candidate.volume_label : "(unknown)",
                                    (unsigned long long)candidate.fs_embed_offset,
                                    part_name,
                                    candidate.fs_type);
            }

            if (svc_prop_oplus_props_complete(&merged)) {
                *out = merged;
                return EDL_OK;
            }
        } else if (svc_prop_probe_is_fatal(err)) {
            return err;
        }
    }

    if (found) {
        *out = best;
        return EDL_OK;
    }
    return EDL_ERR_FILE_NOT_FOUND;
}

static edl_error_t svc_prop_try_live_fs_merge(edl_service_t *svc,
                                              const svc_prop_probe_runtime_t *runtime,
                                              const char *part_name,
                                              const edl_partition_info_t *part,
                                              bool is_super,
                                              int64_t part_bytes,
                                              int candidate_index,
                                              int candidate_total,
                                              const edl_android_props_t *seed,
                                              edl_android_props_t *out)
{
    const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
    const int sector_size = edl_service_sector_size(svc);
    const uint64_t live_budget_ms = svc_prop_live_reader_budget_ms(part_name, part, sector_size);

    if (!svc || !runtime || !part_name || !part || !out)
        return EDL_ERR_INVALID_PARAM;
    if (is_super || part_bytes <= 0 || part_bytes > (int64_t)EDL_PROP_LIVE_FS_MAX_BYTES)
        return EDL_ERR_FILE_NOT_FOUND;
    if (remaining_ms != UINT64_MAX && remaining_ms <= (live_budget_ms + 300))
        return EDL_ERR_FILE_NOT_FOUND;

    if (seed)
        *out = *seed;
    else
        memset(out, 0, sizeof(*out));
    edl_error_t err = svc_prop_probe_partition_live_fs(svc, runtime, part_name, part,
                                                       candidate_index, candidate_total,
                                                       80, 95, out);
    if (err == EDL_OK && seed)
        svc_prop_merge_into(out, seed);
    return err;
}

static edl_error_t svc_prop_probe_candidate(edl_service_t *svc,
                                            const svc_prop_probe_runtime_t *runtime,
                                            const char *part_name,
                                            int candidate_index,
                                            int candidate_total,
                                            const edl_android_props_t *seed,
                                            edl_android_props_t *out)
{
    const edl_partition_info_t *part = edl_service_find_partition(svc, part_name);
    const int sector_size = edl_service_sector_size(svc);
    const int64_t part_bytes = (part && sector_size > 0)
        ? (int64_t)part->num_sectors * (int64_t)sector_size
        : 0;
    svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 0);
    if (!part || part->num_sectors <= 0)
    {
        svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
        return EDL_ERR_FH_PARTITION_NOT_FOUND;
    }

    const bool is_super = svc_prop_probe_is_super(part_name);
    int64_t attempts[3];
    const int attempt_count = svc_prop_probe_attempt_bytes(part_name, attempts, 3);
    int64_t last_bytes = 0;
    edl_android_props_t best;
    bool found = false;
    bool live_merge_attempted = false;
    int best_score = 0;

    memset(&best, 0, sizeof(best));

    for (int attempt = 0; attempt < attempt_count; attempt++) {
        const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
        int64_t probe_bytes = attempts[attempt];
        const int64_t max_bytes = svc_prop_probe_max_bytes(part_name);
        const int attempt_start_pct = 10 + (attempt * 60) / attempt_count;
        const int attempt_done_pct = 10 + ((attempt + 1) * 60) / attempt_count;
        if (probe_bytes <= 0 || probe_bytes == last_bytes)
            continue;
        if (found && remaining_ms != UINT64_MAX && remaining_ms <= 600) {
            *out = best;
            svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
            return EDL_OK;
        }
        if (!found && remaining_ms != UINT64_MAX && remaining_ms <= 250)
            break;
        if (probe_bytes > max_bytes)
            probe_bytes = max_bytes;
        if (probe_bytes == last_bytes)
            continue;

        svc_prop_report_progress(svc, runtime, candidate_index, candidate_total,
                                 attempt_start_pct);

        if (attempt > 0 && runtime->log_detail) {
            runtime->log_detail(svc, "扩展 %s 探测范围：%u -> %u 字节",
                                part_name,
                                (unsigned)last_bytes,
                                (unsigned)probe_bytes);
        }

        memset(out, 0, sizeof(*out));
        edl_error_t err = svc_prop_probe_partition_prefix(svc, runtime, part_name, part,
                                                          probe_bytes, is_super, out);
        if (err == EDL_OK) {
            const int score = svc_prop_score(out);
            if (!found || score >= best_score) {
                best = *out;
                best_score = score;
                found = true;
            }
            svc_prop_report_progress(svc, runtime, candidate_index, candidate_total,
                                     attempt_done_pct);
            if (!svc_prop_candidate_complete(svc, out) && !live_merge_attempted) {
                edl_android_props_t live_props;
                edl_error_t live_err;

                live_merge_attempted = true;
                live_err = svc_prop_try_live_fs_merge(svc, runtime, part_name, part,
                                                      is_super, part_bytes,
                                                      candidate_index, candidate_total,
                                                      out, &live_props);
                if (live_err == EDL_OK) {
                    *out = live_props;
                    svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                    return EDL_OK;
                }
                if (svc_prop_probe_is_fatal(live_err)) {
                    if (svc_prop_probe_should_keep_partial_on_error(live_err)) {
                        *out = best;
                        svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                        return EDL_OK;
                    }
                    svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                    return live_err;
                }
            }
            if (attempt + 1 < attempt_count
                && svc_prop_should_expand_candidate_hit(part_name, svc, out, is_super)
                && (remaining_ms == UINT64_MAX || remaining_ms > 2500)) {
                last_bytes = probe_bytes;
                continue;
            }
            svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
            return EDL_OK;
        }
        if (svc_prop_probe_is_fatal(err))
        {
            if (found && svc_prop_probe_should_keep_partial_on_error(err)) {
                *out = best;
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return EDL_OK;
            }
            svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
            return err;
        }

        if (!found
            && !live_merge_attempted
            && attempt == 0
            && svc_prop_probe_prefers_early_live_fs(part_name)) {
            edl_error_t live_err =
                svc_prop_try_live_fs_merge(svc, runtime, part_name, part,
                                           is_super, part_bytes,
                                           candidate_index, candidate_total,
                                           seed, out);
            live_merge_attempted = true;
            if (live_err == EDL_OK) {
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return EDL_OK;
            }
            if (svc_prop_probe_is_fatal(live_err)) {
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return live_err;
            }
        }

        last_bytes = probe_bytes;
        svc_prop_report_progress(svc, runtime, candidate_index, candidate_total,
                                 attempt_done_pct);
    }

    if (is_super && svc_prop_needs_oplus_super_deep_scan(svc, found ? &best : seed)) {
        edl_android_props_t deep_props;
        const edl_android_props_t *deep_seed = found ? &best : seed;
        memset(&deep_props, 0, sizeof(deep_props));

        edl_error_t deep_err = svc_prop_probe_super_deep(svc, runtime, part_name, part,
                                                         candidate_index, candidate_total,
                                                         deep_seed, &deep_props);
        if (deep_err == EDL_OK) {
            *out = deep_props;
            svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
            return EDL_OK;
        }
        if (svc_prop_probe_is_fatal(deep_err)) {
            if (found && svc_prop_probe_should_keep_partial_on_error(deep_err)) {
                *out = best;
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return EDL_OK;
            }
            svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
            return deep_err;
        }
    }

    if (found) {
        *out = best;
        if (!svc_prop_candidate_complete(svc, &best) && !live_merge_attempted) {
            edl_android_props_t live_props;
            edl_error_t live_err = svc_prop_try_live_fs_merge(svc, runtime, part_name, part,
                                                              is_super, part_bytes,
                                                              candidate_index, candidate_total,
                                                              &best, &live_props);
            if (live_err == EDL_OK) {
                *out = live_props;
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return EDL_OK;
            }
            if (svc_prop_probe_is_fatal(live_err)) {
                if (svc_prop_probe_should_keep_partial_on_error(live_err)) {
                    *out = best;
                    svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                    return EDL_OK;
                }
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return live_err;
            }
        }
        svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
        return EDL_OK;
    }

    {
        const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
        if (!live_merge_attempted
            && !is_super
            && part_bytes > 0
            && (remaining_ms == UINT64_MAX || remaining_ms > 1200)) {
            edl_error_t live_err =
                svc_prop_try_live_fs_merge(svc, runtime, part_name, part,
                                           is_super, part_bytes,
                                           candidate_index, candidate_total,
                                           seed, out);
            if (live_err == EDL_OK)
            {
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return EDL_OK;
            }
            if (svc_prop_probe_is_fatal(live_err))
            {
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
                return live_err;
            }
            if (live_err != EDL_ERR_FILE_NOT_FOUND)
                svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 95);
        }
    }

    if (svc_prop_probe_is_system_slot(part_name) && runtime->log_detail) {
        runtime->log_detail(svc, "未在 %s 前缀中发现 build.prop，继续尝试下一个候选分区",
                            part_name);
    }

    svc_prop_report_progress(svc, runtime, candidate_index, candidate_total, 100);
    return EDL_ERR_FILE_NOT_FOUND;
}

static bool svc_prop_oplus_needs_vendor_overlay(const edl_android_props_t *props)
{
    if (!props)
        return true;
    return !props->ota_version[0]
        || !props->display_ota[0]
        || !props->display_full_id[0]
        || !props->nv_id[0]
        || !props->pipeline_key[0]
        || !props->base_version[0];
}

static bool svc_prop_oplus_needs_system_overlay(const edl_android_props_t *props)
{
    if (!props)
        return true;
    return !props->android_release[0]
        || !props->security_patch[0]
        || !props->build_id[0]
        || !props->incremental[0]
        || !props->sdk[0]
        || !props->common_ota[0]
        || !props->auth_project[0]
        || !props->hardware_code[0];
}

static bool svc_prop_oplus_needs_odm_overlay(const edl_android_props_t *props)
{
    if (!props)
        return true;
    return !props->market_name[0]
        || !props->display_id[0];
}

static void svc_prop_overlay_if_present(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src || !src[0])
        return;
    snprintf(dst, dst_size, "%s", src);
}

static void svc_prop_overlay_if_better(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src || !src[0])
        return;
    if (!dst[0] || (svc_prop_value_looks_generic(dst) && !svc_prop_value_looks_generic(src)))
        snprintf(dst, dst_size, "%s", src);
}

static void svc_prop_apply_oplus_partition_overlay(edl_android_props_t *dst,
                                                   const edl_android_props_t *src,
                                                   const char *part_name)
{
    if (!dst || !src || !part_name)
        return;

    svc_prop_merge_into(dst, src);

    if (svc_prop_probe_is_vendor_slot(part_name)) {
        svc_prop_overlay_if_present(dst->ota_version, sizeof(dst->ota_version), src->ota_version);
        svc_prop_overlay_if_present(dst->display_ota, sizeof(dst->display_ota), src->display_ota);
        svc_prop_overlay_if_present(dst->display_full_id, sizeof(dst->display_full_id), src->display_full_id);
        svc_prop_overlay_if_present(dst->display_id, sizeof(dst->display_id), src->display_id);
        svc_prop_overlay_if_present(dst->nv_id, sizeof(dst->nv_id), src->nv_id);
        svc_prop_overlay_if_present(dst->pipeline_key, sizeof(dst->pipeline_key), src->pipeline_key);
        svc_prop_overlay_if_present(dst->base_version, sizeof(dst->base_version), src->base_version);
        svc_prop_overlay_if_better(dst->android_release, sizeof(dst->android_release), src->android_release);
        svc_prop_overlay_if_better(dst->security_patch, sizeof(dst->security_patch), src->security_patch);
        svc_prop_overlay_if_better(dst->build_id, sizeof(dst->build_id), src->build_id);
        svc_prop_overlay_if_better(dst->incremental, sizeof(dst->incremental), src->incremental);
        svc_prop_overlay_if_better(dst->sdk, sizeof(dst->sdk), src->sdk);
        if (src->fingerprint[0] && !svc_prop_value_looks_generic(src->fingerprint))
            svc_prop_overlay_if_present(dst->fingerprint, sizeof(dst->fingerprint), src->fingerprint);
        return;
    }

    if (svc_prop_probe_is_system_slot(part_name)) {
        svc_prop_overlay_if_better(dst->android_release, sizeof(dst->android_release), src->android_release);
        svc_prop_overlay_if_better(dst->security_patch, sizeof(dst->security_patch), src->security_patch);
        svc_prop_overlay_if_better(dst->build_id, sizeof(dst->build_id), src->build_id);
        svc_prop_overlay_if_better(dst->incremental, sizeof(dst->incremental), src->incremental);
        svc_prop_overlay_if_better(dst->sdk, sizeof(dst->sdk), src->sdk);
        svc_prop_overlay_if_present(dst->common_ota, sizeof(dst->common_ota), src->common_ota);
        svc_prop_overlay_if_present(dst->auth_project, sizeof(dst->auth_project), src->auth_project);
        svc_prop_overlay_if_present(dst->hardware_code, sizeof(dst->hardware_code), src->hardware_code);
        return;
    }

    if (svc_prop_probe_is_odm_slot(part_name)) {
        svc_prop_overlay_if_present(dst->market_name, sizeof(dst->market_name), src->market_name);
        svc_prop_overlay_if_present(dst->display_id, sizeof(dst->display_id), src->display_id);
        if (!dst->display_full_id[0])
            svc_prop_overlay_if_present(dst->display_full_id, sizeof(dst->display_full_id), src->display_full_id);
        svc_prop_overlay_if_better(dst->brand, sizeof(dst->brand), src->brand);
        svc_prop_overlay_if_better(dst->manufacturer, sizeof(dst->manufacturer), src->manufacturer);
        svc_prop_overlay_if_better(dst->model, sizeof(dst->model), src->model);
        svc_prop_overlay_if_better(dst->device, sizeof(dst->device), src->device);
        svc_prop_overlay_if_better(dst->product, sizeof(dst->product), src->product);
        return;
    }

}

static edl_error_t svc_prop_probe_oplus_overlay_partition(edl_service_t *svc,
                                                          const svc_prop_probe_runtime_t *runtime,
                                                          const char *part_name,
                                                          edl_android_props_t *merged)
{
    edl_android_props_t candidate;
    const edl_partition_info_t *part = NULL;
    const int sector_size = edl_service_sector_size(svc);
    int64_t part_bytes = 0;

    if (!svc || !runtime || !part_name || !merged)
        return EDL_ERR_INVALID_PARAM;
    part = edl_service_find_partition(svc, part_name);
    if (!part)
        return EDL_ERR_FH_PARTITION_NOT_FOUND;
    if (sector_size > 0)
        part_bytes = (int64_t)part->num_sectors * (int64_t)sector_size;

    memset(&candidate, 0, sizeof(candidate));
    edl_error_t err = EDL_ERR_FILE_NOT_FOUND;
    const bool has_super = svc_prop_has_super_partition(svc);

    if (!has_super) {
        memset(&candidate, 0, sizeof(candidate));
        if (svc_prop_probe_is_vendor_slot(part_name) || svc_prop_probe_is_odm_slot(part_name)) {
            if (svc_prop_probe_is_vendor_slot(part_name)) {
                edl_android_props_t win1;
                edl_android_props_t win2;
                memset(&win1, 0, sizeof(win1));
                memset(&win2, 0, sizeof(win2));
                edl_error_t err1 = svc_prop_probe_partition_window_raw(
                    svc, runtime, part_name, part,
                    (int64_t)EDL_OPLUS_VENDOR_WINDOW1_OFFSET_BYTES,
                    (int64_t)EDL_OPLUS_VENDOR_WINDOW1_BYTES, &win1);
                edl_error_t err2 = svc_prop_probe_partition_window_raw(
                    svc, runtime, part_name, part,
                    (int64_t)EDL_OPLUS_VENDOR_WINDOW2_OFFSET_BYTES,
                    (int64_t)EDL_OPLUS_VENDOR_WINDOW2_BYTES, &win2);
                if (err1 == EDL_OK)
                    svc_prop_merge_into(&candidate, &win1);
                if (err2 == EDL_OK)
                    svc_prop_merge_into(&candidate, &win2);
                if (err1 == EDL_OK || err2 == EDL_OK)
                    err = EDL_OK;
                else
                    err = (err2 != EDL_OK) ? err2 : err1;
            } else {
                int64_t probe_bytes = part_bytes;
                if (probe_bytes > (int64_t)EDL_OPLUS_ODM_PREFIX_BYTES)
                    probe_bytes = (int64_t)EDL_OPLUS_ODM_PREFIX_BYTES;
                err = svc_prop_probe_partition_prefix(svc, runtime, part_name, part,
                                                      probe_bytes, true, &candidate);
            }
        }
    } else {
        if (svc_prop_probe_is_vendor_slot(part_name)
            || svc_prop_probe_is_system_slot(part_name)
            || svc_prop_probe_is_odm_slot(part_name)
            || svc_prop_probe_is_my_product_slot(part_name)) {
            memset(&candidate, 0, sizeof(candidate));
            int64_t probe_bytes = part_bytes;
            if (svc_prop_probe_is_vendor_slot(part_name)
                && probe_bytes > (int64_t)EDL_OPLUS_VENDOR_PREFIX_BYTES) {
                probe_bytes = (int64_t)EDL_OPLUS_VENDOR_PREFIX_BYTES;
            } else if (svc_prop_probe_is_system_slot(part_name)
                       && probe_bytes > (int64_t)EDL_OPLUS_SYSTEM_PREFIX_BYTES) {
                probe_bytes = (int64_t)EDL_OPLUS_SYSTEM_PREFIX_BYTES;
            } else if ((svc_prop_probe_is_odm_slot(part_name)
                        || svc_prop_probe_is_my_product_slot(part_name))
                       && probe_bytes > (int64_t)EDL_OPLUS_ODM_PREFIX_BYTES) {
                probe_bytes = (int64_t)EDL_OPLUS_ODM_PREFIX_BYTES;
            }
            err = svc_prop_probe_partition_prefix(svc, runtime, part_name, part,
                                                  probe_bytes, true, &candidate);
        }

        if (err != EDL_OK) {
            memset(&candidate, 0, sizeof(candidate));
            err = svc_prop_try_live_fs_merge(svc, runtime, part_name, part,
                                             false, part_bytes, 0, 1,
                                             merged, &candidate);
        }
        if (err != EDL_OK && !svc_prop_probe_is_fatal(err)) {
            memset(&candidate, 0, sizeof(candidate));
            err = svc_prop_probe_candidate(svc, runtime, part_name, 0, 1, merged, &candidate);
        }
    }
    if (err != EDL_OK)
        return err;

    svc_prop_apply_device_knowledge(&candidate);
    svc_prop_apply_oplus_partition_overlay(merged, &candidate, part_name);
    return EDL_OK;
}

static edl_error_t svc_prop_run_oplus_targeted_overlays(edl_service_t *svc,
                                                        const svc_prop_probe_runtime_t *runtime,
                                                        edl_android_props_t *merged)
{
    if (!svc || !runtime || !merged)
        return EDL_ERR_INVALID_PARAM;

    static const char *const vendor_parts[] = { "vendor_a", "vendor_b", "vendor", NULL };
    static const char *const odm_parts[] = { "odm_a", "odm_b", "odm", NULL };

    const struct {
        const char *const *parts;
        bool (*needed)(const edl_android_props_t *props);
    } passes[] = {
        { vendor_parts, svc_prop_oplus_needs_vendor_overlay },
        { odm_parts, svc_prop_oplus_needs_odm_overlay },
    };

    for (int pass = 0; pass < (int)(sizeof(passes) / sizeof(passes[0])); pass++) {
        if (!passes[pass].needed(merged))
            continue;

        for (int i = 0; passes[pass].parts[i]; i++) {
            const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
            if (remaining_ms != UINT64_MAX && remaining_ms <= 2500)
                return EDL_OK;

            edl_error_t err = svc_prop_probe_oplus_overlay_partition(svc, runtime,
                                                                     passes[pass].parts[i],
                                                                     merged);
            if (err == EDL_OK)
                break;
            if (svc_prop_probe_is_fatal(err))
                return err;
        }
    }

    return EDL_OK;
}

static edl_error_t svc_prop_probe_oplus_legacy_layout(edl_service_t *svc,
                                                      const svc_prop_probe_runtime_t *runtime,
                                                      edl_android_props_t *out)
{
    edl_android_props_t merged;
    bool any = false;

    if (!svc || !runtime || !out)
        return EDL_ERR_INVALID_PARAM;

    memset(&merged, 0, sizeof(merged));

    if (runtime->log_detail) {
        runtime->log_detail(svc,
                            "OPlus 老机型：按 vendor -> system -> odm 固定布局解析 Android 属性");
    }

    static const char *const vendor_parts[] = { "vendor_a", "vendor_b", "vendor", NULL };
    static const char *const system_parts[] = { "system_a", "system_b", "system", NULL };
    static const char *const odm_parts[] = { "odm_a", "odm_b", "odm", NULL };
    const char *const *passes[] = {
        vendor_parts,
        system_parts,
        odm_parts,
    };

    for (int pass = 0; pass < (int)(sizeof(passes) / sizeof(passes[0])); pass++) {
        for (int i = 0; passes[pass][i]; i++) {
            const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
            if (remaining_ms != UINT64_MAX && remaining_ms <= 2500)
                break;

            edl_error_t err = svc_prop_probe_oplus_overlay_partition(svc, runtime,
                                                                     passes[pass][i],
                                                                     &merged);
            if (err == EDL_OK) {
                any = true;
                break;
            }
            if (svc_prop_probe_is_fatal(err))
                return err;
        }
    }

    if (!any)
        return EDL_ERR_FILE_NOT_FOUND;

    snprintf(merged.source_partition, sizeof(merged.source_partition), "%s", "vendor+odm");
    snprintf(merged.fs_type, sizeof(merged.fs_type), "%s", "overlay");
    svc_prop_apply_device_knowledge(&merged);
    *out = merged;
    return EDL_OK;
}

static edl_error_t svc_prop_probe_candidates(edl_service_t *svc,
                                             const svc_prop_probe_runtime_t *runtime,
                                             edl_android_props_t *out)
{
    static const char *const default_candidates[] = {
        "system_a", "system_b", "system",
        "vendor_a", "vendor_b", "vendor",
        "product_a", "product_b", "product",
        "system_ext_a", "system_ext_b", "system_ext",
        "odm_a", "odm_b", "odm",
        "my_product_a", "my_product_b", "my_product",
        "cust_a", "cust_b", "cust",
        "super_a", "super_b", "super",
        NULL
    };
    static const char *const oplus_candidates[] = {
        "vendor_a", "vendor_b", "vendor",
        "odm_a", "odm_b", "odm",
        "my_product_a", "my_product_b", "my_product",
        "product_a", "product_b", "product",
        "system_ext_a", "system_ext_b", "system_ext",
        "system_a", "system_b", "system",
        "cust_a", "cust_b", "cust",
        "super_a", "super_b", "super",
        NULL
    };
    const bool is_oplus_family = svc_prop_is_oplus_family(svc);
    const bool has_super = svc_prop_has_super_partition(svc);
    if (is_oplus_family && !has_super) {
        edl_error_t legacy_err = svc_prop_probe_oplus_legacy_layout(svc, runtime, out);
        if (legacy_err == EDL_OK || svc_prop_probe_is_fatal(legacy_err))
            return legacy_err;
    }
    const char *const *candidates = is_oplus_family ? oplus_candidates : default_candidates;
    const char *fallback_candidates[8];
    edl_android_props_t merged;
    edl_android_props_t best;
    bool found = false;
    int best_score = 0;
    const int primary_candidate_total = svc_prop_count_candidates_filtered(candidates, has_super);
    const int fallback_candidate_total =
        svc_prop_collect_fallback_candidates(svc, candidates, has_super,
                                             fallback_candidates,
                                             (int)(sizeof(fallback_candidates) / sizeof(fallback_candidates[0])));
    const int candidate_total = primary_candidate_total + fallback_candidate_total;
    int candidate_index = 0;

    memset(&merged, 0, sizeof(merged));
    memset(&best, 0, sizeof(best));

    if (candidate_total > 0)
        svc_prop_report_progress(svc, runtime, 0, candidate_total, 0);

    for (int i = 0; candidates[i]; i++) {
        if (runtime->is_cancelled && runtime->is_cancelled(svc))
            return EDL_ERR_CANCELLED;
        if (!has_super && svc_prop_probe_is_super(candidates[i]))
            continue;

        edl_android_props_t candidate;
        memset(&candidate, 0, sizeof(candidate));

        edl_error_t err = svc_prop_probe_candidate(svc, runtime, candidates[i],
                                                   candidate_index, candidate_total,
                                                   found ? &merged : NULL,
                                                   &candidate);
        candidate_index++;
        if (err == EDL_OK) {
            svc_prop_apply_device_knowledge(&candidate);
            const int score = svc_prop_score(&candidate);
            if (!found) {
                merged = candidate;
                best = candidate;
                best_score = score;
                found = true;
            } else {
                svc_prop_merge_into(&merged, &candidate);
                if (score >= best_score) {
                    best = candidate;
                    best_score = score;
                }
            }

            if (svc_prop_core_complete(&merged)
                && (!is_oplus_family || svc_prop_oplus_props_complete(&merged))) {
                svc_prop_apply_best_source(&merged, &best);
                svc_prop_apply_device_knowledge(&merged);
                svc_prop_report_progress(svc, runtime, candidate_total, candidate_total, 100);
                *out = merged;
                return EDL_OK;
            }
            if (svc_prop_identity_useful(&merged)) {
                const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
                if (remaining_ms != UINT64_MAX
                    && remaining_ms <= 1500
                    && svc_prop_has_build_context(&merged)
                    && (!is_oplus_family || svc_prop_oplus_props_complete(&merged))) {
                    svc_prop_apply_best_source(&merged, &best);
                    svc_prop_apply_device_knowledge(&merged);
                    svc_prop_report_progress(svc, runtime, candidate_total, candidate_total, 100);
                    *out = merged;
                    return EDL_OK;
                }
            }
            continue;
        }
        if (svc_prop_probe_is_fatal(err)) {
            if (found && svc_prop_probe_should_keep_partial_on_error(err)) {
                svc_prop_apply_best_source(&merged, &best);
                svc_prop_apply_device_knowledge(&merged);
                svc_prop_report_progress(svc, runtime, candidate_total, candidate_total, 100);
                *out = merged;
                return EDL_OK;
            }
            return err;
        }
    }

    if (!is_oplus_family
        && fallback_candidate_total > 0
        && (!found
            || !svc_prop_core_complete(&merged)
            || (is_oplus_family && !svc_prop_oplus_props_complete(&merged)))) {
        if (runtime->log_detail) {
            runtime->log_detail(svc,
                                "Android 属性仍不完整，继续补扫 %d 个额外分区",
                                fallback_candidate_total);
        }

        for (int i = 0; i < fallback_candidate_total; i++) {
            if (runtime->is_cancelled && runtime->is_cancelled(svc))
                return EDL_ERR_CANCELLED;

            const uint64_t remaining_ms = svc_prop_remaining_stage_ms(svc, runtime);
            if (remaining_ms != UINT64_MAX && remaining_ms <= (found ? 4000 : 6000))
                break;

            edl_android_props_t candidate;
            memset(&candidate, 0, sizeof(candidate));

            edl_error_t err = svc_prop_probe_candidate(svc, runtime, fallback_candidates[i],
                                                       candidate_index, candidate_total,
                                                       found ? &merged : NULL,
                                                       &candidate);
            candidate_index++;
            if (err == EDL_OK) {
                svc_prop_apply_device_knowledge(&candidate);
                const int score = svc_prop_score(&candidate);
                if (!found) {
                    merged = candidate;
                    best = candidate;
                    best_score = score;
                    found = true;
                } else {
                    svc_prop_merge_into(&merged, &candidate);
                    if (score >= best_score) {
                        best = candidate;
                        best_score = score;
                    }
                }

                if (svc_prop_core_complete(&merged)
                    && (!is_oplus_family || svc_prop_oplus_props_complete(&merged))) {
                    svc_prop_apply_best_source(&merged, &best);
                    svc_prop_apply_device_knowledge(&merged);
                    svc_prop_report_progress(svc, runtime, candidate_total, candidate_total, 100);
                    *out = merged;
                    return EDL_OK;
                }
                continue;
            }
            if (svc_prop_probe_is_fatal(err)) {
                if (found && svc_prop_probe_should_keep_partial_on_error(err)) {
                    svc_prop_apply_best_source(&merged, &best);
                    svc_prop_apply_device_knowledge(&merged);
                    svc_prop_report_progress(svc, runtime, candidate_total, candidate_total, 100);
                    *out = merged;
                    return EDL_OK;
                }
                return err;
            }
        }
    }

    if (found) {
        if (is_oplus_family && !svc_prop_oplus_props_complete(&merged)) {
            edl_error_t overlay_err = svc_prop_run_oplus_targeted_overlays(svc, runtime, &merged);
            if (svc_prop_probe_is_fatal(overlay_err))
                return overlay_err;
        }
        svc_prop_apply_best_source(&merged, &best);
        svc_prop_apply_device_knowledge(&merged);
        svc_prop_report_progress(svc, runtime, candidate_total, candidate_total, 100);
        *out = merged;
        return EDL_OK;
    }

    if (runtime->log_detail) {
        runtime->log_detail(svc,
                            "未在 super/system/vendor 等候选分区中发现 build.prop");
    }

    svc_prop_report_progress(svc, runtime, candidate_total, candidate_total, 100);
    return EDL_ERR_FILE_NOT_FOUND;
}

edl_error_t edl_service_probe_android_build_props_impl(
    edl_service_t *svc,
    const svc_prop_probe_runtime_t *runtime,
    edl_android_props_t *out)
{
    if (!svc || !runtime || !out)
        return EDL_ERR_INVALID_PARAM;
    if (!edl_service_is_connected(svc))
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = runtime->now_ms ? runtime->now_ms() : 0;
    memset(out, 0, sizeof(*out));

    edl_error_t err = edl_service_ensure_gpt_cache_ex(svc, 0u);
    if (err != EDL_OK) {
        if (runtime->log_elapsed)
            runtime->log_elapsed(svc, "解析 Android 属性", err, start_ms);
        return err;
    }

    err = svc_prop_probe_candidates(svc, runtime, out);
    if (err == EDL_ERR_CANCELLED && svc && svc->stage_timeout_hit)
        err = EDL_ERR_TIMEOUT;
    if (runtime->log_elapsed)
        runtime->log_elapsed(svc, "解析 Android 属性", err, start_ms);
    return err;
}
