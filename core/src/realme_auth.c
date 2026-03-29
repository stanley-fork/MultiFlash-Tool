#include "edl/realme_auth.h"
#include "edl/chip_db.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

#define REALME_RESP_MAX (64 * 1024)
#define REALME_INITDIGEST_ACK_TIMEOUT_MS      3000
#define REALME_INITDIGEST_RESULT_TIMEOUT_MS   6000
#define REALME_GETSIGDATA_TIMEOUT_MS          9000
#define REALME_VERIFY_RAWMODE_TIMEOUT_MS      4000
#define REALME_SIGNATURE_RESULT_TIMEOUT_MS    5000
#define REALME_VIP_TRANSFERCFG_TIMEOUT_MS     2500
#define REALME_VIP_VERIFY_TIMEOUT_MS          2500
#define REALME_VIP_SIGNATURE_TIMEOUT_MS       3500
#define REALME_VIP_SHA256INIT_TIMEOUT_MS      2500
#define REALME_READ_POLL_SLICE_MS              250

/* ===== 内部日志辅助 ===== */

static bool ra_log_should_skip(const char *msg)
{
    if (!msg || !msg[0])
        return true;
    if (strncmp(msg, "Legacy:", 7) == 0)
        return true;
    if (strncmp(msg, "Modern:", 7) == 0)
        return true;
    if (strstr(msg, "ChipSn=") != NULL)
        return true;
    if (strstr(msg, "Rand=") != NULL)
        return true;
    if (strstr(msg, "已获取签名") != NULL)
        return true;
    if (strstr(msg, "设备侧 verify 已通过") != NULL)
        return true;
    if (strstr(msg, "设备返回 ACK") != NULL)
        return true;
    return false;
}

static void ra_log(const edl_callbacks_t *cb, const char *fmt, ...)
{
    if (!cb || !cb->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strstr(buf, "请求云端签名") != NULL)
        snprintf(buf, sizeof(buf), "%s", "Realme：云端认证中...");
    else if (ra_log_should_skip(buf))
        return;
    cb->log(buf, cb->user_data);
}

static void ra_detail(const edl_callbacks_t *cb, const char *fmt, ...)
{
    if (!cb || !cb->log_detail) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cb->log_detail(buf, cb->user_data);
}

static bool ra_is_cancelled(const edl_callbacks_t *cb)
{
    return cb && cb->is_cancelled && cb->is_cancelled(cb->user_data);
}

/* ===== XML 构建器 ===== */

int edl_realme_build_configure(char *buf, int buf_size,
                                const char *storage_type, int payload_size,
                                bool enable_vip)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<data>\n"
        "<configure MemoryName=\"%s\" Verbose=\"0\" "
        "AlwaysValidate=\"0\" MaxPayloadSizeToTargetInBytes=\"%d\" "
        "MaxPayloadSizeToTargetInBytesSupported=\"%d\" "
        "ZlpAwareHost=\"1\" SkipStorageInit=\"0\" "
        "EnableVip=\"%d\" />\n</data>\n",
        storage_type ? storage_type : "ufs",
        payload_size, payload_size,
        enable_vip ? 1 : 0);
}

int edl_realme_build_getsigndata(char *buf, int buf_size, const char *project_id)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<data>\n"
        "<getsigndata ProjectID=\"%s\" value=\"ping\" />\n</data>\n",
        project_id ? project_id : "");
}

int edl_realme_build_getsigndata_legacy(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<data>\n"
        "<getsigndata value=\"ping\" />\n</data>\n");
}

int edl_realme_build_initdigest(char *buf, int buf_size, int digest_data_len)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<data>\n"
        "<initdigest SECTOR_SIZE_IN_BYTES=\"1\" num_partition_sectors=\"%d\" />\n</data>\n",
        digest_data_len);
}

int edl_realme_build_verify_modern(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        "<data><verify EnableVip=\"0\" value=\"ping\"/></data>");
}

int edl_realme_build_verify_legacy(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        "<data><verify value=\"ping\"/></data>");
}

int edl_realme_build_transfercfg(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        "<data><transfercfg reboot_type=\"off\" timeout_in_sec=\"90\" /></data>");
}

int edl_realme_build_verify_vip(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        "<data><verify value=\"ping\" EnableVip=\"1\"/></data>");
}

int edl_realme_build_sha256init(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        "<data><sha256init Verbose=\"1\"/></data>");
}

int edl_realme_build_sha256final(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data>\n<sha256final />\n</data>\n");
}

int edl_realme_build_getstorageinfo(char *buf, int buf_size, int lun)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<data>\n"
        "<getstorageinfo physical_partition_number=\"%d\" />\n</data>\n",
        lun);
}

int edl_realme_build_nop(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<data>\n<nop />\n</data>\n");
}

/* ===== 属性提取 ===== */

static bool extract_attr(const char *xml, const char *attr, char *out, int out_size)
{
    char search[128];
    snprintf(search, sizeof(search), "%s=\"", attr);
    const char *p = strstr(xml, search);
    if (!p) return false;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

/* Sakura ExtractResponseValue：log 行内 Key:Value;（与 <log value="INFO: ..."/> 一致） */
static int strncasecmp_ascii(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (!ca && !cb) return 0;
        if (!ca) return -1;
        if (!cb) return 1;
        ca = (unsigned char)tolower((int)ca);
        cb = (unsigned char)tolower((int)cb);
        if (ca != cb) return (ca < cb) ? -1 : 1;
    }
    return 0;
}

static void copy_kv_token(const char *start, char *out, int out_size)
{
    int i = 0;
    while (*start && *start != ';' && *start != '"' && *start != '<' &&
           *start != '\r' && *start != '\n' && i < out_size - 1)
        out[i++] = *start++;
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t'))
        i--;
    out[i] = '\0';
}

static bool find_kv_colon(const char *response, const char *key, char *out, int out_size)
{
    size_t klen = strlen(key);
    const char *p = response;
    if (!response || !key || !out || out_size <= 0)
        return false;
    while (*p) {
        if (strncasecmp_ascii(p, key, klen) == 0 && p[klen] == ':') {
            if (p > response) {
                char prev = p[-1];
                if (isalnum((unsigned char)prev) || prev == '_') {
                    p++;
                    continue;
                }
            }
            copy_kv_token(p + klen + 1, out, out_size);
            return out[0] != '\0';
        }
        p++;
    }
    return false;
}

static void fill_digest_read_kv(const char *response, char *out, int out_size)
{
    static const char *dkeys[] = {
        "DigestRead", "digest_read", "oldSwNameSign", "OldSwNameSign", "old_sw_name_sign"
    };
    char tmp[96];
    size_t ki;
    if (!out || out_size <= 0)
        return;
    out[0] = '\0';
    for (ki = 0; ki < sizeof(dkeys) / sizeof(dkeys[0]); ki++) {
        if (!find_kv_colon(response, dkeys[ki], tmp, (int)sizeof(tmp)))
            continue;
        if (strlen(tmp) != 64)
            continue;
        {
            size_t j;
            for (j = 0; j < 64; j++) {
                if (!isxdigit((unsigned char)tmp[j]))
                    break;
            }
            if (j != 64)
                continue;
        }
        {
            size_t j;
            for (j = 0; j < 64 && (int)j < out_size - 1; j++) {
                char c = tmp[j];
                out[j] = (char)(isupper((unsigned char)c) ? tolower((unsigned char)c) : c);
            }
            out[j] = '\0';
        }
        return;
    }
}

/* ===== 签名材料解析 ===== */

/* Firehose 常见格式：<log value="INFO: Version:0; Platform1:QCOM; Rand:...; DigestWrite:...; ID:..."/> */
static void merge_realme_info_kv(const char *response, edl_realme_sign_material_t *out)
{
    char tmp[512];

    if (out->version[0] == '\0' && find_kv_colon(response, "Version", tmp, sizeof(tmp)))
        snprintf(out->version, sizeof(out->version), "%s", tmp);
    if (out->platform1[0] == '\0' && find_kv_colon(response, "Platform1", tmp, sizeof(tmp)))
        snprintf(out->platform1, sizeof(out->platform1), "%s", tmp);
    if (out->platform2[0] == '\0' && find_kv_colon(response, "Platform2", tmp, sizeof(tmp)))
        snprintf(out->platform2, sizeof(out->platform2), "%s", tmp);
    if (out->mode[0] == '\0' && find_kv_colon(response, "Mode", tmp, sizeof(tmp)))
        snprintf(out->mode, sizeof(out->mode), "%s", tmp);
    if (out->rand[0] == '\0' && find_kv_colon(response, "Rand", tmp, sizeof(tmp)))
        snprintf(out->rand, sizeof(out->rand), "%s", tmp);
    if (out->project_write[0] == '\0' && find_kv_colon(response, "ProjectWrite", tmp, sizeof(tmp)))
        snprintf(out->project_write, sizeof(out->project_write), "%s", tmp);
    if (out->project_read[0] == '\0' && find_kv_colon(response, "ProjectRead", tmp, sizeof(tmp)))
        snprintf(out->project_read, sizeof(out->project_read), "%s", tmp);
    if (out->digest_write[0] == '\0' && find_kv_colon(response, "DigestWrite", tmp, sizeof(tmp)))
        snprintf(out->digest_write, sizeof(out->digest_write), "%s", tmp);
    if (out->secure_boot[0] == '\0' && find_kv_colon(response, "SecureBoot", tmp, sizeof(tmp)))
        snprintf(out->secure_boot, sizeof(out->secure_boot), "%s", tmp);
    if (out->nv_data[0] == '\0' && find_kv_colon(response, "NV", tmp, sizeof(tmp)))
        snprintf(out->nv_data, sizeof(out->nv_data), "%s", tmp);

    if (out->chip_sn[0] == '\0') {
        if (find_kv_colon(response, "ID", tmp, sizeof(tmp)))
            snprintf(out->chip_sn, sizeof(out->chip_sn), "%s", tmp);
        else if (find_kv_colon(response, "ChipSn", tmp, sizeof(tmp)))
            snprintf(out->chip_sn, sizeof(out->chip_sn), "%s", tmp);
        else if (find_kv_colon(response, "chip_sn", tmp, sizeof(tmp)))
            snprintf(out->chip_sn, sizeof(out->chip_sn), "%s", tmp);
    }

    if (!out->nv_check) {
        if (find_kv_colon(response, "NVCheck", tmp, sizeof(tmp)) ||
            find_kv_colon(response, "NvCheck", tmp, sizeof(tmp))) {
            for (char *p = tmp; *p; p++)
                *p = (char)tolower((unsigned char)*p);
            out->nv_check = (strcmp(tmp, "true") == 0 || strcmp(tmp, "1") == 0);
        }
    }
}

bool edl_realme_parse_sign_material(const char *response,
                                     const char *requested_project,
                                     edl_realme_sign_material_t *out)
{
    if (!response || !out) return false;
    memset(out, 0, sizeof(*out));

    if (requested_project)
        snprintf(out->requested_project, sizeof(out->requested_project), "%s", requested_project);

    /* 标准 XML 属性 */
    extract_attr(response, "Version", out->version, sizeof(out->version));
    extract_attr(response, "Platform1", out->platform1, sizeof(out->platform1));
    extract_attr(response, "Platform2", out->platform2, sizeof(out->platform2));
    extract_attr(response, "Mode", out->mode, sizeof(out->mode));
    extract_attr(response, "Rand", out->rand, sizeof(out->rand));
    extract_attr(response, "ProjectWrite", out->project_write, sizeof(out->project_write));
    extract_attr(response, "ProjectRead", out->project_read, sizeof(out->project_read));
    extract_attr(response, "DigestWrite", out->digest_write, sizeof(out->digest_write));
    extract_attr(response, "SecureBoot", out->secure_boot, sizeof(out->secure_boot));

    /* ChipSn: 先尝试 ID 属性 */
    if (!extract_attr(response, "ID", out->chip_sn, sizeof(out->chip_sn)))
        extract_attr(response, "ChipSn", out->chip_sn, sizeof(out->chip_sn));

    /* DigestRead: 尝试多种属性名 */
    if (!extract_attr(response, "DigestRead", out->digest_read, sizeof(out->digest_read)))
        snprintf(out->digest_read, sizeof(out->digest_read), "%s", out->digest_write);

    /* ProjectRead 回退 */
    if (out->project_read[0] == '\0' && out->project_write[0] != '\0')
        snprintf(out->project_read, sizeof(out->project_read), "%s", out->project_write);

    /* NV 数据 */
    extract_attr(response, "NV", out->nv_data, sizeof(out->nv_data));

    /* NvCode: 从 g_nv_id 和 ChipSn 推导 */
    char g_nv_id[256] = {0};
    if (!extract_attr(response, "g_nv_id", g_nv_id, sizeof(g_nv_id)))
        find_kv_colon(response, "g_nv_id", g_nv_id, sizeof(g_nv_id));

    /* <log value="INFO: Key:Val;..."/> 与 C# ExtractResponseValue 对齐 */
    merge_realme_info_kv(response, out);

    if (g_nv_id[0] && out->chip_sn[0]) {
        int gnv_len = (int)strlen(g_nv_id);
        int sn_len = (int)strlen(out->chip_sn);
        if (gnv_len > sn_len) {
            int code_len = gnv_len - sn_len;
            if (code_len > 0 && code_len < (int)sizeof(out->nv_code)) {
                memcpy(out->nv_code, g_nv_id, (size_t)code_len);
                out->nv_code[code_len] = '\0';
            }
        }
    }

    char nv_check[16] = {0};
    if (extract_attr(response, "NVCheck", nv_check, sizeof(nv_check)) ||
        extract_attr(response, "NvCheck", nv_check, sizeof(nv_check)))
        out->nv_check = (strcmp(nv_check, "true") == 0 || strcmp(nv_check, "1") == 0);

    /* DigestRead：64 位 hex 专用键（与 fill_digest_read_kv 一致） */
    if (strlen(out->digest_read) != 64) {
        char dr[96];
        fill_digest_read_kv(response, dr, sizeof(dr));
        if (dr[0])
            snprintf(out->digest_read, sizeof(out->digest_read), "%s", dr);
    }
    if (out->digest_read[0] == '\0' && out->digest_write[0])
        snprintf(out->digest_read, sizeof(out->digest_read), "%s", out->digest_write);

    if (out->project_read[0] == '\0' && out->project_write[0] != '\0')
        snprintf(out->project_read, sizeof(out->project_read), "%s", out->project_write);

    out->is_valid = (out->chip_sn[0] != '\0' && out->rand[0] != '\0' && out->digest_write[0] != '\0');
    return out->is_valid;
}

/* ===== 签名数据处理 ===== */

int edl_realme_normalize_signature(const uint8_t *sig_in, int sig_len,
                                    uint8_t *sig_out)
{
    if (!sig_in || !sig_out || sig_len <= 0) return 0;
    memset(sig_out, 0, 256);
    int copy_len = sig_len > 256 ? 256 : sig_len;
    memcpy(sig_out, sig_in, copy_len);
    return 256;
}

void edl_realme_pad_signature_4096(const uint8_t *sig_256, uint8_t *padded_out)
{
    memset(padded_out, 0, 4096);
    if (sig_256) memcpy(padded_out, sig_256, 256);
}

/* ===== 响应判断 ===== */

edl_realme_protocol_t edl_realme_pick_auth_protocol(uint32_t msm_id, bool have_digest_file)
{
    if (edl_realme_is_modern_platform(msm_id))
        return REALME_PROTO_MODERN;
    if (have_digest_file)
        return REALME_PROTO_LEGACY_DIGEST;
    return REALME_PROTO_LEGACY_SIMPLE;
}

edl_realme_protocol_t edl_realme_detect_protocol(const char *response)
{
    if (!response) return REALME_PROTO_UNKNOWN;
    if (strstr(response, "initdigest"))
        return REALME_PROTO_LEGACY_DIGEST;
    if (strstr(response, "getsigndata")) {
        if (strstr(response, "getstorageinfo"))
            return REALME_PROTO_LEGACY_SIMPLE;
        return REALME_PROTO_MODERN;
    }
    return REALME_PROTO_MODERN;
}

bool edl_realme_is_rawmode(const char *response)
{
    return response && strstr(response, "rawmode=\"true\"") != NULL;
}

bool edl_realme_is_verify_passed(const char *response)
{
    if (!response) return false;
    /* 不区分大小写搜索 "verify passed" */
    const char *p = response;
    while (*p) {
        if ((*p == 'v' || *p == 'V') && strncmp(p+1, "erify ", 6) == 0) {
            if (strncmp(p+7, "passed", 6) == 0 || strncmp(p+7, "Passed", 6) == 0)
                return true;
        }
        p++;
    }
    if (strstr(response, "value=\"ACK\"") && strstr(response, "rawmode=\"false\""))
        return true;
    return false;
}

bool edl_realme_is_ack(const char *response)
{
    return response && strstr(response, "value=\"ACK\"") != NULL;
}

bool edl_realme_is_nak(const char *response)
{
    return response && strstr(response, "value=\"NAK\"") != NULL;
}

/* ===== 串口读写辅助 ===== */

static int port_send_xml(edl_port_t *port, const char *xml)
{
    return edl_port_write(port, (const uint8_t *)xml, (int)strlen(xml));
}

static int port_send_raw(edl_port_t *port, const uint8_t *data, int len)
{
    return edl_port_write(port, data, len);
}

/* 默认：任意 ACK/NAK/rawmode 即视为本条 Firehose 回包结束 */
static bool response_complete_default(const char *buf)
{
    return buf && (strstr(buf, "value=\"ACK\"") != NULL ||
                   strstr(buf, "value=\"NAK\"") != NULL ||
                   strstr(buf, "rawmode=\"false\"") != NULL ||
                   strstr(buf, "rawmode=\"true\"") != NULL);
}

/*
 * getsigndata 与 C# ReadCompositeResponseAsync 一致：不能在出现 value="ACK" 时提前结束，
 * 否则后续 <log value="INFO: Version:...; Rand:...; DigestWrite:..."/> 尚未读入，解析必失败。
 */
static bool response_complete_getsigndata(const char *buf)
{
    return buf && (strstr(buf, "rawmode=\"false\"") != NULL ||
                   strstr(buf, "value=\"NAK\"") != NULL);
}

static int port_read_response_ex(edl_port_t *port, char *buf, int buf_size, int timeout_ms,
                                 bool getsigndata_style, const edl_callbacks_t *cb)
{
    int total = 0;
    int remaining_ms = timeout_ms;
    uint8_t tmp[4096];
    while (total < buf_size - 1 && remaining_ms > 0) {
        if (ra_is_cancelled(cb))
            return EDL_ERR_CANCELLED;

        int chunk = buf_size - total - 1;
        int wait_ms = remaining_ms > REALME_READ_POLL_SLICE_MS
            ? REALME_READ_POLL_SLICE_MS : remaining_ms;
        if (chunk > (int)sizeof(tmp)) chunk = (int)sizeof(tmp);
        int n = edl_port_read(port, tmp, chunk, wait_ms);
        remaining_ms -= wait_ms;
        if (n <= 0)
            continue;
        memcpy(buf + total, tmp, n);
        total += n;
        buf[total] = '\0';
        if (getsigndata_style ? response_complete_getsigndata(buf) : response_complete_default(buf))
            break;
    }
    buf[total] = '\0';
    return total;
}

static int port_read_response(edl_port_t *port, char *buf, int buf_size, int timeout_ms,
                              const edl_callbacks_t *cb)
{
    return port_read_response_ex(port, buf, buf_size, timeout_ms, false, cb);
}

/* ===== Realme 云端签名认证 ===== */

edl_error_t edl_realme_authenticate(edl_port_t *port,
                                     edl_realme_protocol_t protocol,
                                     const char *project_id,
                                     const uint8_t *digest_data, int digest_len,
                                     edl_realme_sign_cb sign_cb, void *cb_data,
                                     const edl_callbacks_t *cb)
{
    if (!port || !sign_cb) return EDL_ERR_INVALID_PARAM;

    char xml[2048];
    char resp[16384];
    edl_realme_sign_material_t material;

    if (ra_is_cancelled(cb)) {
        ra_log(cb, "Realme 认证已取消");
        return EDL_ERR_CANCELLED;
    }

    /* Step 1: Legacy 需要先 initdigest */
    if (protocol == REALME_PROTO_LEGACY_DIGEST && digest_data && digest_len > 0) {
        ra_log(cb, "Legacy: 执行 initdigest (%d 字节)...", digest_len);
        edl_realme_build_initdigest(xml, sizeof(xml), digest_len);
        port_send_xml(port, xml);

        int n = port_read_response(port, resp, sizeof(resp), REALME_INITDIGEST_ACK_TIMEOUT_MS, cb);
        if (n == EDL_ERR_CANCELLED) {
            ra_log(cb, "Realme 认证已取消");
            return EDL_ERR_CANCELLED;
        }
        if (n <= 0 || edl_realme_is_nak(resp)) {
            ra_log(cb, "initdigest 请求被设备拒绝");
            return EDL_ERR_FH_AUTH_REQUIRED;
        }
        if (!edl_realme_is_rawmode(resp)) {
            ra_log(cb, "initdigest 未进入 rawmode");
            return EDL_ERR_FH_AUTH_REQUIRED;
        }

        /* 发送 digest 二进制数据 */
        if (port_send_raw(port, digest_data, digest_len) <= 0) {
            ra_log(cb, "Digest 数据发送失败");
            return EDL_ERR_IO;
        }

        n = port_read_response(port, resp, sizeof(resp), REALME_INITDIGEST_RESULT_TIMEOUT_MS, cb);
        if (n == EDL_ERR_CANCELLED) {
            ra_log(cb, "Realme 认证已取消");
            return EDL_ERR_CANCELLED;
        }
        if (edl_realme_is_nak(resp)) {
            ra_log(cb, "initdigest 被设备拒绝");
            ra_detail(cb, "设备原始响应: %.200s", resp);
            return EDL_ERR_FH_AUTH_REQUIRED;
        }
        ra_log(cb, "initdigest 成功");
    }

    /* Step 2: 发送 getsigndata 获取签名材料 */
    if (protocol == REALME_PROTO_MODERN) {
        if (!project_id || !project_id[0]) {
            ra_log(cb, "Modern 认证需要 ProjectID");
            return EDL_ERR_INVALID_PARAM;
        }
        ra_log(cb, "Modern: getsigndata (ProjectID=%s)...", project_id);
        edl_realme_build_getsigndata(xml, sizeof(xml), project_id);
    } else {
        ra_log(cb, "Legacy: getsigndata...");
        edl_realme_build_getsigndata_legacy(xml, sizeof(xml));
    }

    port_send_xml(port, xml);
    int n = port_read_response_ex(port, resp, sizeof(resp), REALME_GETSIGDATA_TIMEOUT_MS, true, cb);

    if (n == EDL_ERR_CANCELLED) {
        ra_log(cb, "Realme 认证已取消");
        return EDL_ERR_CANCELLED;
    }

    if (n <= 0) {
        ra_log(cb, "getsigndata 无响应");
        return EDL_ERR_SAHARA_NO_RESPONSE;
    }
    if (edl_realme_is_nak(resp)) {
        ra_log(cb, "getsigndata 被设备拒绝");
        ra_detail(cb, "设备原始响应: %.200s", resp);
        return EDL_ERR_FH_NAK;
    }

    /* 解析签名材料 */
    if (!edl_realme_parse_sign_material(resp, project_id, &material)) {
        ra_log(cb, "设备返回的签名材料不完整");
        ra_detail(cb, "getsigndata 原始响应: %.400s", resp);
        return EDL_ERR_FH_AUTH_REQUIRED;
    }

    ra_log(cb, "签名材料: ChipSn=%s, Mode=%s, NvCheck=%s",
           material.chip_sn, material.mode, material.nv_check ? "true" : "false");
    ra_detail(cb, "Rand=%s, Digest=%.16s...",
              material.rand, material.digest_write);

    /* Step 3: 调用云端签名回调 */
    ra_log(cb, "请求云端签名...");
    uint8_t signature_raw[4096];
    int sig_raw_len = 0;

    if (!sign_cb(&material, signature_raw, &sig_raw_len, cb_data) || sig_raw_len <= 0) {
        if (ra_is_cancelled(cb)) {
            ra_log(cb, "云端签名已取消");
            return EDL_ERR_CANCELLED;
        }
        ra_log(cb, "云端签名失败");
        return EDL_ERR_FH_AUTH_REQUIRED;
    }
    ra_log(cb, "已获取签名 (%d 字节)", sig_raw_len);

    /* Step 4: 标准化签名 → 256 字节 → 填充到 4096 字节 */
    uint8_t sig_normalized[256];
    edl_realme_normalize_signature(signature_raw, sig_raw_len, sig_normalized);

    uint8_t sig_padded[4096];
    edl_realme_pad_signature_4096(sig_normalized, sig_padded);

    /* Step 5: 发送 verify 命令 */
    if (protocol == REALME_PROTO_MODERN) {
        ra_detail(cb, "发送 verify (EnableVip=0)...");
        edl_realme_build_verify_modern(xml, sizeof(xml));
    } else {
        ra_detail(cb, "发送 verify (legacy)...");
        edl_realme_build_verify_legacy(xml, sizeof(xml));
    }

    port_send_xml(port, xml);
    n = port_read_response(port, resp, sizeof(resp), REALME_VERIFY_RAWMODE_TIMEOUT_MS, cb);
    if (n == EDL_ERR_CANCELLED) {
        ra_log(cb, "Realme 认证已取消");
        return EDL_ERR_CANCELLED;
    }

    if (edl_realme_is_nak(resp)) {
        ra_log(cb, "verify 请求被设备拒绝");
        ra_detail(cb, "设备原始响应: %.200s", resp);
        return EDL_ERR_FH_NAK;
    }
    if (!edl_realme_is_rawmode(resp)) {
        ra_log(cb, "verify 未进入 rawmode");
        return EDL_ERR_FH_AUTH_REQUIRED;
    }

    /* Step 6: 下发签名数据 (4096 字节, rawmode 扇区对齐) */
    ra_detail(cb, "下发签名数据 (256 -> 4096 字节, rawmode 填充)...");
    if (port_send_raw(port, sig_padded, 4096) <= 0) {
        ra_log(cb, "签名数据发送失败");
        return EDL_ERR_IO;
    }

    n = port_read_response(port, resp, sizeof(resp), REALME_SIGNATURE_RESULT_TIMEOUT_MS, cb);
    if (n == EDL_ERR_CANCELLED) {
        ra_log(cb, "Realme 认证已取消");
        return EDL_ERR_CANCELLED;
    }
    if (n <= 0) {
        ra_log(cb, "签名下发后无回执");
        return EDL_ERR_SAHARA_NO_RESPONSE;
    }

    if (edl_realme_is_nak(resp)) {
        ra_log(cb, "设备侧 verify 失败");
        ra_detail(cb, "设备原始响应: %.200s", resp);
        return EDL_ERR_FH_AUTH_REQUIRED;
    }

    if (edl_realme_is_verify_passed(resp)) {
        ra_log(cb, "设备侧 verify 已通过，认证成功");
        return EDL_OK;
    }

    if (edl_realme_is_ack(resp)) {
        ra_log(cb, "设备返回 ACK，认证成功");
        return EDL_OK;
    }

    ra_log(cb, "未收到预期的 verify 成功回执");
    ra_detail(cb, "设备原始响应: %.200s", resp);
    return EDL_ERR_FH_AUTH_REQUIRED;
}

/* ===== VIP 认证 (Digest + Signature 文件) ===== */

edl_error_t edl_realme_vip_authenticate(edl_port_t *port,
                                         const uint8_t *digest_data, int digest_len,
                                         const uint8_t *signature_data, int sig_len,
                                         const edl_callbacks_t *cb)
{
    if (!port || !digest_data || digest_len <= 0 || !signature_data || sig_len <= 0)
        return EDL_ERR_INVALID_PARAM;

    char xml[2048];
    char resp[8192];

    ra_log(cb, "正在执行安全验证...");
    edl_port_purge(port);

    if (ra_is_cancelled(cb)) {
        ra_log(cb, "VIP 安全验证已取消");
        return EDL_ERR_CANCELLED;
    }

    /* Step 1: 发送 Digest 二进制 */
    ra_detail(cb, "Step 1/5: 发送 Digest (%d 字节)", digest_len);
    if (port_send_raw(port, digest_data, digest_len) <= 0) return EDL_ERR_IO;
    if (port_read_response(port, resp, sizeof(resp), REALME_INITDIGEST_ACK_TIMEOUT_MS, cb) == EDL_ERR_CANCELLED) {
        ra_log(cb, "VIP 安全验证已取消");
        return EDL_ERR_CANCELLED;
    }
    ra_detail(cb, "Step 1 响应: %.100s", resp);

    /* Step 2: TransferCfg */
    ra_detail(cb, "Step 2/5: TransferCfg");
    edl_realme_build_transfercfg(xml, sizeof(xml));
    port_send_xml(port, xml);
    if (port_read_response(port, resp, sizeof(resp), REALME_VIP_TRANSFERCFG_TIMEOUT_MS, cb) == EDL_ERR_CANCELLED) {
        ra_log(cb, "VIP 安全验证已取消");
        return EDL_ERR_CANCELLED;
    }
    ra_detail(cb, "Step 2 响应: %.100s", resp);
    if (edl_realme_is_nak(resp)) {
        ra_log(cb, "VIP: TransferCfg 阶段设备返回 NAK");
        return EDL_ERR_FH_AUTH_REQUIRED;
    }

    /* Step 3: Verify (EnableVip=1) */
    ra_detail(cb, "Step 3/5: Verify (EnableVip=1)");
    edl_realme_build_verify_vip(xml, sizeof(xml));
    port_send_xml(port, xml);
    int n = port_read_response(port, resp, sizeof(resp), REALME_VIP_VERIFY_TIMEOUT_MS, cb);
    if (n == EDL_ERR_CANCELLED) {
        ra_log(cb, "VIP 安全验证已取消");
        return EDL_ERR_CANCELLED;
    }
    ra_detail(cb, "Step 3 响应: %.100s", resp);
    bool is_raw = edl_realme_is_rawmode(resp);

    /* Step 4: 发送 Signature 二进制 */
    uint8_t sig_norm[256];
    edl_realme_normalize_signature(signature_data, sig_len, sig_norm);
    int send_size;
    uint8_t sig_send[4096];
    if (is_raw) {
        edl_realme_pad_signature_4096(sig_norm, sig_send);
        send_size = 4096;
        ra_detail(cb, "Step 4/5: Signature (%d -> 4096 字节, rawmode 填充)", sig_len);
    } else {
        memcpy(sig_send, sig_norm, 256);
        send_size = 256;
        ra_detail(cb, "Step 4/5: Signature (%d -> 256 字节)", sig_len);
    }
    if (port_send_raw(port, sig_send, send_size) <= 0) return EDL_ERR_IO;
    n = port_read_response(port, resp, sizeof(resp), REALME_VIP_SIGNATURE_TIMEOUT_MS, cb);
    if (n == EDL_ERR_CANCELLED) {
        ra_log(cb, "VIP 安全验证已取消");
        return EDL_ERR_CANCELLED;
    }
    ra_detail(cb, "Step 4 响应: %.100s", resp);

    if (edl_realme_is_nak(resp)) {
        ra_log(cb, "VIP: Signature 被设备拒绝 (NAK)");
        ra_detail(cb, "设备原始响应: %.200s", resp);
        return EDL_ERR_FH_AUTH_REQUIRED;
    }

    /* Step 5: SHA256Init */
    ra_detail(cb, "Step 5/5: SHA256Init");
    edl_realme_build_sha256init(xml, sizeof(xml));
    port_send_xml(port, xml);
    n = port_read_response(port, resp, sizeof(resp), REALME_VIP_SHA256INIT_TIMEOUT_MS, cb);
    if (n == EDL_ERR_CANCELLED) {
        ra_log(cb, "VIP 安全验证已取消");
        return EDL_ERR_CANCELLED;
    }
    (void)n;
    ra_detail(cb, "Step 5 响应: %.100s", resp);
    if (edl_realme_is_nak(resp)) {
        ra_log(cb, "VIP: SHA256Init 阶段设备返回 NAK");
        return EDL_ERR_FH_AUTH_REQUIRED;
    }

    ra_log(cb, "VIP 安全验证完成");
    return EDL_OK;
}
