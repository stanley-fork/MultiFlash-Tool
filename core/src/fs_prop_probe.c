#include "edl/fs_prop_probe.h"
#include "edl/ext4_parser.h"
#include "edl/erofs_parser.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <strings.h>
#endif

#define PROP_PATH_MAX_LEN 256
#define PROP_IMPORT_MAX_DEPTH 6
#define PROP_IMPORTS_PER_FILE 16

typedef struct {
    const uint8_t *base;
    int64_t        len;
} mem_rd_ctx_t;

static int mem_read_fn(int64_t offset, uint8_t *buf, int len, void *ctx)
{
    mem_rd_ctx_t *c = (mem_rd_ctx_t *)ctx;
    if (!c || !c->base || offset < 0 || offset >= c->len)
        return -1;
    int64_t avail = c->len - offset;
    if (len > avail)
        len = (int)avail;
    if (len <= 0)
        return 0;
    memcpy(buf, c->base + offset, (size_t)len);
    return len;
}

typedef struct {
    edl_prop_probe_log_fn fn;
    void                 *user;
} log_wrap_t;

static void wrap_ext4_log(const char *msg, void *ctx)
{
    log_wrap_t *w = (log_wrap_t *)ctx;
    if (w && w->fn)
        w->fn(msg, w->user);
}

static void wrap_erofs_log(const char *msg, void *ctx)
{
    log_wrap_t *w = (log_wrap_t *)ctx;
    if (w && w->fn)
        w->fn(msg, w->user);
}

enum {
    PROP_FIELD_BRAND = 0,
    PROP_FIELD_MANUFACTURER,
    PROP_FIELD_MARKET_NAME,
    PROP_FIELD_LOCALE,
    PROP_FIELD_REGION_MARK,
    PROP_FIELD_REGION_TYPE,
    PROP_FIELD_MODEL,
    PROP_FIELD_DEVICE,
    PROP_FIELD_PRODUCT,
    PROP_FIELD_ANDROID_RELEASE,
    PROP_FIELD_FINGERPRINT,
    PROP_FIELD_SECURITY_PATCH,
    PROP_FIELD_BUILD_ID,
    PROP_FIELD_INCREMENTAL,
    PROP_FIELD_DISPLAY_ID,
    PROP_FIELD_BUILD_DATE,
    PROP_FIELD_BUILD_DATE_UTC,
    PROP_FIELD_BUILD_TYPE,
    PROP_FIELD_BUILD_TAGS,
    PROP_FIELD_MIUI_VERSION,
    PROP_FIELD_SDK,
    PROP_FIELD_OTA_VERSION,
    PROP_FIELD_DISPLAY_OTA,
    PROP_FIELD_DISPLAY_FULL_ID,
    PROP_FIELD_COMMON_OTA,
    PROP_FIELD_PROJECT_NUMBER,
    PROP_FIELD_AUTH_PROJECT,
    PROP_FIELD_HARDWARE_CODE,
    PROP_FIELD_NV_ID,
    PROP_FIELD_PIPELINE_KEY,
    PROP_FIELD_BASE_VERSION,
};

typedef struct {
    size_t      offset;
    size_t      cap;
    const char *keys[32];
} prop_field_t;

#define PROP_FIELD(field, ...) \
    { offsetof(edl_android_props_t, field), sizeof(((edl_android_props_t *)0)->field), { __VA_ARGS__, NULL } }

static const prop_field_t g_prop_fields[] = {
    [PROP_FIELD_BRAND] = PROP_FIELD(brand,
        "ro.product.brand",
        "ro.product.system.brand",
        "ro.product.system_ext.brand",
        "ro.product.product.brand",
        "ro.product.vendor.brand",
        "ro.product.odm.brand",
        "ro.system.product.brand",
        "ro.system_ext.product.brand",
        "ro.vendor.product.brand",
        "ro.odm.product.brand",
        "ro.vendor.oppo.product.brand",
        "ro.vendor.oplus.product.brand"),
    [PROP_FIELD_MANUFACTURER] = PROP_FIELD(manufacturer,
        "ro.product.manufacturer",
        "ro.product.system.manufacturer",
        "ro.product.system_ext.manufacturer",
        "ro.product.product.manufacturer",
        "ro.product.vendor.manufacturer",
        "ro.product.odm.manufacturer",
        "ro.system.product.manufacturer",
        "ro.system_ext.product.manufacturer",
        "ro.vendor.product.manufacturer",
        "ro.odm.product.manufacturer",
        "ro.vendor.oppo.product.manufacturer",
        "ro.vendor.oplus.product.manufacturer"),
    [PROP_FIELD_MARKET_NAME] = PROP_FIELD(market_name,
        "ro.product.marketname",
        "ro.product.system.marketname",
        "ro.product.system_ext.marketname",
        "ro.product.product.marketname",
        "ro.product.vendor.marketname",
        "ro.product.odm.marketname",
        "ro.system.product.marketname",
        "ro.system_ext.product.marketname",
        "ro.vendor.product.marketname",
        "ro.odm.product.marketname",
        "ro.vendor.oplus.market.name",
        "ro.vendor.oplus.marketname",
        "ro.vendor.oplus.market_name",
        "ro.oplus.market.name",
        "ro.oplus.marketname",
        "ro.oplus.market.enname",
        "ro.vendor.oplus.market.enname",
        "ro.oppo.market.name",
        "ro.oppo.marketname",
        "ro.oppo.market.enname",
        "ro.vendor.oppo.market.name",
        "ro.vendor.oppo.market.enname"),
    [PROP_FIELD_LOCALE] = PROP_FIELD(locale,
        "ro.product.locale",
        "ro.product.system.locale",
        "ro.product.vendor.locale",
        "ro.system.locale"),
    [PROP_FIELD_REGION_MARK] = PROP_FIELD(region_mark,
        "ro.vendor.oplus.regionmark",
        "ro.vendor.oppo.regionmark",
        "ro.oplus.regionmark",
        "ro.oppo.regionmark",
        "ro.vendor.oplus.region_mark",
        "ro.vendor.oppo.region_mark",
        "ro.oplus.region_mark",
        "ro.oppo.region_mark"),
    [PROP_FIELD_REGION_TYPE] = PROP_FIELD(region_type,
        "ro.oplus.image.my_region.type",
        "ro.oppo.image.my_region.type",
        "ro.vendor.oplus.image.my_region.type",
        "ro.vendor.oppo.image.my_region.type"),
    [PROP_FIELD_MODEL] = PROP_FIELD(model,
        "ro.product.model",
        "ro.product.system.model",
        "ro.product.system_ext.model",
        "ro.product.product.model",
        "ro.product.vendor.model",
        "ro.product.odm.model",
        "ro.system.product.model",
        "ro.system_ext.product.model",
        "ro.vendor.product.model",
        "ro.odm.product.model",
        "ro.product.oppo_model",
        "ro.product.oplus_model",
        "ro.vendor.oppo.product.model",
        "ro.vendor.oplus.product.model"),
    [PROP_FIELD_DEVICE] = PROP_FIELD(device,
        "ro.product.device",
        "ro.product.system.device",
        "ro.product.system_ext.device",
        "ro.product.product.device",
        "ro.product.vendor.device",
        "ro.product.odm.device",
        "ro.system.product.device",
        "ro.system_ext.product.device",
        "ro.vendor.product.device",
        "ro.odm.product.device",
        "ro.vendor.oppo.product.device",
        "ro.vendor.oplus.product.device"),
    [PROP_FIELD_PRODUCT] = PROP_FIELD(product,
        "ro.product.name",
        "ro.product.system.name",
        "ro.product.system_ext.name",
        "ro.product.product.name",
        "ro.product.vendor.name",
        "ro.product.odm.name",
        "ro.system.product.name",
        "ro.system_ext.product.name",
        "ro.vendor.product.name",
        "ro.odm.product.name",
        "ro.vendor.oppo.product.name",
        "ro.vendor.oplus.product.name",
        "ro.build.product"),
    [PROP_FIELD_ANDROID_RELEASE] = PROP_FIELD(android_release,
        "ro.build.version.release",
        "ro.build.version.release_or_codename",
        "ro.system.build.version.release_or_codename",
        "ro.system_ext.build.version.release_or_codename",
        "ro.product.build.version.release_or_codename",
        "ro.vendor.build.version.release_or_codename",
        "ro.odm.build.version.release_or_codename",
        "ro.system.build.version.release",
        "ro.system_ext.build.version.release",
        "ro.product.build.version.release",
        "ro.vendor.build.version.release",
        "ro.odm.build.version.release"),
    [PROP_FIELD_FINGERPRINT] = PROP_FIELD(fingerprint,
        "ro.build.fingerprint",
        "ro.bootimage.build.fingerprint",
        "ro.system.build.fingerprint",
        "ro.system_ext.build.fingerprint",
        "ro.product.build.fingerprint",
        "ro.vendor.build.fingerprint",
        "ro.odm.build.fingerprint"),
    [PROP_FIELD_SECURITY_PATCH] = PROP_FIELD(security_patch,
        "ro.build.version.security_patch",
        "ro.system.build.version.security_patch",
        "ro.system.build.security_patch",
        "ro.system_ext.build.version.security_patch",
        "ro.system_ext.build.security_patch",
        "ro.product.build.version.security_patch",
        "ro.product.build.security_patch",
        "ro.vendor.build.security_patch",
        "ro.odm.build.security_patch"),
    [PROP_FIELD_BUILD_ID] = PROP_FIELD(build_id,
        "ro.build.id",
        "ro.system.build.id",
        "ro.system_ext.build.id",
        "ro.product.build.id",
        "ro.vendor.build.id",
        "ro.odm.build.id"),
    [PROP_FIELD_INCREMENTAL] = PROP_FIELD(incremental,
        "ro.build.version.incremental",
        "ro.system.build.version.incremental",
        "ro.system_ext.build.version.incremental",
        "ro.product.build.version.incremental",
        "ro.vendor.build.version.incremental",
        "ro.odm.build.version.incremental"),
    [PROP_FIELD_DISPLAY_ID] = PROP_FIELD(display_id,
        "ro.build.display.id",
        "ro.system.build.display.id",
        "ro.system_ext.build.display.id",
        "ro.product.build.display.id",
        "ro.vendor.build.display.id",
        "ro.odm.build.display.id"),
    [PROP_FIELD_BUILD_DATE] = PROP_FIELD(build_date,
        "ro.build.date",
        "ro.system.build.date",
        "ro.system_ext.build.date",
        "ro.product.build.date",
        "ro.vendor.build.date",
        "ro.odm.build.date"),
    [PROP_FIELD_BUILD_DATE_UTC] = PROP_FIELD(build_date_utc,
        "ro.build.date.utc",
        "ro.system.build.date.utc",
        "ro.system_ext.build.date.utc",
        "ro.product.build.date.utc",
        "ro.vendor.build.date.utc",
        "ro.odm.build.date.utc"),
    [PROP_FIELD_BUILD_TYPE] = PROP_FIELD(build_type,
        "ro.build.type",
        "ro.system.build.type",
        "ro.system_ext.build.type",
        "ro.product.build.type",
        "ro.vendor.build.type",
        "ro.odm.build.type"),
    [PROP_FIELD_BUILD_TAGS] = PROP_FIELD(build_tags,
        "ro.build.tags",
        "ro.system.build.tags",
        "ro.system_ext.build.tags",
        "ro.product.build.tags",
        "ro.vendor.build.tags",
        "ro.odm.build.tags"),
    [PROP_FIELD_MIUI_VERSION] = PROP_FIELD(miui_version,
        "ro.miui.ui.version.name",
        "ro.mi.os.version.name"),
    [PROP_FIELD_SDK] = PROP_FIELD(sdk,
        "ro.build.version.sdk",
        "ro.system.build.version.sdk",
        "ro.system_ext.build.version.sdk",
        "ro.product.build.version.sdk",
        "ro.vendor.build.version.sdk",
        "ro.odm.build.version.sdk"),
    [PROP_FIELD_OTA_VERSION] = PROP_FIELD(ota_version,
        "ro.build.version.ota",
        "ro.vendor.build.version.ota",
        "ro.system.build.version.ota"),
    [PROP_FIELD_DISPLAY_OTA] = PROP_FIELD(display_ota,
        "ro.build.display.ota",
        "ro.vendor.build.display.ota",
        "ro.system.build.display.ota"),
    [PROP_FIELD_DISPLAY_FULL_ID] = PROP_FIELD(display_full_id,
        "ro.build.display.full_id",
        "ro.vendor.build.display.full_id",
        "ro.system.build.display.full_id"),
    [PROP_FIELD_COMMON_OTA] = PROP_FIELD(common_ota,
        "ro.commonsoft.ota"),
    [PROP_FIELD_PROJECT_NUMBER] = PROP_FIELD(project_number,
        "ro.separate.soft",
        "ro.product.supported_versions",
        "ro.boot.prjname",
        "ro.vendor.oplus.prjname",
        "ro.vendor.oppo.prjname"),
    [PROP_FIELD_AUTH_PROJECT] = PROP_FIELD(auth_project,
        "ro.product.authentication"),
    [PROP_FIELD_HARDWARE_CODE] = PROP_FIELD(hardware_code,
        "ro.product.hw"),
    [PROP_FIELD_NV_ID] = PROP_FIELD(nv_id,
        "ro.build.oplus_nv_id",
        "ro.build.oppo_nv_id"),
    [PROP_FIELD_PIPELINE_KEY] = PROP_FIELD(pipeline_key,
        "ro.oplus.pipeline_key",
        "ro.vendor.oplus.pipeline_key",
        "ro.oppo.pipeline_key",
        "ro.vendor.oppo.pipeline_key"),
    [PROP_FIELD_BASE_VERSION] = PROP_FIELD(base_version,
        "ro.oplus.version.base",
        "ro.oppo.version.base"),
};

static const char *g_prop_text_paths[] = {
    "/build.prop",
    "/default.prop",
    "/prop.default",
    "/etc/build.prop",
    "/etc/prop.default",
    "/first_stage_ramdisk/default.prop",
    "/first_stage_ramdisk/prop.default",
    "/system_root/build.prop",
    "/system_root/default.prop",
    "/system_root/prop.default",
    "/system_root/etc/build.prop",
    "/system_root/etc/prop.default",
    "/system_root/system/build.prop",
    "/system_root/system/default.prop",
    "/system_root/system/etc/build.prop",
    "/system_root/system/etc/prop.default",
    "/system/build.prop",
    "/system/default.prop",
    "/system/etc/build.prop",
    "/system/etc/prop.default",
    "/system/system/build.prop",
    "/system/system/etc/build.prop",
    "/system/product/build.prop",
    "/system/product/default.prop",
    "/system/product/etc/build.prop",
    "/system/product/etc/prop.default",
    "/system/system_ext/build.prop",
    "/system/system_ext/default.prop",
    "/system/system_ext/etc/build.prop",
    "/system/system_ext/etc/prop.default",
    "/system/vendor/build.prop",
    "/system/vendor/default.prop",
    "/system/vendor/etc/build.prop",
    "/system/vendor/etc/prop.default",
    "/system/odm/build.prop",
    "/system/odm/default.prop",
    "/system/odm/etc/build.prop",
    "/system/odm/etc/prop.default",
    "/system_root/product/build.prop",
    "/system_root/product/default.prop",
    "/system_root/product/etc/build.prop",
    "/system_root/product/etc/prop.default",
    "/system_root/system_ext/build.prop",
    "/system_root/system_ext/default.prop",
    "/system_root/system_ext/etc/build.prop",
    "/system_root/system_ext/etc/prop.default",
    "/system_root/vendor/build.prop",
    "/system_root/vendor/default.prop",
    "/system_root/vendor/etc/build.prop",
    "/system_root/vendor/etc/prop.default",
    "/system_root/odm/build.prop",
    "/system_root/odm/default.prop",
    "/system_root/odm/etc/build.prop",
    "/system_root/odm/etc/prop.default",
    "/vendor/build.prop",
    "/vendor/default.prop",
    "/vendor/etc/build.prop",
    "/vendor/etc/prop.default",
    "/product/build.prop",
    "/product/default.prop",
    "/product/etc/build.prop",
    "/product/etc/prop.default",
    "/system_ext/build.prop",
    "/system_ext/default.prop",
    "/system_ext/etc/build.prop",
    "/system_ext/etc/prop.default",
    "/odm/build.prop",
    "/odm/default.prop",
    "/odm/etc/build.prop",
    "/odm/etc/prop.default",
    "/my_product/build.prop",
    "/my_product/default.prop",
    "/my_product/etc/build.prop",
    "/my_product/etc/prop.default",
    "/cust/build.prop",
    "/cust/default.prop",
    "/cust/etc/build.prop",
    "/cust/etc/prop.default",
    NULL
};

static bool scan_prop_blob_into(const uint8_t *data, int len, edl_android_props_t *out);
static bool props_core_complete(const edl_android_props_t *out);
static bool props_scan_complete(const edl_android_props_t *out);
static bool probe_prop_path_list_deep(void *parser,
                                      edl_prop_probe_read_text_fn read_text,
                                      const char *fs_name,
                                      const char *const *paths,
                                      edl_android_props_t *out,
                                      edl_prop_probe_log_fn log_fn,
                                      void *log_user,
                                      char seen_paths[][PROP_PATH_MAX_LEN],
                                      int *seen_count,
                                      int *progress_current,
                                      int *progress_total,
                                      edl_prop_probe_progress_fn progress_fn,
                                      void *progress_user);

static const char *g_prop_root_paths[] = {
    "/build.prop",
    "/default.prop",
    "/prop.default",
    "/etc/build.prop",
    "/etc/prop.default",
    "/first_stage_ramdisk/default.prop",
    "/first_stage_ramdisk/prop.default",
    "/system_root/build.prop",
    "/system_root/default.prop",
    "/system_root/prop.default",
    "/system_root/etc/build.prop",
    "/system_root/etc/prop.default",
    "/system_root/system/build.prop",
    "/system_root/system/default.prop",
    "/system_root/system/etc/build.prop",
    "/system_root/system/etc/prop.default",
    NULL
};

static const char *g_prop_system_paths[] = {
    "/system/build.prop",
    "/system/default.prop",
    "/system/etc/build.prop",
    "/system/etc/prop.default",
    "/system/system/build.prop",
    "/system/system/etc/build.prop",
    "/system/product/build.prop",
    "/system/product/default.prop",
    "/system/product/etc/build.prop",
    "/system/product/etc/prop.default",
    "/system/system_ext/build.prop",
    "/system/system_ext/default.prop",
    "/system/system_ext/etc/build.prop",
    "/system/system_ext/etc/prop.default",
    "/system/vendor/build.prop",
    "/system/vendor/default.prop",
    "/system/vendor/etc/build.prop",
    "/system/vendor/etc/prop.default",
    "/system/odm/build.prop",
    "/system/odm/default.prop",
    "/system/odm/etc/build.prop",
    "/system/odm/etc/prop.default",
    "/system_root/build.prop",
    "/system_root/default.prop",
    "/system_root/etc/build.prop",
    "/system_root/etc/prop.default",
    "/system_root/system/build.prop",
    "/system_root/system/default.prop",
    "/system_root/system/etc/build.prop",
    "/system_root/system/etc/prop.default",
    "/system_root/product/build.prop",
    "/system_root/product/default.prop",
    "/system_root/product/etc/build.prop",
    "/system_root/product/etc/prop.default",
    "/system_root/system_ext/build.prop",
    "/system_root/system_ext/default.prop",
    "/system_root/system_ext/etc/build.prop",
    "/system_root/system_ext/etc/prop.default",
    "/system_root/vendor/build.prop",
    "/system_root/vendor/default.prop",
    "/system_root/vendor/etc/build.prop",
    "/system_root/vendor/etc/prop.default",
    "/system_root/odm/build.prop",
    "/system_root/odm/default.prop",
    "/system_root/odm/etc/build.prop",
    "/system_root/odm/etc/prop.default",
    NULL
};

static const char *g_prop_vendor_paths[] = {
    "/vendor/build.prop",
    "/vendor/default.prop",
    "/vendor/etc/build.prop",
    "/vendor/etc/prop.default",
    NULL
};

static const char *g_prop_product_paths[] = {
    "/product/build.prop",
    "/product/default.prop",
    "/product/etc/build.prop",
    "/product/etc/prop.default",
    NULL
};

static const char *g_prop_system_ext_paths[] = {
    "/system_ext/build.prop",
    "/system_ext/default.prop",
    "/system_ext/etc/build.prop",
    "/system_ext/etc/prop.default",
    NULL
};

static const char *g_prop_odm_paths[] = {
    "/odm/build.prop",
    "/odm/default.prop",
    "/odm/etc/build.prop",
    "/odm/etc/prop.default",
    NULL
};

static const char *g_prop_my_product_paths[] = {
    "/my_product/build.prop",
    "/my_product/etc/build.prop",
    "/my_product/default.prop",
    "/my_product/etc/prop.default",
    NULL
};

static const char *g_prop_cust_paths[] = {
    "/cust/build.prop",
    "/cust/etc/build.prop",
    "/cust/default.prop",
    "/cust/etc/prop.default",
    NULL
};

static bool prop_name_equals(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs)
        return false;
#ifdef _WIN32
    return _stricmp(lhs, rhs) == 0;
#else
    return strcasecmp(lhs, rhs) == 0;
#endif
}

static void prop_partition_base_name(const char *label, char *buf, size_t buf_size)
{
    size_t len = 0;

    if (!buf || buf_size == 0)
        return;
    buf[0] = '\0';
    if (!label || !label[0])
        return;

    snprintf(buf, buf_size, "%s", label);
    len = strlen(buf);
    if (len > 2 && buf[len - 2] == '_' && (buf[len - 1] == 'a' || buf[len - 1] == 'b'))
        buf[len - 2] = '\0';
}

static bool prop_partition_base_supported(const char *label)
{
    char base[64];

    prop_partition_base_name(label, base, sizeof(base));
    if (!base[0])
        return false;
    return prop_name_equals(base, "system")
        || prop_name_equals(base, "vendor")
        || prop_name_equals(base, "product")
        || prop_name_equals(base, "system_ext")
        || prop_name_equals(base, "odm")
        || prop_name_equals(base, "my_product")
        || prop_name_equals(base, "cust");
}

static const char *prop_partition_hint_label(const edl_android_props_t *props)
{
    if (!props)
        return NULL;
    if (prop_partition_base_supported(props->volume_label))
        return props->volume_label;
    if (prop_partition_base_supported(props->source_partition))
        return props->source_partition;
    if (props->volume_label[0])
        return props->volume_label;
    if (props->source_partition[0])
        return props->source_partition;
    return NULL;
}

static const char *const *prop_partition_paths_for_label(const char *label)
{
    char base[64];

    prop_partition_base_name(label, base, sizeof(base));
    if (!base[0])
        return NULL;
    if (prop_name_equals(base, "system"))
        return g_prop_system_paths;
    if (prop_name_equals(base, "vendor"))
        return g_prop_vendor_paths;
    if (prop_name_equals(base, "product"))
        return g_prop_product_paths;
    if (prop_name_equals(base, "system_ext"))
        return g_prop_system_ext_paths;
    if (prop_name_equals(base, "odm"))
        return g_prop_odm_paths;
    if (prop_name_equals(base, "my_product"))
        return g_prop_my_product_paths;
    if (prop_name_equals(base, "cust"))
        return g_prop_cust_paths;
    return NULL;
}

static bool prop_project_token_valid(const char *value)
{
    if (!value || !value[0])
        return false;

    for (const char *p = value; *p; p++) {
        const unsigned char ch = (unsigned char)*p;
        if (!isalnum(ch) && ch != '_' && ch != '-')
            return false;
    }
    return true;
}

static int prop_partition_dynamic_paths_for_label(const char *label,
                                                  const edl_android_props_t *props,
                                                  const char **out_paths,
                                                  char generated[][128],
                                                  int capacity)
{
    char base[64];
    const char *project = NULL;
    int count = 0;

    if (!label || !props || !out_paths || !generated || capacity <= 0)
        return 0;

    prop_partition_base_name(label, base, sizeof(base));
    if (!base[0])
        return 0;

    project = props->project_number;
    if (!prop_project_token_valid(project))
        project = NULL;

    if (project) {
        if (count < capacity) {
            snprintf(generated[count], 128, "/etc/%s/build.prop", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/etc/%s/prop.default", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/my_product/etc/%s/build.prop", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/my_product/etc/%s/prop.default", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/product/etc/%s/build.prop", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/product/etc/%s/prop.default", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/odm/etc/%s/build.prop", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/odm/etc/%s/prop.default", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/system_ext/etc/%s/build.prop", project);
            out_paths[count] = generated[count];
            count++;
        }
        if (count < capacity) {
            snprintf(generated[count], 128, "/system_ext/etc/%s/prop.default", project);
            out_paths[count] = generated[count];
            count++;
        }
    }

    return count;
}

#define PROP_PATH_SEEN_MAX 96

static int prop_path_list_count(const char *const *paths)
{
    int count = 0;

    if (!paths)
        return 0;
    while (paths[count])
        count++;
    return count;
}

static bool prop_path_seen(const char *path, const char *const *seen_paths, int seen_count)
{
    if (!path || !seen_paths || seen_count <= 0)
        return false;

    for (int i = 0; i < seen_count; i++) {
        if (seen_paths[i] && strcmp(seen_paths[i], path) == 0)
            return true;
    }
    return false;
}

static bool prop_path_seen_buf(const char *path,
                               char seen_paths[][PROP_PATH_MAX_LEN],
                               int seen_count)
{
    if (!path || !seen_paths || seen_count <= 0)
        return false;

    for (int i = 0; i < seen_count; i++) {
        if (seen_paths[i][0] && strcmp(seen_paths[i], path) == 0)
            return true;
    }
    return false;
}

static void prop_path_mark_seen_buf(const char *path,
                                    char seen_paths[][PROP_PATH_MAX_LEN],
                                    int *seen_count)
{
    if (!path || !path[0] || !seen_paths || !seen_count)
        return;
    if (*seen_count < 0 || *seen_count >= PROP_PATH_SEEN_MAX)
        return;

    snprintf(seen_paths[*seen_count], PROP_PATH_MAX_LEN, "%s", path);
    (*seen_count)++;
}

static void prop_progress_report(edl_prop_probe_progress_fn progress_fn, void *progress_user,
                                 int current, int total)
{
    if (!progress_fn || total <= 0)
        return;

    if (current < 0)
        current = 0;
    if (current > total)
        current = total;
    progress_fn(current, total, progress_user);
}

static bool probe_prop_path_list(void *parser,
                                 edl_prop_probe_read_text_fn read_text,
                                 const char *fs_name,
                                 const char *const *paths,
                                 edl_android_props_t *out,
                                 edl_prop_probe_log_fn log_fn,
                                 void *log_user,
                                 const char **seen_paths,
                                 int *seen_count,
                                 int *progress_current,
                                 int progress_total,
                                 edl_prop_probe_progress_fn progress_fn,
                                 void *progress_user)
{
    char text[256 * 1024];
    bool found = false;

    if (!parser || !read_text || !paths || !out)
        return false;

    for (int i = 0; paths[i]; i++) {
        const char *path = paths[i];
        if (prop_path_seen(path, seen_paths, seen_count ? *seen_count : 0))
            continue;
        if (seen_paths && seen_count && *seen_count < PROP_PATH_SEEN_MAX)
            seen_paths[(*seen_count)++] = path;

        if (progress_current) {
            (*progress_current)++;
            prop_progress_report(progress_fn, progress_user, *progress_current, progress_total);
        }

        int n = read_text(parser, path, text, (int)sizeof(text));
        if (n > 0) {
            if (log_fn) {
                char msg[320];
                snprintf(msg, sizeof(msg), "[属性] %s：读取 %s", fs_name, path);
                log_fn(msg, log_user);
            }

            if (scan_prop_blob_into((const uint8_t *)text, n, out))
                found = true;
        }

        if (progress_current) {
            (*progress_current)++;
            prop_progress_report(progress_fn, progress_user, *progress_current, progress_total);
        }

        if (props_core_complete(out))
            break;
    }

    return found;
}

static char *prop_field_ptr(edl_android_props_t *out, const prop_field_t *field)
{
    return (char *)out + field->offset;
}

static const char *prop_field_value_cstr(const edl_android_props_t *out, size_t field_index)
{
    if (!out || field_index >= sizeof(g_prop_fields) / sizeof(g_prop_fields[0]))
        return "";
    return (const char *)out + g_prop_fields[field_index].offset;
}

static bool prop_field_has_value(const edl_android_props_t *out, size_t field_index)
{
    const char *value = prop_field_value_cstr(out, field_index);
    return value && value[0] != '\0';
}

static bool any_prop_set(const edl_android_props_t *o)
{
    for (size_t i = 0; i < sizeof(g_prop_fields) / sizeof(g_prop_fields[0]); i++) {
        if (prop_field_has_value(o, i))
            return true;
    }
    return false;
}

static uint32_t prop_fields_full_mask(void)
{
    return (uint32_t)((1u << (sizeof(g_prop_fields) / sizeof(g_prop_fields[0]))) - 1u);
}

static bool prop_starts_with_nocase(const char *value, const char *prefix)
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

static bool prop_contains_nocase(const char *value, const char *needle)
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

static bool prop_token_looks_soc_id(const char *value)
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

static bool prop_value_looks_generic(const char *value)
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
    if (prop_starts_with_nocase(value, "qti/")
        || prop_starts_with_nocase(value, "qcom/")
        || prop_starts_with_nocase(value, "qualcomm/"))
        return true;
    if (prop_starts_with_nocase(value, "qti")
        || prop_starts_with_nocase(value, "qcom")
        || prop_starts_with_nocase(value, "qualcomm"))
        return true;
    if (prop_token_looks_soc_id(value))
        return true;
    if (prop_contains_nocase(value, " for arm")
        || prop_contains_nocase(value, " for aarch64"))
        return true;
    return false;
}

static bool prop_field_is_build_field(size_t field_index)
{
    return field_index >= PROP_FIELD_ANDROID_RELEASE && field_index <= PROP_FIELD_BASE_VERSION;
}

static bool prop_name_ends_with_nocase(const uint8_t *key, int key_len, const char *suffix)
{
    size_t suffix_len = 0;

    if (!key || key_len <= 0 || !suffix || !suffix[0])
        return false;

    suffix_len = strlen(suffix);
    if ((size_t)key_len < suffix_len)
        return false;

#ifdef _WIN32
    return _strnicmp((const char *)(key + key_len - (int)suffix_len), suffix, (int)suffix_len) == 0;
#else
    return strncasecmp((const char *)(key + key_len - (int)suffix_len), suffix, suffix_len) == 0;
#endif
}

static void prop_store_value(edl_android_props_t *out, size_t field_index,
                             const uint8_t *val, int val_len,
                             bool allow_replace, uint32_t *found_mask)
{
    if (!out || field_index >= (sizeof(g_prop_fields) / sizeof(g_prop_fields[0])) || !val || val_len <= 0)
        return;

    const prop_field_t *field = &g_prop_fields[field_index];
    char *dst = prop_field_ptr(out, field);
    if (dst[0] != '\0' && !allow_replace)
        return;

    int copy_len = val_len;
    if (copy_len >= (int)field->cap)
        copy_len = (int)field->cap - 1;
    if (copy_len > 0) {
        memcpy(dst, val, (size_t)copy_len);
        dst[copy_len] = '\0';
    }
    if (found_mask)
        *found_mask |= (1u << field_index);
}

static bool prop_try_store_alias(edl_android_props_t *out,
                                 const uint8_t *key, int key_len,
                                 const uint8_t *val, int val_len,
                                 uint32_t *found_mask)
{
    static const struct {
        size_t field_index;
        const char *suffixes[12];
    } alias_rules[] = {
        { PROP_FIELD_BRAND, {
            ".product.brand",
            ".brand_name",
            NULL
        } },
        { PROP_FIELD_MANUFACTURER, {
            ".product.manufacturer",
            NULL
        } },
        { PROP_FIELD_MARKET_NAME, {
            ".marketname",
            ".market_name",
            ".market.name",
            ".market.enname",
            ".marketing_name",
            ".marketing.name",
            ".commercial.name",
            NULL
        } },
        { PROP_FIELD_MODEL, {
            ".product.model",
            ".oppo_model",
            ".oplus_model",
            ".market_model",
            NULL
        } },
        { PROP_FIELD_DEVICE, {
            ".product.device",
            NULL
        } },
        { PROP_FIELD_PRODUCT, {
            ".product.name",
            NULL
        } },
        { PROP_FIELD_OTA_VERSION, {
            ".version.ota",
            NULL
        } },
        { PROP_FIELD_DISPLAY_OTA, {
            ".display.ota",
            NULL
        } },
        { PROP_FIELD_DISPLAY_FULL_ID, {
            ".display.full_id",
            NULL
        } },
        { PROP_FIELD_COMMON_OTA, {
            ".commonsoft.ota",
            NULL
        } },
        { PROP_FIELD_PROJECT_NUMBER, {
            ".supported_versions",
            ".prjname",
            ".project_number",
            NULL
        } },
        { PROP_FIELD_AUTH_PROJECT, {
            ".authentication",
            NULL
        } },
        { PROP_FIELD_HARDWARE_CODE, {
            ".product.hw",
            ".hardware_code",
            NULL
        } },
        { PROP_FIELD_NV_ID, {
            ".oplus_nv_id",
            ".oppo_nv_id",
            ".nv_id",
            NULL
        } },
        { PROP_FIELD_PIPELINE_KEY, {
            ".pipeline_key",
            NULL
        } },
        { PROP_FIELD_BASE_VERSION, {
            ".version.base",
            NULL
        } },
    };

    if (!out || !key || key_len <= 0 || !val || val_len <= 0)
        return false;

    for (size_t i = 0; i < sizeof(alias_rules) / sizeof(alias_rules[0]); i++) {
        const size_t field_index = alias_rules[i].field_index;
        for (size_t k = 0; alias_rules[i].suffixes[k]; k++) {
            if (!prop_name_ends_with_nocase(key, key_len, alias_rules[i].suffixes[k]))
                continue;

            const char *existing = prop_field_value_cstr(out, field_index);
            const bool allow_replace =
                existing[0] != '\0' && prop_value_looks_generic(existing)
                && !prop_field_is_build_field(field_index);
            prop_store_value(out, field_index, val, val_len, allow_replace, found_mask);
            return true;
        }
    }

    return false;
}

static bool prop_try_store_pair(edl_android_props_t *out,
                                const uint8_t *key, int key_len,
                                const uint8_t *val, int val_len,
                                uint32_t *found_mask)
{
    if (!out || !key || key_len <= 0 || !val || val_len <= 0)
        return false;

    for (size_t i = 0; i < sizeof(g_prop_fields) / sizeof(g_prop_fields[0]); i++) {
        const prop_field_t *field = &g_prop_fields[i];
        for (size_t k = 0; k < sizeof(field->keys) / sizeof(field->keys[0]); k++) {
            const char *slot_key = field->keys[k];
            if (!slot_key)
                break;
            if ((int)strlen(slot_key) != key_len || memcmp(key, slot_key, (size_t)key_len) != 0)
                continue;

            const char *existing = prop_field_value_cstr(out, i);
            const bool allow_replace =
                existing[0] != '\0' && prop_value_looks_generic(existing)
                && !prop_field_is_build_field(i);
            prop_store_value(out, i, val, val_len, allow_replace, found_mask);
            return true;
        }
    }

    return prop_try_store_alias(out, key, key_len, val, val_len, found_mask);
}

static void normalize_props_after_scan(edl_android_props_t *out);

static bool scan_prop_blob_into(const uint8_t *data, int len, edl_android_props_t *out)
{
    if (!data || len <= 0 || !out)
        return false;

    uint32_t found_mask = 0;
    const uint32_t full_mask = prop_fields_full_mask();
    int line_start = 0;

    while (line_start < len) {
        while (line_start < len && (data[line_start] == (uint8_t)'\n' || data[line_start] == (uint8_t)'\r'))
            line_start++;
        if (line_start >= len)
            break;

        int line_end = line_start;
        while (line_end < len && data[line_end] != (uint8_t)'\n' && data[line_end] != (uint8_t)'\r')
            line_end++;

        int key_start = line_start;
        while (key_start < line_end && (data[key_start] == (uint8_t)' ' || data[key_start] == (uint8_t)'\t'))
            key_start++;

        if (key_start < line_end && data[key_start] != (uint8_t)'#') {
            int eq = key_start;
            while (eq < line_end && data[eq] != (uint8_t)'=')
                eq++;
            if (eq < line_end) {
                int key_end = eq;
                while (key_end > key_start &&
                       (data[key_end - 1] == (uint8_t)' ' || data[key_end - 1] == (uint8_t)'\t'))
                    key_end--;

                int val_start = eq + 1;
                while (val_start < line_end &&
                       (data[val_start] == (uint8_t)' ' || data[val_start] == (uint8_t)'\t'))
                    val_start++;

                int val_end = line_end;
                while (val_end > val_start &&
                       (data[val_end - 1] == (uint8_t)' ' || data[val_end - 1] == (uint8_t)'\t'))
                    val_end--;

                const int key_len = key_end - key_start;
                const int val_len = val_end - val_start;
                if (key_len > 0 && val_len > 0) {
                    (void)prop_try_store_pair(out,
                                              data + key_start, key_len,
                                              data + val_start, val_len,
                                              &found_mask);
                    if (found_mask == full_mask)
                        break;
                }
            }
        }

        line_start = line_end + 1;
    }

    if (any_prop_set(out)) {
        normalize_props_after_scan(out);
        return true;
    }
    return false;
}

static bool props_oplus_family_hint(const edl_android_props_t *out)
{
    if (!out)
        return false;
    if (out->project_number[0] || out->ota_version[0] || out->display_ota[0]
        || out->display_full_id[0] || out->common_ota[0] || out->auth_project[0]
        || out->hardware_code[0] || out->nv_id[0] || out->pipeline_key[0]
        || out->base_version[0]) {
        return true;
    }
    return prop_starts_with_nocase(out->brand, "realme")
        || prop_starts_with_nocase(out->brand, "oppo")
        || prop_starts_with_nocase(out->brand, "oneplus")
        || prop_starts_with_nocase(out->manufacturer, "realme")
        || prop_starts_with_nocase(out->manufacturer, "oppo")
        || prop_starts_with_nocase(out->manufacturer, "oneplus")
        || prop_starts_with_nocase(out->market_name, "realme ")
        || prop_starts_with_nocase(out->market_name, "oppo ")
        || prop_starts_with_nocase(out->market_name, "oneplus ")
        || prop_starts_with_nocase(out->model, "RMX")
        || prop_starts_with_nocase(out->model, "CPH")
        || prop_starts_with_nocase(out->model, "PJD")
        || prop_starts_with_nocase(out->model, "PJA")
        || prop_starts_with_nocase(out->model, "LE");
}

static bool props_oplus_extended_complete(const edl_android_props_t *out)
{
    if (!out)
        return false;

    return out->project_number[0]
        && (out->ota_version[0] || out->display_ota[0] || out->display_full_id[0]
            || out->common_ota[0])
        && (out->auth_project[0] || out->hardware_code[0] || out->nv_id[0]
            || out->pipeline_key[0] || out->base_version[0]);
}

static bool props_scan_complete(const edl_android_props_t *out)
{
    if (!props_core_complete(out))
        return false;
    if (!props_oplus_family_hint(out))
        return true;
    return props_oplus_extended_complete(out);
}

static const char *prop_lookup_value_from_props(const edl_android_props_t *props, const char *key)
{
    if (!props || !key || !key[0])
        return NULL;

    if (prop_name_equals(key, "ro.product.brand") || prop_name_equals(key, "ro.vendor.product.brand")
        || prop_name_equals(key, "ro.system.product.brand") || prop_name_equals(key, "ro.odm.product.brand"))
        return props->brand;
    if (prop_name_equals(key, "ro.product.manufacturer") || prop_name_equals(key, "ro.vendor.product.manufacturer")
        || prop_name_equals(key, "ro.system.product.manufacturer") || prop_name_equals(key, "ro.odm.product.manufacturer"))
        return props->manufacturer;
    if (prop_name_equals(key, "ro.product.marketname") || prop_name_equals(key, "ro.oplus.market.name")
        || prop_name_equals(key, "ro.oplus.marketname") || prop_name_equals(key, "ro.oplus.market.enname"))
        return props->market_name;
    if (prop_name_equals(key, "ro.product.model") || prop_name_equals(key, "ro.product.oplus_model")
        || prop_name_equals(key, "ro.product.oppo_model"))
        return props->model;
    if (prop_name_equals(key, "ro.product.device"))
        return props->device;
    if (prop_name_equals(key, "ro.product.name") || prop_name_equals(key, "ro.build.product"))
        return props->product;
    if (prop_name_equals(key, "ro.build.version.release")
        || prop_name_equals(key, "ro.build.version.release_or_codename"))
        return props->android_release;
    if (prop_name_equals(key, "ro.build.fingerprint"))
        return props->fingerprint;
    if (prop_name_equals(key, "ro.build.version.security_patch"))
        return props->security_patch;
    if (prop_name_equals(key, "ro.build.id"))
        return props->build_id;
    if (prop_name_equals(key, "ro.build.version.incremental"))
        return props->incremental;
    if (prop_name_equals(key, "ro.build.display.id"))
        return props->display_id;
    if (prop_name_equals(key, "ro.build.version.sdk"))
        return props->sdk;
    if (prop_name_equals(key, "ro.build.version.ota"))
        return props->ota_version;
    if (prop_name_equals(key, "ro.build.display.ota"))
        return props->display_ota;
    if (prop_name_equals(key, "ro.build.display.full_id"))
        return props->display_full_id;
    if (prop_name_equals(key, "ro.commonsoft.ota"))
        return props->common_ota;
    if (prop_name_equals(key, "ro.boot.prjname") || prop_name_equals(key, "ro.product.supported_versions")
        || prop_name_equals(key, "ro.separate.soft"))
        return props->project_number;
    if (prop_name_equals(key, "ro.product.authentication"))
        return props->auth_project;
    if (prop_name_equals(key, "ro.product.hw"))
        return props->hardware_code;
    if (prop_name_equals(key, "ro.build.oplus_nv_id") || prop_name_equals(key, "ro.build.oppo_nv_id"))
        return props->nv_id;
    if (prop_name_equals(key, "ro.oplus.pipeline_key"))
        return props->pipeline_key;
    if (prop_name_equals(key, "ro.oplus.version.base"))
        return props->base_version;
    return NULL;
}

static bool prop_find_value_in_blob(const uint8_t *data, int len,
                                    const char *target_key,
                                    char *value_buf, size_t value_cap)
{
    int line_start = 0;

    if (!data || len <= 0 || !target_key || !target_key[0] || !value_buf || value_cap == 0)
        return false;
    value_buf[0] = '\0';

    while (line_start < len) {
        while (line_start < len && (data[line_start] == (uint8_t)'\n' || data[line_start] == (uint8_t)'\r'))
            line_start++;
        if (line_start >= len)
            break;

        int line_end = line_start;
        while (line_end < len && data[line_end] != (uint8_t)'\n' && data[line_end] != (uint8_t)'\r')
            line_end++;

        int key_start = line_start;
        while (key_start < line_end && (data[key_start] == (uint8_t)' ' || data[key_start] == (uint8_t)'\t'))
            key_start++;

        if (key_start < line_end && data[key_start] != (uint8_t)'#') {
            int eq = key_start;
            while (eq < line_end && data[eq] != (uint8_t)'=')
                eq++;
            if (eq < line_end) {
                int key_end = eq;
                while (key_end > key_start &&
                       (data[key_end - 1] == (uint8_t)' ' || data[key_end - 1] == (uint8_t)'\t'))
                    key_end--;
                if ((int)strlen(target_key) == (key_end - key_start)
                    && memcmp(data + key_start, target_key, (size_t)(key_end - key_start)) == 0) {
                    int value_start = eq + 1;
                    while (value_start < line_end &&
                           (data[value_start] == (uint8_t)' ' || data[value_start] == (uint8_t)'\t'))
                        value_start++;
                    int value_end = line_end;
                    while (value_end > value_start &&
                           (data[value_end - 1] == (uint8_t)' ' || data[value_end - 1] == (uint8_t)'\t'))
                        value_end--;
                    if (value_end > value_start) {
                        size_t copy_len = (size_t)(value_end - value_start);
                        if (copy_len >= value_cap)
                            copy_len = value_cap - 1;
                        memcpy(value_buf, data + value_start, copy_len);
                        value_buf[copy_len] = '\0';
                        return true;
                    }
                }
            }
        }

        line_start = line_end + 1;
    }

    return false;
}

static bool prop_resolve_import_path(const uint8_t *data, int len,
                                     const char *current_path,
                                     const char *raw_path,
                                     const edl_android_props_t *props,
                                     char *out_path, size_t out_cap)
{
    char expanded[PROP_PATH_MAX_LEN];
    size_t out_len = 0;

    if (!raw_path || !raw_path[0] || !out_path || out_cap == 0)
        return false;

    for (const char *p = raw_path; *p && out_len + 1 < sizeof(expanded); ) {
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            char key[96];
            char value[160];
            const char *fallback = NULL;
            size_t key_len = 0;
            size_t value_len = 0;

            if (!end)
                return false;
            key_len = (size_t)(end - (p + 2));
            if (key_len == 0 || key_len >= sizeof(key))
                return false;
            memcpy(key, p + 2, key_len);
            key[key_len] = '\0';

            if (!prop_find_value_in_blob(data, len, key, value, sizeof(value))) {
                fallback = prop_lookup_value_from_props(props, key);
                if (!fallback || !fallback[0])
                    return false;
                snprintf(value, sizeof(value), "%s", fallback);
            }

            value_len = strlen(value);
            if (out_len + value_len >= sizeof(expanded))
                return false;
            memcpy(expanded + out_len, value, value_len);
            out_len += value_len;
            p = end + 1;
            continue;
        }

        expanded[out_len++] = *p++;
    }
    expanded[out_len] = '\0';

    if (expanded[0] == '/') {
        snprintf(out_path, out_cap, "%s", expanded);
        return true;
    }

    if (current_path && current_path[0]) {
        const char *slash = strrchr(current_path, '/');
        if (slash && slash != current_path) {
            size_t dir_len = (size_t)(slash - current_path);
            if (dir_len + 1 + strlen(expanded) + 1 <= out_cap) {
                memcpy(out_path, current_path, dir_len);
                out_path[dir_len] = '/';
                snprintf(out_path + dir_len + 1, out_cap - dir_len - 1, "%s", expanded);
                return true;
            }
        }
    }

    snprintf(out_path, out_cap, "/%s", expanded);
    return true;
}

static int prop_collect_import_paths(const uint8_t *data, int len,
                                     const char *current_path,
                                     const edl_android_props_t *props,
                                     char import_paths[][PROP_PATH_MAX_LEN],
                                     int capacity)
{
    int line_start = 0;
    int count = 0;

    if (!data || len <= 0 || !import_paths || capacity <= 0)
        return 0;

    while (line_start < len && count < capacity) {
        while (line_start < len && (data[line_start] == (uint8_t)'\n' || data[line_start] == (uint8_t)'\r'))
            line_start++;
        if (line_start >= len)
            break;

        int line_end = line_start;
        while (line_end < len && data[line_end] != (uint8_t)'\n' && data[line_end] != (uint8_t)'\r')
            line_end++;

        int token_start = line_start;
        while (token_start < line_end && (data[token_start] == (uint8_t)' ' || data[token_start] == (uint8_t)'\t'))
            token_start++;

        if (token_start + 6 < line_end
            && memcmp(data + token_start, "import", 6) == 0
            && (data[token_start + 6] == (uint8_t)' ' || data[token_start + 6] == (uint8_t)'\t')) {
            int path_start = token_start + 6;
            char raw_path[PROP_PATH_MAX_LEN];
            char resolved[PROP_PATH_MAX_LEN];
            int raw_len = 0;
            bool duplicate = false;

            while (path_start < line_end &&
                   (data[path_start] == (uint8_t)' ' || data[path_start] == (uint8_t)'\t'))
                path_start++;

            int path_end = path_start;
            while (path_end < line_end
                   && data[path_end] != (uint8_t)' '
                   && data[path_end] != (uint8_t)'\t'
                   && data[path_end] != (uint8_t)'#') {
                path_end++;
            }

            raw_len = path_end - path_start;
            if (raw_len > 1
                && ((data[path_start] == (uint8_t)'\"' && data[path_end - 1] == (uint8_t)'\"')
                    || (data[path_start] == (uint8_t)'\'' && data[path_end - 1] == (uint8_t)'\''))) {
                path_start++;
                raw_len -= 2;
            }

            if (raw_len > 0 && raw_len < (int)sizeof(raw_path)) {
                memcpy(raw_path, data + path_start, (size_t)raw_len);
                raw_path[raw_len] = '\0';
                if (prop_resolve_import_path(data, len, current_path, raw_path, props,
                                             resolved, sizeof(resolved))) {
                    for (int i = 0; i < count; i++) {
                        if (strcmp(import_paths[i], resolved) == 0) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        snprintf(import_paths[count], PROP_PATH_MAX_LEN, "%s", resolved);
                        count++;
                    }
                }
            }
        }

        line_start = line_end + 1;
    }

    return count;
}

static bool probe_prop_text_path_deep(void *parser,
                                      edl_prop_probe_read_text_fn read_text,
                                      const char *fs_name,
                                      const char *path,
                                      edl_android_props_t *out,
                                      edl_prop_probe_log_fn log_fn,
                                      void *log_user,
                                      char seen_paths[][PROP_PATH_MAX_LEN],
                                      int *seen_count,
                                      int *progress_current,
                                      int *progress_total,
                                      edl_prop_probe_progress_fn progress_fn,
                                      void *progress_user,
                                      int depth)
{
    char text[256 * 1024];
    char import_paths[PROP_IMPORTS_PER_FILE][PROP_PATH_MAX_LEN];
    bool found = false;

    if (!parser || !read_text || !path || !path[0] || !out)
        return false;
    if (depth > PROP_IMPORT_MAX_DEPTH)
        return false;
    if (prop_path_seen_buf(path, seen_paths, seen_count ? *seen_count : 0))
        return false;
    if (seen_paths && seen_count)
        prop_path_mark_seen_buf(path, seen_paths, seen_count);

    if (progress_current) {
        (*progress_current)++;
        prop_progress_report(progress_fn, progress_user, *progress_current,
                             progress_total ? *progress_total : 0);
    }

    {
        int n = read_text(parser, path, text, (int)sizeof(text));
        if (n > 0) {
            if (log_fn) {
                char msg[320];
                snprintf(msg, sizeof(msg), "[props %s] read %s", fs_name, path);
                log_fn(msg, log_user);
            }

            if (scan_prop_blob_into((const uint8_t *)text, n, out))
                found = true;

            if (!props_scan_complete(out) && depth < PROP_IMPORT_MAX_DEPTH) {
                const int import_count =
                    prop_collect_import_paths((const uint8_t *)text, n, path, out,
                                              import_paths,
                                              PROP_IMPORTS_PER_FILE);
                for (int i = 0; i < import_count; i++) {
                    if (progress_total)
                        *progress_total += 2;
                    if (log_fn) {
                        char msg[320];
                        snprintf(msg, sizeof(msg), "[props %s] follow import %s", fs_name, import_paths[i]);
                        log_fn(msg, log_user);
                    }
                    if (probe_prop_text_path_deep(parser, read_text, fs_name, import_paths[i], out,
                                                  log_fn, log_user, seen_paths, seen_count,
                                                  progress_current, progress_total,
                                                  progress_fn, progress_user, depth + 1)) {
                        found = true;
                    }
                    if (props_scan_complete(out))
                        break;
                }
            }
        }
    }

    if (progress_current) {
        (*progress_current)++;
        prop_progress_report(progress_fn, progress_user, *progress_current,
                             progress_total ? *progress_total : 0);
    }

    return found;
}

static bool probe_prop_path_list_deep(void *parser,
                                      edl_prop_probe_read_text_fn read_text,
                                      const char *fs_name,
                                      const char *const *paths,
                                      edl_android_props_t *out,
                                      edl_prop_probe_log_fn log_fn,
                                      void *log_user,
                                      char seen_paths[][PROP_PATH_MAX_LEN],
                                      int *seen_count,
                                      int *progress_current,
                                      int *progress_total,
                                      edl_prop_probe_progress_fn progress_fn,
                                      void *progress_user)
{
    bool found = false;

    if (!parser || !read_text || !paths || !out)
        return false;

    for (int i = 0; paths[i]; i++) {
        if (probe_prop_text_path_deep(parser, read_text, fs_name, paths[i], out,
                                      log_fn, log_user, seen_paths, seen_count,
                                      progress_current, progress_total,
                                      progress_fn, progress_user, 0)) {
            found = true;
        }
    }

    return found;
}

static bool prop_is_key_char(uint8_t ch)
{
    return (ch >= (uint8_t)'a' && ch <= (uint8_t)'z')
        || (ch >= (uint8_t)'A' && ch <= (uint8_t)'Z')
        || (ch >= (uint8_t)'0' && ch <= (uint8_t)'9')
        || ch == (uint8_t)'.'
        || ch == (uint8_t)'_'
        || ch == (uint8_t)'-';
}

static bool scan_prop_binary_blob_into(const uint8_t *data, int len, edl_android_props_t *out)
{
    if (!data || len <= 0 || !out)
        return false;

    uint32_t found_mask = 0;
    const uint32_t full_mask = prop_fields_full_mask();

    for (int i = 0; i + 3 < len; i++) {
        if (data[i] != (uint8_t)'r' || data[i + 1] != (uint8_t)'o' || data[i + 2] != (uint8_t)'.')
            continue;
        if (i > 0 && prop_is_key_char(data[i - 1]))
            continue;

        int key_start = i;
        int key_end = i;
        while (key_end < len && prop_is_key_char(data[key_end]))
            key_end++;

        int eq = key_end;
        while (eq < len && (data[eq] == (uint8_t)' ' || data[eq] == (uint8_t)'\t'))
            eq++;
        if (eq >= len || data[eq] != (uint8_t)'=')
            continue;

        int val_start = eq + 1;
        while (val_start < len && (data[val_start] == (uint8_t)' ' || data[val_start] == (uint8_t)'\t'))
            val_start++;

        int val_end = val_start;
        while (val_end < len) {
            const uint8_t ch = data[val_end];
            if (ch == (uint8_t)'\n' || ch == (uint8_t)'\r' || ch == 0)
                break;
            if (ch < 0x20 && ch != (uint8_t)'\t')
                break;
            if (ch == 0x7f)
                break;
            val_end++;
        }
        while (val_end > val_start
               && (data[val_end - 1] == (uint8_t)' ' || data[val_end - 1] == (uint8_t)'\t'))
            val_end--;

        if (key_end > key_start && val_end > val_start) {
            (void)prop_try_store_pair(out,
                                      data + key_start, key_end - key_start,
                                      data + val_start, val_end - val_start,
                                      &found_mask);
            if (found_mask == full_mask)
                break;
        }

        i = key_end;
    }

    if (any_prop_set(out)) {
        normalize_props_after_scan(out);
        return true;
    }
    return false;
}

static void prop_copy_if_empty(char *dst, size_t dst_size, const char *src, int src_len)
{
    if (!dst || dst_size == 0 || dst[0] != '\0' || !src || src_len <= 0)
        return;

    int copy_len = src_len;
    if (copy_len >= (int)dst_size)
        copy_len = (int)dst_size - 1;
    memcpy(dst, src, (size_t)copy_len);
    dst[copy_len] = '\0';
}

static void prop_copy_if_missing_or_generic(char *dst, size_t dst_size, const char *src, int src_len)
{
    if (!dst || dst_size == 0 || !src || src_len <= 0)
        return;
    if (dst[0] != '\0' && !prop_value_looks_generic(dst))
        return;

    {
        int copy_len = src_len;
        if (copy_len >= (int)dst_size)
            copy_len = (int)dst_size - 1;
        memcpy(dst, src, (size_t)copy_len);
        dst[copy_len] = '\0';
    }
}

static void enrich_props_from_display_id(edl_android_props_t *out)
{
    const char *display = out ? out->display_id : NULL;
    const char *end = NULL;
    bool has_alpha = false;
    bool has_digit = false;

    if (!display || !display[0])
        return;

    end = display;
    while (*end && *end != '_' && *end != ' ')
        end++;

    const int token_len = (int)(end - display);
    if (token_len < 4 || token_len > 32)
        return;

    for (int i = 0; i < token_len; i++) {
        const unsigned char ch = (unsigned char)display[i];
        if (isalpha(ch))
            has_alpha = true;
        else if (isdigit(ch))
            has_digit = true;
        else
            return;
    }

    if (!has_alpha || !has_digit)
        return;

    prop_copy_if_missing_or_generic(out->model, sizeof(out->model), display, token_len);
    prop_copy_if_missing_or_generic(out->device, sizeof(out->device), display, token_len);
    prop_copy_if_missing_or_generic(out->product, sizeof(out->product), display, token_len);
}

static void enrich_props_from_model_hint(edl_android_props_t *out)
{
    const char *model = out ? out->model : NULL;

    if (!model || !model[0])
        return;

    if (prop_starts_with_nocase(model, "RMX")) {
        prop_copy_if_missing_or_generic(out->brand, sizeof(out->brand), "Realme", 6);
        prop_copy_if_missing_or_generic(out->manufacturer, sizeof(out->manufacturer), "Realme", 6);
    }
}

static void enrich_props_from_market_name(edl_android_props_t *out)
{
    const char *market = out ? out->market_name : NULL;

    if (!market || !market[0])
        return;

    if (prop_starts_with_nocase(market, "realme ")) {
        prop_copy_if_missing_or_generic(out->brand, sizeof(out->brand), "Realme", 6);
        prop_copy_if_missing_or_generic(out->manufacturer, sizeof(out->manufacturer), "Realme", 6);
    } else if (prop_starts_with_nocase(market, "oppo ")) {
        prop_copy_if_missing_or_generic(out->brand, sizeof(out->brand), "OPPO", 4);
        prop_copy_if_missing_or_generic(out->manufacturer, sizeof(out->manufacturer), "OPPO", 4);
    } else if (prop_starts_with_nocase(market, "oneplus ")) {
        prop_copy_if_missing_or_generic(out->brand, sizeof(out->brand), "OnePlus", 7);
        prop_copy_if_missing_or_generic(out->manufacturer, sizeof(out->manufacturer), "OnePlus", 7);
    }
}

static void enrich_props_from_fingerprint(edl_android_props_t *out)
{
    const char *fp = out ? out->fingerprint : NULL;
    const char *p1;
    const char *p2;
    const char *p3;
    const char *p4;
    const char *p5;
    const char *p6;

    if (!fp || !fp[0])
        return;

    p1 = strchr(fp, '/');
    p2 = p1 ? strchr(p1 + 1, '/') : NULL;
    p3 = p2 ? strchr(p2 + 1, ':') : NULL;
    p4 = p3 ? strchr(p3 + 1, '/') : NULL;
    p5 = p4 ? strchr(p4 + 1, '/') : NULL;
    p6 = p5 ? strchr(p5 + 1, ':') : NULL;

    if (p1)
        prop_copy_if_empty(out->brand, sizeof(out->brand), fp, (int)(p1 - fp));
    if (p2)
        prop_copy_if_empty(out->product, sizeof(out->product), p1 + 1, (int)(p2 - (p1 + 1)));
    if (p3)
        prop_copy_if_empty(out->device, sizeof(out->device), p2 + 1, (int)(p3 - (p2 + 1)));
    if (p4)
        prop_copy_if_empty(out->android_release, sizeof(out->android_release), p3 + 1,
                           (int)(p4 - (p3 + 1)));
    if (p5)
        prop_copy_if_empty(out->build_id, sizeof(out->build_id), p4 + 1, (int)(p5 - (p4 + 1)));
    if (p6)
        prop_copy_if_empty(out->incremental, sizeof(out->incremental), p5 + 1,
                           (int)(p6 - (p5 + 1)));
}

static void normalize_props_after_scan(edl_android_props_t *out)
{
    if (!out)
        return;

    enrich_props_from_fingerprint(out);
    enrich_props_from_display_id(out);
    enrich_props_from_model_hint(out);
    enrich_props_from_market_name(out);

    if (!out->manufacturer[0] && out->brand[0])
        snprintf(out->manufacturer, sizeof(out->manufacturer), "%s", out->brand);
    else if (!out->brand[0] && out->manufacturer[0])
        snprintf(out->brand, sizeof(out->brand), "%s", out->manufacturer);
}

static void merge_props_into(edl_android_props_t *dst, const edl_android_props_t *src,
                             bool prefer_build_fields)
{
    if (!dst || !src)
        return;

    for (size_t i = 0; i < sizeof(g_prop_fields) / sizeof(g_prop_fields[0]); i++) {
        const char *src_value = prop_field_value_cstr(src, i);
        const char *dst_value = prop_field_value_cstr(dst, i);
        const bool src_has = src_value && src_value[0] != '\0';
        const bool dst_has = dst_value && dst_value[0] != '\0';
        bool allow_replace = false;

        if (!src_has)
            continue;
        if (!dst_has) {
            allow_replace = true;
        } else if (prop_field_is_build_field(i) && prefer_build_fields) {
            allow_replace = true;
        } else if (prop_value_looks_generic(dst_value) && !prop_value_looks_generic(src_value)) {
            allow_replace = true;
        }

        if (allow_replace)
            prop_store_value(dst, i, (const uint8_t *)src_value, (int)strlen(src_value), true, NULL);
    }
}

static void clear_android_prop_strings(edl_android_props_t *out)
{
    if (!out) return;
    for (size_t i = 0; i < sizeof(g_prop_fields) / sizeof(g_prop_fields[0]); i++) {
        char *dst = prop_field_ptr(out, &g_prop_fields[i]);
        dst[0] = '\0';
    }
}

static int props_quality_score(const edl_android_props_t *out)
{
    int score = 0;
    if (!out)
        return 0;

    for (size_t i = 0; i < sizeof(g_prop_fields) / sizeof(g_prop_fields[0]); i++) {
        if (!prop_field_has_value(out, i))
            continue;
        if (!prop_field_is_build_field(i) && prop_value_looks_generic(prop_field_value_cstr(out, i)))
            continue;
        score += prop_field_is_build_field(i) ? 2 : 1;
    }

    if (out->fingerprint[0])
        score += prop_value_looks_generic(out->fingerprint) ? 1 : 4;
    if (out->android_release[0])
        score += 3;
    if (out->market_name[0] && !prop_value_looks_generic(out->market_name))
        score += 2;
    if (out->model[0] && !prop_value_looks_generic(out->model))
        score += 2;
    return score;
}

static bool props_core_complete(const edl_android_props_t *out)
{
    if (!out)
        return false;

    return out->brand[0] && !prop_value_looks_generic(out->brand)
        && out->manufacturer[0] && !prop_value_looks_generic(out->manufacturer)
        && out->model[0] && !prop_value_looks_generic(out->model)
        && out->device[0] && !prop_value_looks_generic(out->device)
        && out->product[0] && !prop_value_looks_generic(out->product)
        && out->android_release[0]
        && out->fingerprint[0]
        && out->build_id[0]
        && out->incremental[0]
        && out->display_id[0]
        && out->sdk[0];
}

static bool raw_scan_kv_into(const uint8_t *data, int len, edl_android_props_t *out)
{
    if (!data || len < 64 || !out)
        return false;
    return scan_prop_blob_into(data, len, out);
}

static bool raw_scan_android_props(const uint8_t *data, int len, edl_android_props_t *out,
                                   edl_prop_probe_log_fn log_fn, void *log_user)
{
    if (!raw_scan_kv_into(data, len, out))
        return false;
    snprintf(out->fs_type, sizeof(out->fs_type), "text_scan");
    out->volume_label[0] = '\0';
    if (log_fn)
        log_fn("[props] text_scan：缓冲区中发现 ro.* 属性", log_user);
    return true;
}

typedef int (*prop_read_text_fn)(void *parser, const char *path, char *buf, int buf_size);

static bool raw_scan_android_props_merged(const uint8_t *data, int len, edl_android_props_t *out,
                                          edl_prop_probe_log_fn log_fn, void *log_user)
{
    edl_android_props_t merged;
    edl_android_props_t candidate;
    bool found = false;

    if (!out)
        return false;

    memset(&merged, 0, sizeof(merged));
    memset(&candidate, 0, sizeof(candidate));

    if (raw_scan_kv_into(data, len, &merged))
        found = true;

    if (!props_scan_complete(&merged)) {
        if (scan_prop_binary_blob_into(data, len, &candidate)) {
            if (found)
                merge_props_into(&merged, &candidate, true);
            else
                merged = candidate;
            found = true;
        }
    }

    if (!found)
        return false;

    *out = merged;
    snprintf(out->fs_type, sizeof(out->fs_type), "text_scan");
    out->volume_label[0] = '\0';
    if (log_fn)
        log_fn("[props] text_scan：缓冲区中发现 ro.* 属性", log_user);
    return true;
}

static int ext4_read_text_bridge(void *parser, const char *path, char *buf, int buf_size)
{
    return ext4_read_text((ext4_parser_t *)parser, path, buf, buf_size);
}

static int erofs_read_text_bridge(void *parser, const char *path, char *buf, int buf_size)
{
    return erofs_read_text((erofs_parser_t *)parser, path, buf, buf_size);
}

bool edl_probe_android_props_from_filesystem_ex(void *parser,
                                                edl_prop_probe_read_text_fn read_text,
                                                const char *fs_name,
                                                edl_android_props_t *out,
                                                edl_prop_probe_log_fn log_fn,
                                                void *log_user,
                                                edl_prop_probe_progress_fn progress_fn,
                                                void *progress_user)
{
    bool found = false;
    const char *const *label_paths = NULL;
    const char *dynamic_paths[16];
    char dynamic_path_buf[16][128];
    int dynamic_path_count = 0;
    char seen_paths[PROP_PATH_SEEN_MAX][PROP_PATH_MAX_LEN];
    int seen_count = 0;
    int progress_current = 0;
    int progress_total = 0;
    const char *label_hint = NULL;

    if (!parser || !read_text || !out)
        return false;

    memset(seen_paths, 0, sizeof(seen_paths));
    progress_total += prop_path_list_count(g_prop_root_paths) * 2;
    progress_total += prop_path_list_count(g_prop_text_paths) * 2;
    progress_total += 2;

    label_hint = prop_partition_hint_label(out);
    label_paths = prop_partition_paths_for_label(label_hint);
    if (label_paths)
        progress_total += prop_path_list_count(label_paths) * 2;
    prop_progress_report(progress_fn, progress_user, 0, progress_total);

    if (probe_prop_path_list_deep(parser, read_text, fs_name, g_prop_root_paths, out,
                                  log_fn, log_user, seen_paths, &seen_count,
                                  &progress_current, &progress_total,
                                  progress_fn, progress_user)) {
        found = true;
    }

    label_hint = prop_partition_hint_label(out);
    label_paths = prop_partition_paths_for_label(label_hint);
    if (label_paths) {
        if (probe_prop_path_list_deep(parser, read_text, fs_name, label_paths, out,
                                      log_fn, log_user, seen_paths, &seen_count,
                                      &progress_current, &progress_total,
                                      progress_fn, progress_user)) {
            found = true;
        }
    }

    label_hint = prop_partition_hint_label(out);
    dynamic_path_count = prop_partition_dynamic_paths_for_label(label_hint, out,
                                                                dynamic_paths,
                                                                dynamic_path_buf,
                                                                16);
    if (dynamic_path_count > 0) {
        progress_total += dynamic_path_count * 2;
        dynamic_paths[dynamic_path_count] = NULL;
        if (probe_prop_path_list_deep(parser, read_text, fs_name, dynamic_paths, out,
                                      log_fn, log_user, seen_paths, &seen_count,
                                      &progress_current, &progress_total,
                                      progress_fn, progress_user)) {
            found = true;
        }
    }

    if (probe_prop_path_list_deep(parser, read_text, fs_name, g_prop_text_paths, out,
                                  log_fn, log_user, seen_paths, &seen_count,
                                  &progress_current, &progress_total,
                                  progress_fn, progress_user)) {
        found = true;
    }

    prop_progress_report(progress_fn, progress_user, progress_total, progress_total);
    return found;
}

bool edl_probe_android_props_from_filesystem(void *parser,
                                             edl_prop_probe_read_text_fn read_text,
                                             const char *fs_name,
                                             edl_android_props_t *out,
                                             edl_prop_probe_log_fn log_fn,
                                             void *log_user)
{
    return edl_probe_android_props_from_filesystem_ex(parser, read_text, fs_name,
                                                      out, log_fn, log_user,
                                                      NULL, NULL);
}

static int volume_label_priority(const char *label)
{
    if (!label || !label[0])
        return 0;
#ifdef _WIN32
    if (_stricmp(label, "system") == 0 || _stricmp(label, "system_a") == 0
        || _stricmp(label, "system_b") == 0)
        return 500;
    if (_stricmp(label, "product") == 0 || _stricmp(label, "product_a") == 0
        || _stricmp(label, "product_b") == 0 || _stricmp(label, "my_product") == 0
        || _stricmp(label, "my_product_a") == 0 || _stricmp(label, "my_product_b") == 0)
        return 460;
    if (_stricmp(label, "system_ext") == 0 || _stricmp(label, "system_ext_a") == 0
        || _stricmp(label, "system_ext_b") == 0)
        return 430;
    if (_stricmp(label, "vendor") == 0 || _stricmp(label, "vendor_a") == 0
        || _stricmp(label, "vendor_b") == 0)
        return 380;
    if (_stricmp(label, "odm") == 0 || _stricmp(label, "odm_a") == 0
        || _stricmp(label, "odm_b") == 0 || _stricmp(label, "cust") == 0
        || _stricmp(label, "cust_a") == 0 || _stricmp(label, "cust_b") == 0)
        return 340;
#else
    if (strcasecmp(label, "system") == 0 || strcasecmp(label, "system_a") == 0
        || strcasecmp(label, "system_b") == 0)
        return 500;
    if (strcasecmp(label, "product") == 0 || strcasecmp(label, "product_a") == 0
        || strcasecmp(label, "product_b") == 0 || strcasecmp(label, "my_product") == 0
        || strcasecmp(label, "my_product_a") == 0 || strcasecmp(label, "my_product_b") == 0)
        return 460;
    if (strcasecmp(label, "system_ext") == 0 || strcasecmp(label, "system_ext_a") == 0
        || strcasecmp(label, "system_ext_b") == 0)
        return 430;
    if (strcasecmp(label, "vendor") == 0 || strcasecmp(label, "vendor_a") == 0
        || strcasecmp(label, "vendor_b") == 0)
        return 380;
    if (strcasecmp(label, "odm") == 0 || strcasecmp(label, "odm_a") == 0
        || strcasecmp(label, "odm_b") == 0 || strcasecmp(label, "cust") == 0
        || strcasecmp(label, "cust_a") == 0 || strcasecmp(label, "cust_b") == 0)
        return 340;
#endif
    return 0;
}

static bool get_ext4_props(ext4_parser_t *p, edl_android_props_t *out,
                           edl_prop_probe_log_fn log_fn, void *log_user)
{
    return edl_probe_android_props_from_filesystem(p, ext4_read_text_bridge, "ext4",
                                                   out, log_fn, log_user);
}

static bool get_erofs_props(erofs_parser_t *p, edl_android_props_t *out,
                            edl_prop_probe_log_fn log_fn, void *log_user)
{
    return edl_probe_android_props_from_filesystem(p, erofs_read_text_bridge, "erofs",
                                                   out, log_fn, log_user);
}

static uint32_t read_le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static bool quick_ext4_sb(const uint8_t *d, int len, int64_t off)
{
    if (off < 0 || off + (int64_t)EXT4_MAGIC_FILE_OFFSET + 4 > len)
        return false;
    size_t base = (size_t)off;
    uint16_t m = (uint16_t)(d[base + EXT4_MAGIC_FILE_OFFSET] |
                            (d[base + EXT4_MAGIC_FILE_OFFSET + 1] << 8));
    if (m != EXT4_SUPER_MAGIC)
        return false;
    if (read_le32(d + base + EXT4_SUPERBLOCK_OFF + 24) > 16u)
        return false;
    return true;
}

static bool quick_erofs_sb(const uint8_t *d, int len, int64_t off)
{
    if (off < 0 || off + (int64_t)EROFS_MAGIC_FILE_OFFSET + 4 > len)
        return false;
    return read_le32(d + (size_t)off + EROFS_SUPERBLOCK_OFF) == EROFS_SUPER_MAGIC;
}

static bool try_one_offset(const uint8_t *data, int len, int64_t off, edl_android_props_t *out,
                           log_wrap_t *lw)
{
    if (!data || !out || off < 0 || len - off < 8192)
        return false;

    mem_rd_ctx_t mc = { data + off, len - off };

    if (ext4_detect(mem_read_fn, &mc)) {
        ext4_parser_t *p = ext4_open(mem_read_fn, &mc, wrap_ext4_log, lw);
        if (p) {
            char volsave[sizeof(out->volume_label)];
            snprintf(volsave, sizeof(volsave), "%s", ext4_volume_name(p));
            snprintf(out->fs_type, sizeof(out->fs_type), "ext4");
            snprintf(out->volume_label, sizeof(out->volume_label), "%s", volsave);
            (void)get_ext4_props(p, out, lw ? lw->fn : NULL, lw ? lw->user : NULL);
            ext4_close(p);
            if (any_prop_set(out))
                return true;

            /* 前缀缓冲里的 ext4 只够识别文件系统，但未必够读到真正的 build.prop；
             * 这里不能退回 text_scan，否则会把截断数据里的零散 ro.* 误判成成功结果。 */
            memset(out, 0, sizeof(*out));
            return false;

            clear_android_prop_strings(out);
            snprintf(out->fs_type, sizeof(out->fs_type), "ext4");
            snprintf(out->volume_label, sizeof(out->volume_label), "%s", volsave);
            if (raw_scan_kv_into(data + off, (int)(len - off), out)) {
                if (lw && lw->fn)
                    lw->fn("[props] ext4：文件遍历未命中，回退到前缀 ro.* 扫描", lw->user);
                return true;
            }
            memset(out, 0, sizeof(*out));
        }
    }

    mc.base = data + off;
    mc.len = len - off;
    if (erofs_detect(mem_read_fn, &mc)) {
        erofs_parser_t *p = erofs_open(mem_read_fn, &mc, wrap_erofs_log, lw);
        if (p) {
            char volsave[sizeof(out->volume_label)];
            snprintf(volsave, sizeof(volsave), "%s", erofs_volume_name(p));
            snprintf(out->fs_type, sizeof(out->fs_type), "erofs");
            snprintf(out->volume_label, sizeof(out->volume_label), "%s", volsave);
            (void)get_erofs_props(p, out, lw ? lw->fn : NULL, lw ? lw->user : NULL);
            erofs_close(p);
            if (any_prop_set(out))
                return true;

            /* 同 ext4：识别到 EROFS 但没从真实文件里拿到属性时，宁可继续扩大/走 live-fs，
             * 也不要把前缀里的零散 ro.* 当成最终结果。 */
            memset(out, 0, sizeof(*out));
            return false;

            clear_android_prop_strings(out);
            snprintf(out->fs_type, sizeof(out->fs_type), "erofs");
            snprintf(out->volume_label, sizeof(out->volume_label), "%s", volsave);
            if (raw_scan_kv_into(data + off, (int)(len - off), out)) {
                if (lw && lw->fn)
                    lw->fn("[props] erofs：文件遍历未命中，回退到前缀 ro.* 扫描", lw->user);
                return true;
            }
            memset(out, 0, sizeof(*out));
        }
    }

    return false;
}

bool edl_probe_android_props_from_buffer(const uint8_t *data, int len,
                                         edl_android_props_t *out,
                                         edl_prop_probe_log_fn log_fn, void *log_user)
{
    int64_t z = 0;
    return edl_probe_android_props_from_buffer_scanned(data, len, out, &z, false, log_fn,
                                                       log_user);
}

#ifndef EDL_PROBE_SCAN_MIN_TAIL
#define EDL_PROBE_SCAN_MIN_TAIL (32 * 1024 * 1024)
#endif
#ifndef EDL_PROBE_SCAN_STEP
#define EDL_PROBE_SCAN_STEP 4096
#endif
#ifndef EDL_PROBE_SCAN_MIN_TAIL_SMALL
#define EDL_PROBE_SCAN_MIN_TAIL_SMALL (8 * 1024 * 1024)
#endif

bool edl_probe_android_props_from_buffer_scanned(const uint8_t *data, int len,
                                                 edl_android_props_t *out,
                                                 int64_t *out_fs_offset,
                                                 bool scan_embedded,
                                                 edl_prop_probe_log_fn log_fn,
                                                 void *log_user)
{
    if (!data || len < 1024 || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->fs_embed_offset = 0;

    log_wrap_t lw = { log_fn, log_user };
    edl_android_props_t merged;
    edl_android_props_t best_meta;
    bool found = false;
    bool fs_detected = false;
    int best_score = 0;
    int64_t best_off = 0;

    memset(&merged, 0, sizeof(merged));
    memset(&best_meta, 0, sizeof(best_meta));

    {
        edl_android_props_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        if (quick_ext4_sb(data, len, 0) || quick_erofs_sb(data, len, 0))
            fs_detected = true;
        if (try_one_offset(data, len, 0, &candidate, &lw)) {
            if (!scan_embedded) {
                *out = candidate;
                if (out_fs_offset)
                    *out_fs_offset = 0;
                return true;
            }
            merged = candidate;
            best_meta = candidate;
            best_score = props_quality_score(&candidate) + volume_label_priority(candidate.volume_label);
            best_off = 0;
            found = true;
            if (props_scan_complete(&merged)) {
                *out = merged;
                if (out_fs_offset)
                    *out_fs_offset = 0;
                return true;
            }
        }
    }

    if (scan_embedded) {
        int64_t min_tail = (len >= EDL_PROBE_SCAN_MIN_TAIL) ? EDL_PROBE_SCAN_MIN_TAIL
                                                            : EDL_PROBE_SCAN_MIN_TAIL_SMALL;
        if (len >= min_tail + 1024 + 64) {
            int64_t max_off = len - min_tail;
            for (int64_t off = EDL_PROBE_SCAN_STEP; off <= max_off; off += EDL_PROBE_SCAN_STEP) {
                const bool ext4_hit = quick_ext4_sb(data, len, off);
                const bool erofs_hit = quick_erofs_sb(data, len, off);
                if (!ext4_hit && !erofs_hit)
                    continue;
                fs_detected = true;

                edl_android_props_t candidate;
                memset(&candidate, 0, sizeof(candidate));
                if (!try_one_offset(data, len, off, &candidate, &lw))
                    continue;

                const int score = props_quality_score(&candidate)
                                + volume_label_priority(candidate.volume_label);
                if (!found) {
                    merged = candidate;
                    best_meta = candidate;
                    best_score = score;
                    best_off = off;
                    found = true;
                } else {
                    const bool prefer_build_fields = score > best_score;
                    merge_props_into(&merged, &candidate, prefer_build_fields);
                    if (score > best_score) {
                        best_meta = candidate;
                        best_score = score;
                        best_off = off;
                    }
                }

                if (props_scan_complete(&merged))
                    break;
            }
        }

        if (found) {
            snprintf(merged.fs_type, sizeof(merged.fs_type), "%s", best_meta.fs_type);
            snprintf(merged.volume_label, sizeof(merged.volume_label), "%s", best_meta.volume_label);
            *out = merged;
            if (out_fs_offset)
                *out_fs_offset = best_off;
            return true;
        }
    }

    if (fs_detected) {
        if (out_fs_offset)
            *out_fs_offset = 0;
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (raw_scan_android_props_merged(data, len, out, log_fn, log_user)) {
        if (out_fs_offset)
            *out_fs_offset = 0;
        return true;
    }

    if (out_fs_offset)
        *out_fs_offset = 0;
    return false;
}
