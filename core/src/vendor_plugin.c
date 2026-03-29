#include "vendor_plugin.h"

#include "edl/realme_auth.h"
#include "edl/xiaomi_auth.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define plugin_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define plugin_sleep_ms(ms) usleep((ms) * 1000)
#endif

#define PLUGIN_READ_POLL_SLICE_MS 200

static void plugin_vlog(const edl_vendor_plugin_ctx_t *ctx, bool detail,
                        const char *fmt, va_list ap)
{
    char buf[512];
    if (!ctx || !ctx->cb)
        return;

    edl_log_cb fn = detail ? ctx->cb->log_detail : ctx->cb->log;
    if (!fn)
        return;

    vsnprintf(buf, sizeof(buf), fmt, ap);
    if (!detail && strstr(buf, "Realme") != NULL && strstr(buf, "MSM 0x") != NULL)
        snprintf(buf, sizeof(buf), "%s", "Realme：开始认证...");
    fn(buf, ctx->cb->user_data);
}

static void plugin_log(const edl_vendor_plugin_ctx_t *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    plugin_vlog(ctx, false, fmt, ap);
    va_end(ap);
}

static void plugin_log_detail(const edl_vendor_plugin_ctx_t *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    plugin_vlog(ctx, true, fmt, ap);
    va_end(ap);
}

static bool plugin_is_cancelled(const edl_vendor_plugin_ctx_t *ctx)
{
    return ctx && ctx->cb && ctx->cb->is_cancelled
        && ctx->cb->is_cancelled(ctx->cb->user_data);
}

static bool plugin_sleep_cancelable(const edl_vendor_plugin_ctx_t *ctx, int total_ms)
{
    while (total_ms > 0) {
        if (plugin_is_cancelled(ctx))
            return true;
        int slice = total_ms > 20 ? 20 : total_ms;
        plugin_sleep_ms(slice);
        total_ms -= slice;
    }
    return false;
}

static int plugin_port_read_response(edl_port_t *port, char *buf, int buf_size, int timeout_ms,
                                     const edl_callbacks_t *cb)
{
    int total = 0;
    int remaining_ms = timeout_ms;
    uint8_t tmp[4096];

    while (total < buf_size - 1 && remaining_ms > 0) {
        if (cb && cb->is_cancelled && cb->is_cancelled(cb->user_data))
            return EDL_ERR_CANCELLED;

        int chunk = buf_size - total - 1;
        int wait_ms = remaining_ms > PLUGIN_READ_POLL_SLICE_MS
                    ? PLUGIN_READ_POLL_SLICE_MS
                    : remaining_ms;
        if (chunk > (int)sizeof(tmp))
            chunk = (int)sizeof(tmp);
        int n = edl_port_read(port, tmp, chunk, wait_ms);
        remaining_ms -= wait_ms;
        if (n <= 0)
            continue;
        memcpy(buf + total, tmp, (size_t)n);
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "value=\"ACK\"") || strstr(buf, "value=\"NAK\"") ||
            strstr(buf, "rawmode=\"false\"") || strstr(buf, "rawmode=\"true\""))
            break;
    }

    buf[total] = '\0';
    return total;
}

static uint8_t *plugin_load_file(const char *path, size_t *out_size)
{
    if (!path || !out_size)
        return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return NULL;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(fp);
        return NULL;
    }

    if ((long)fread(data, 1, (size_t)size, fp) != size) {
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *out_size = (size_t)size;
    return data;
}

const char *edl_vendor_plugin_name(edl_svc_auth_mode_t mode)
{
    switch (mode) {
    case EDL_SVC_AUTH_OPLUS_VIP: return "OPLUS VIP";
    case EDL_SVC_AUTH_REALME: return "Realme";
    case EDL_SVC_AUTH_ONEPLUS: return "OnePlus";
    case EDL_SVC_AUTH_XIAOMI: return "Xiaomi";
    default: return "None";
    }
}

static edl_error_t plugin_run_oplus_vip(const edl_vendor_plugin_ctx_t *ctx)
{
    if (!ctx || !ctx->auth)
        return EDL_ERR_INVALID_PARAM;
    if (ctx->phase != EDL_VENDOR_PLUGIN_PRE_CONFIGURE)
        return EDL_OK;
    if (!ctx->auth->digest_path[0] || !ctx->auth->signature_path[0]) {
        plugin_log(ctx, "OPLUS VIP 缺少 Digest 或 Signature 文件");
        return EDL_ERR_INVALID_PARAM;
    }

    size_t digest_len = 0;
    size_t sig_len = 0;
    uint8_t *digest = plugin_load_file(ctx->auth->digest_path, &digest_len);
    uint8_t *signature = plugin_load_file(ctx->auth->signature_path, &sig_len);
    if (!digest || !signature || digest_len == 0 || sig_len == 0) {
        free(digest);
        free(signature);
        plugin_log(ctx, "OPLUS VIP 认证文件读取失败");
        return EDL_ERR_FILE_NOT_FOUND;
    }

    plugin_log(ctx, "OPLUS VIP：Digest %u B | Signature %u B",
               (unsigned)digest_len, (unsigned)sig_len);
    edl_error_t err = edl_realme_vip_authenticate(ctx->port,
                                                  digest, (int)digest_len,
                                                  signature, (int)sig_len,
                                                  ctx->cb);
    free(digest);
    free(signature);
    if (err == EDL_OK)
        plugin_log(ctx, "OPLUS VIP 认证成功");
    else
        plugin_log(ctx, "OPLUS VIP 认证失败: %s", edl_error_str(err));
    return err;
}

static edl_error_t plugin_run_realme(const edl_vendor_plugin_ctx_t *ctx)
{
    if (!ctx || !ctx->auth)
        return EDL_ERR_INVALID_PARAM;
    if (ctx->phase != EDL_VENDOR_PLUGIN_POST_CONFIGURE)
        return EDL_OK;
    if (!ctx->sahara || !ctx->chip_info) {
        plugin_log_detail(ctx, "Realme：当前为 Firehose 直连，跳过串口 getsigndata 认证");
        return EDL_OK;
    }
    if (!ctx->auth->realme_sign_cb) {
        plugin_log(ctx, "Realme 需要云端签名回调");
        return EDL_ERR_FH_AUTH_REQUIRED;
    }

    const uint32_t msm = ctx->chip_info->msm_id;
    if (msm == 0) {
        plugin_log(ctx, "Realme 需要有效的 MSM ID");
        return EDL_ERR_INVALID_PARAM;
    }

    size_t digest_len = 0;
    uint8_t *digest = NULL;
    if (ctx->auth->digest_path[0])
        digest = plugin_load_file(ctx->auth->digest_path, &digest_len);
    const bool have_digest = (digest != NULL && digest_len > 0);

    edl_realme_protocol_t proto = edl_realme_pick_auth_protocol(msm, have_digest);
    const char *proto_name =
        proto == REALME_PROTO_MODERN ? "Modern"
      : proto == REALME_PROTO_LEGACY_DIGEST ? "Legacy + initdigest"
      : proto == REALME_PROTO_LEGACY_SIMPLE ? "Legacy"
      : "Unknown";

    plugin_log(ctx, "Realme：芯片 %s | MSM 0x%08X | %s",
               edl_chip_name(msm), msm, proto_name);
    if (ctx->profile) {
        plugin_log_detail(ctx, "Realme：建议认证策略 %s | Loader 建议 %s",
                          edl_auth_hint_name(ctx->profile->auth_hint),
                          edl_loader_arch_hint_name(ctx->profile->loader_arch_hint));
    }

    const char *project_id = ctx->auth->project_id[0] ? ctx->auth->project_id : NULL;
    if (proto == REALME_PROTO_MODERN && (!project_id || !project_id[0])) {
        free(digest);
        plugin_log(ctx, "Realme Modern 协议缺少 ProjectID");
        return EDL_ERR_INVALID_PARAM;
    }
    if (proto == REALME_PROTO_LEGACY_DIGEST && !have_digest) {
        free(digest);
        plugin_log(ctx, "Realme Legacy + initdigest 缺少 Digest 文件");
        return EDL_ERR_INVALID_PARAM;
    }

    edl_error_t err = edl_realme_authenticate(ctx->port, proto, project_id,
                                              digest, have_digest ? (int)digest_len : 0,
                                              ctx->auth->realme_sign_cb,
                                              ctx->auth->realme_sign_user,
                                              ctx->cb);
    free(digest);
    if (err != EDL_OK) {
        plugin_log(ctx, "Realme 认证失败: %s", edl_error_str(err));
        return err;
    }

    plugin_log(ctx, "Realme 认证成功");
    edl_port_purge(ctx->port);
    if (plugin_sleep_cancelable(ctx, 200))
        return EDL_ERR_CANCELLED;

    err = edl_firehose_ping(ctx->firehose);
    if (err != EDL_OK) {
        plugin_log(ctx, "Realme 认证后链路探测失败: %s", edl_error_str(err));
        return err;
    }

    err = edl_firehose_configure(ctx->firehose, edl_firehose_storage_type(ctx->firehose), 0);
    if (err != EDL_OK) {
        plugin_log(ctx, "Realme 认证后重新配置 Firehose 失败: %s", edl_error_str(err));
        return err;
    }

    if (plugin_sleep_cancelable(ctx, 120))
        return EDL_ERR_CANCELLED;
    return EDL_OK;
}

static edl_error_t plugin_run_xiaomi(const edl_vendor_plugin_ctx_t *ctx)
{
    if (!ctx || !ctx->port || !ctx->firehose)
        return EDL_ERR_INVALID_PARAM;
    if (ctx->phase != EDL_VENDOR_PLUGIN_POST_CONFIGURE)
        return EDL_OK;

    char xml[512];
    char resp[8192];

    plugin_log(ctx, "Xiaomi：尝试内置签名认证");
    for (int i = 0; i < edl_xiaomi_builtin_sign_count(); i++) {
        edl_xiaomi_build_sig_request(xml, sizeof(xml));
        edl_port_write(ctx->port, (const uint8_t *)xml, (int)strlen(xml));
        int n = plugin_port_read_response(ctx->port, resp, sizeof(resp), 4000, ctx->cb);
        if (n == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;
        if (edl_realme_is_nak(resp))
            continue;

        uint8_t sig[384];
        int sig_len = edl_xiaomi_builtin_sign(i, sig, sizeof(sig));
        if (sig_len <= 0)
            continue;

        edl_port_write(ctx->port, sig, sig_len);
        n = plugin_port_read_response(ctx->port, resp, sizeof(resp), 6000, ctx->cb);
        if (n == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;

        if (edl_xiaomi_is_auth_success(resp)) {
            plugin_log(ctx, "Xiaomi 认证成功");
            (void)edl_firehose_ping(ctx->firehose);
            return EDL_OK;
        }
    }

    plugin_log(ctx, "Xiaomi 认证失败：内置签名未通过");
    return EDL_ERR_FH_AUTH_REQUIRED;
}

static edl_error_t plugin_run_oneplus(const edl_vendor_plugin_ctx_t *ctx)
{
    if (!ctx)
        return EDL_ERR_INVALID_PARAM;
    if (ctx->phase != EDL_VENDOR_PLUGIN_POST_CONFIGURE)
        return EDL_OK;
    plugin_log(ctx, "OnePlus 认证插件尚未接入");
    return EDL_ERR_INVALID_PARAM;
}

edl_error_t edl_vendor_plugin_run(edl_svc_auth_mode_t mode,
                                  const edl_vendor_plugin_ctx_t *ctx)
{
    switch (mode) {
    case EDL_SVC_AUTH_NONE:
        return EDL_OK;
    case EDL_SVC_AUTH_OPLUS_VIP:
        return plugin_run_oplus_vip(ctx);
    case EDL_SVC_AUTH_REALME:
        return plugin_run_realme(ctx);
    case EDL_SVC_AUTH_ONEPLUS:
        return plugin_run_oneplus(ctx);
    case EDL_SVC_AUTH_XIAOMI:
        return plugin_run_xiaomi(ctx);
    default:
        return EDL_ERR_INVALID_PARAM;
    }
}
