#include "edl/firehose.h"
#include "edl/xml_helper.h"
#include "edl/gpt.h"
#include "edl/sparse.h"
#include "edl/storage_report.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>

#ifndef _WIN32
#include <strings.h>
#endif

#ifdef _WIN32
#include <windows.h>
#define edl_sleep_ms(ms) Sleep(ms)
#else
#include <time.h>
#include <unistd.h>
#define edl_sleep_ms(ms) usleep((ms) * 1000)
#endif

/* ===== Constants ===== */
#define FH_ACK_TIMEOUT_MS        15000
#define FH_DEFAULT_PAYLOAD       (1024 * 1024)
#define FH_OPTIMAL_PAYLOAD       (1024 * 1024)
#define FH_FALLBACK_PAYLOAD_1    (512 * 1024)
#define FH_FALLBACK_PAYLOAD_2    (256 * 1024)
#define FH_FALLBACK_PAYLOAD_3    (128 * 1024)
#define FH_IDLE_POLL_MS          1
#define FH_XML_BUF_SIZE          2048
#define FH_RX_BUF_SIZE           65536
#define FH_TAIL_CACHE_MAX        4096
#define FH_SMALL_READ_TIMEOUT_MS 5000
#define FH_GPT_READ_TIMEOUT_MS   3200
#define FH_GPT_TAIL_ACK_MS       120
#define FH_SMALL_TAIL_ACK_MS     180
#define FH_READ_TAIL_ACK_MS      260
#define FH_GPT_LUN_GAP_MS        2
#define FH_GPT_RETRY_SETTLE_MS   40
#define FH_GPT_PROGRESS_UNIT     (1024 * 1024)
#define FH_GPT_PROGRESS_STEPS    4
#define FH_FILE_BUF_SIZE         (4 * 1024 * 1024)
/* getddrtype：读 GPT 后链路可能仍忙，等待需长于普通 ACK；部分旧 Programmer 不支持则一直无应答 */
#define FH_GETDDR_TIMEOUT_MS     25000
#define FH_GETDDR_RETRY_MS       20000

/* ===== Internal State ===== */
struct edl_firehose {
    edl_port_t      *port;
    edl_callbacks_t  cb;

    char  storage_type[16];
    int   sector_size;
    int   max_payload_size;
    /* 从 Configure/GetStorageInfo 等 XML 日志中解析（与 Sahara MSM HWID 含义一致），供 Realme 等认证 */
    uint32_t msm_hwid_hint;
    int      reported_lun_count;
    uint32_t reported_lun_enable_mask;
    /* 写入策略：默认与旧版一致（补满 GPT）；program XML 默认带 read_back_verify（对齐 SakuraEDL） */
    bool  pad_short_image_to_gpt;
    bool  program_read_back_verify;
    bool  progress_remap_active;
    int64_t progress_remap_base;
    int64_t progress_remap_span;
    int64_t progress_remap_total;
};

/* ===== Helpers ===== */

static edl_error_t fh_wait_response(edl_firehose_t *ctx, edl_xml_response_t *resp, int timeout_ms);
static edl_error_t fh_wait_response_capture(edl_firehose_t *ctx, edl_xml_response_t *resp,
                                            char *rx_snapshot, size_t rx_cap,
                                            char *log_concat, size_t log_cap,
                                            int timeout_ms);
static void fh_extract_msm_hwid_from_rx(edl_firehose_t *ctx, const char *rx_buf);
static void fh_update_storage_info_hints(edl_firehose_t *ctx, const char *rx_buf);
static void fh_update_storage_info_hints_shared(edl_firehose_t *ctx, const char *rx_buf);

static void fh_log(edl_firehose_t *ctx, const char *fmt, ...)
{
    if (!ctx->cb.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->cb.log(buf, ctx->cb.user_data);
}

static void fh_log_detail(edl_firehose_t *ctx, const char *fmt, ...)
{
    /* 仅当上层显式设置 log_detail 时输出；勿回退到 log，避免调试信息刷屏主日志 */
    if (!ctx->cb.log_detail) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->cb.log_detail(buf, ctx->cb.user_data);
}

static bool fh_is_cancelled(edl_firehose_t *ctx)
{
    return ctx->cb.is_cancelled && ctx->cb.is_cancelled(ctx->cb.user_data);
}

static bool fh_sleep_cancelable(edl_firehose_t *ctx, int total_ms)
{
    while (total_ms > 0) {
        if (fh_is_cancelled(ctx))
            return true;
        int slice = total_ms > 20 ? 20 : total_ms;
        edl_sleep_ms(slice);
        total_ms -= slice;
    }
    return false;
}

static uint64_t fh_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void fh_log_elapsed_detail(edl_firehose_t *ctx, const char *stage, edl_error_t err, uint64_t start_ms)
{
    if (!ctx || !stage || start_ms == 0)
        return;

    const uint64_t elapsed = fh_now_ms() - start_ms;
    if (err == EDL_OK) {
        fh_log_detail(ctx, "【耗时】%s：%llu ms", stage, (unsigned long long)elapsed);
    } else if (err == EDL_ERR_CANCELLED) {
        fh_log_detail(ctx, "【耗时】%s：%llu ms（已取消）", stage, (unsigned long long)elapsed);
    } else {
        fh_log_detail(ctx, "【耗时】%s：%llu ms（%s）",
                      stage, (unsigned long long)elapsed, edl_error_str(err));
    }
}

static edl_error_t fh_wait_idle_backoff(edl_firehose_t *ctx, uint64_t start_ms, int *idle_rounds)
{
    const uint64_t elapsed = fh_now_ms() - start_ms;
    const int rounds = idle_rounds ? *idle_rounds : 0;

#ifdef _WIN32
    if (elapsed < 20 && rounds < 8) {
        if (idle_rounds)
            *idle_rounds = rounds + 1;
        Sleep(0);
        return EDL_OK;
    }
#endif

    if (idle_rounds)
        *idle_rounds = rounds + 1;

    if (elapsed < 1000)
        return fh_sleep_cancelable(ctx, 1) ? EDL_ERR_CANCELLED : EDL_OK;
    return fh_sleep_cancelable(ctx, 5) ? EDL_ERR_CANCELLED : EDL_OK;
}

static void fh_trim_rx_prefix(char *rx_buf, int *rx_pos, const char *keep_from)
{
    int keep_offset = 0;
    int remain = 0;

    if (!rx_buf || !rx_pos || !keep_from || *rx_pos <= 0)
        return;

    keep_offset = (int)(keep_from - rx_buf);
    if (keep_offset <= 0 || keep_offset > *rx_pos)
        return;

    remain = *rx_pos - keep_offset;
    if (remain > 0)
        memmove(rx_buf, keep_from, (size_t)remain);
    *rx_pos = remain;
    rx_buf[remain] = '\0';
}

static void fh_compact_rx_window(char *rx_buf, int *rx_pos)
{
    int keep = 0;

    if (!rx_buf || !rx_pos || *rx_pos <= 0)
        return;

    keep = *rx_pos;
    if (keep > FH_TAIL_CACHE_MAX)
        keep = FH_TAIL_CACHE_MAX;
    if (keep <= 0 || keep >= *rx_pos)
        return;

    memmove(rx_buf, rx_buf + (*rx_pos - keep), (size_t)keep);
    *rx_pos = keep;
    rx_buf[keep] = '\0';
}

static void fh_cache_tail_bytes(edl_firehose_t *ctx, const uint8_t *tail, int tail_len)
{
    int cache_len = 0;

    if (!ctx || !tail || tail_len <= 0)
        return;

    cache_len = tail_len;
    if (cache_len > FH_TAIL_CACHE_MAX) {
        fh_log_detail(ctx, "Firehose 尾部残留 %d 字节过大，仅缓存前 %d 字节", tail_len, FH_TAIL_CACHE_MAX);
        cache_len = FH_TAIL_CACHE_MAX;
    }

    if (edl_port_push_rx(ctx->port, tail, cache_len) != 0)
        fh_log_detail(ctx, "Firehose 尾部残留 %d 字节缓存失败", cache_len);
}

static void fh_cache_xml_tail(edl_firehose_t *ctx, const char *resp_end, int rx_pos, const char *rx_buf)
{
    int tail_len = 0;

    if (!ctx || !resp_end || !rx_buf || rx_pos <= 0)
        return;

    tail_len = rx_pos - (int)(resp_end - rx_buf);
    if (tail_len <= 0)
        return;

    fh_cache_tail_bytes(ctx, (const uint8_t *)resp_end, tail_len);
}

static int fh_is_false_rawmode_or_plain_ack(const edl_xml_response_t *resp)
{
    const char *rawmode = NULL;

    if (!resp || !resp->is_ack)
        return 0;

    rawmode = edl_xml_get_attr(resp, "rawmode");
    if (!rawmode || !rawmode[0])
        return 1;

#ifdef _WIN32
    return _stricmp(rawmode, "false") == 0;
#else
    return strcasecmp(rawmode, "false") == 0;
#endif
}

static int fh_handle_read_tail_bytes(edl_firehose_t *ctx, const uint8_t *tail, int tail_len)
{
    char xml_tail[FH_TAIL_CACHE_MAX + 1];
    const char *resp_start = NULL;
    const char *resp_end = NULL;
    int scan_len = 0;

    if (!ctx || !tail || tail_len <= 0)
        return 0;

    scan_len = tail_len;
    if (scan_len > FH_TAIL_CACHE_MAX)
        scan_len = FH_TAIL_CACHE_MAX;

    memcpy(xml_tail, tail, (size_t)scan_len);
    xml_tail[scan_len] = '\0';

    resp_start = edl_xml_find_element(xml_tail, "response", &resp_end);
    if (resp_start && resp_end && resp_end > resp_start) {
        edl_xml_response_t resp;
        char element[1024];
        int elem_len = (int)(resp_end - resp_start);
        if (elem_len > 0 && elem_len < (int)sizeof(element)) {
            int consumed = 0;
            memcpy(element, resp_start, (size_t)elem_len);
            element[elem_len] = '\0';
            if (edl_xml_parse_response(element, &resp) == 0 &&
                fh_is_false_rawmode_or_plain_ack(&resp)) {
                consumed = (int)(resp_end - xml_tail);
                if (tail_len > consumed)
                    fh_cache_tail_bytes(ctx, tail + consumed, tail_len - consumed);
                return 1;
            }
        }
    }

    fh_cache_tail_bytes(ctx, tail, tail_len);
    return 0;
}

static void fh_send_xml(edl_firehose_t *ctx, const char *xml)
{
    edl_port_write(ctx->port, (const uint8_t *)xml, (int)strlen(xml));
}

static void fh_discard_rx(edl_firehose_t *ctx)
{
    edl_port_discard_rx(ctx->port);
}

/* start_sector + num_sectors 不溢出 int64（不校验介质容量，仅避免未定义行为） */
static int fh_sector_range_valid(int64_t start, int64_t num_sec)
{
    if (num_sec < 0 || start < 0)
        return 0;
    if (num_sec == 0)
        return 1;
    if (num_sec > INT64_MAX - start)
        return 0;
    return 1;
}

/*
 * 分区表中的扇区大小必须与 Configure 后的 Firehose 扇区大小一致，否则：
 * - file_sector_offset / num_sectors 对应的字节数会错位；
 * - sparse 展开与 program 步进与设备真实布局不一致，可导致无法开机。
 * sector_size==0 表示调用方未填，跳过（由 Firehose 单方面决定）。
 */
static edl_error_t fh_require_part_sector_matches_session(edl_firehose_t *ctx,
                                                          const edl_partition_info_t *part)
{
    if (!ctx || !part)
        return EDL_ERR_INVALID_PARAM;
    if (part->sector_size <= 0)
        return EDL_OK;
    if (part->sector_size != ctx->sector_size) {
        fh_log(ctx,
               "中止写入 %s：分区表扇区 %d B 与当前会话 %d B 不一致。"
               "请重新「读取分区表」或核对存储类型(ufs/emmc)，否则极易写错位导致无法开机。",
               part->name, part->sector_size, ctx->sector_size);
        return EDL_ERR_INVALID_PARAM;
    }
    return EDL_OK;
}

static int fh_is_primary_or_backup_gpt_name(const char *name)
{
    if (!name || !name[0]) return 0;
#ifdef _WIN32
    return (_stricmp(name, "PrimaryGPT") == 0 || _stricmp(name, "BackupGPT") == 0);
#else
    return (strcasecmp(name, "PrimaryGPT") == 0 || strcasecmp(name, "BackupGPT") == 0);
#endif
}

static int fh_is_primary_gpt_name(const char *name)
{
    if (!name || !name[0]) return 0;
#ifdef _WIN32
    return (_stricmp(name, "PrimaryGPT") == 0);
#else
    return (strcasecmp(name, "PrimaryGPT") == 0);
#endif
}

static int fh_is_backup_gpt_name(const char *name)
{
    if (!name || !name[0]) return 0;
#ifdef _WIN32
    return (_stricmp(name, "BackupGPT") == 0);
#else
    return (strcasecmp(name, "BackupGPT") == 0);
#endif
}

/* 与 Bus Hound 抓包一致：PrimaryGPT=6/34，BackupGPT=5/33 */
static void fh_apply_gpt_capture_num_sectors(edl_firehose_t *ctx, edl_partition_info_t *pio)
{
    if (!ctx || !pio || !fh_is_primary_or_backup_gpt_name(pio->name))
        return;
    int std = fh_is_backup_gpt_name(pio->name)
        ? edl_gpt_backup_region_sectors(ctx->sector_size)
        : edl_gpt_firehose_gpt_region_sectors(ctx->sector_size);
    if (pio->num_sectors != std) {
        fh_log_detail(ctx, "%s: 按抓包标准使用 num_partition_sectors=%d（原 %lld）",
                      pio->name, std, (long long)pio->num_sectors);
        pio->num_sectors = std;
    }
}

/*
 * SakuraEDL WriteSectorsAsync：program + 发送前 DiscardInBuffer。
 * PrimaryGPT/BackupGPT：与 Bus Hound 官方抓包一致用 edl_xml_build_program_raw（无 label），
 * 避免部分 loader 对 label 路径处理与纯 raw program 不一致导致 GPT 写入异常。
 */
static edl_error_t fh_write_sectors_program(edl_firehose_t *ctx, int lun, int64_t start_sector,
                                            const char *start_sector_expr,
                                            const uint8_t *data, int padded_len,
                                            const char *label)
{
    if (!ctx || !data || padded_len <= 0)
        return EDL_ERR_INVALID_PARAM;
    if ((padded_len % ctx->sector_size) != 0)
        return EDL_ERR_INVALID_PARAM;

    int64_t num_sectors = (int64_t)padded_len / ctx->sector_size;

    char xml[FH_XML_BUF_SIZE];
    if (label && fh_is_primary_or_backup_gpt_name(label)) {
        if (start_sector_expr && start_sector_expr[0]) {
            edl_xml_build_program_raw_expr(xml, sizeof(xml), ctx->sector_size, lun,
                                           start_sector_expr, num_sectors,
                                           ctx->program_read_back_verify);
            fh_log_detail(ctx, "program(raw): %s LUN%d start=%s sectors=%lld",
                          label, lun, start_sector_expr, (long long)num_sectors);
        } else {
            edl_xml_build_program_raw(xml, sizeof(xml), ctx->sector_size, lun,
                                      start_sector, num_sectors,
                                      ctx->program_read_back_verify);
            fh_log_detail(ctx, "program(raw): %s LUN%d start=%lld sectors=%lld",
                          label, lun, (long long)start_sector, (long long)num_sectors);
        }
    } else {
        edl_xml_build_program_label(xml, sizeof(xml), ctx->sector_size, lun,
                                    start_sector, num_sectors, label ? label : "Partition",
                                    ctx->program_read_back_verify,
                                    true);
    }

    fh_discard_rx(ctx);
    fh_send_xml(ctx, xml);

    edl_xml_response_t resp;
    edl_error_t wait_err = fh_wait_response(ctx, &resp, FH_ACK_TIMEOUT_MS);
    if (wait_err == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (wait_err != EDL_OK || !resp.is_ack)
        return EDL_ERR_FH_WRITE;

    edl_port_write(ctx->port, data, padded_len);

    wait_err = fh_wait_response(ctx, &resp, FH_ACK_TIMEOUT_MS);
    if (wait_err == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (wait_err != EDL_OK || !resp.is_ack)
        return EDL_ERR_FH_WRITE;
    return EDL_OK;
}

#ifdef _WIN32
#define fh_strnicmp(a, b, n) _strnicmp((a), (b), (int)(n))
#else
#define fh_strnicmp(a, b, n) strncasecmp((a), (b), (n))
#endif

static int fh_hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t fh_msm_from_hwid64(uint64_t v)
{
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)(v & 0xFFFFFFFFu);
    if (hi != 0) return hi;
    return lo;
}

static int fh_try_parse_hex_at(const char *s, uint64_t *out)
{
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return -1;
    const char *p = s + 2;
    int digits = 0;
    uint64_t v = 0;
    while (digits < 16) {
        int d = fh_hex_digit((unsigned char)*p);
        if (d < 0) break;
        v = (v << 4) | (uint64_t)d;
        p++; digits++;
    }
    if (digits < 8) return -1;
    *out = v;
    return 0;
}

static void fh_extract_msm_hwid_from_rx(edl_firehose_t *ctx, const char *rx_buf)
{
    if (!ctx || !rx_buf || ctx->msm_hwid_hint != 0) return;

    for (const char *p = rx_buf; *p;) {
        const char *hit = NULL;
        if (fh_strnicmp(p, "MSMHWID", 7) == 0) hit = p;
        else if (fh_strnicmp(p, "msm_hwid", 8) == 0) hit = p;
        else if (fh_strnicmp(p, "msm_hw_id", 9) == 0) hit = p;
        else {
            p++;
            continue;
        }

        const char *end = hit + 220;
        for (const char *q = hit; q < end && *q; q++) {
            if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
                uint64_t v = 0;
                if (fh_try_parse_hex_at(q, &v) == 0) {
                    uint32_t msm = fh_msm_from_hwid64(v);
                    if (msm != 0) {
                        ctx->msm_hwid_hint = msm;
                        return;
                    }
                }
            }
        }
        p = hit + 1;
    }

    for (const char *p = rx_buf; *p; p++) {
        if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) continue;
        const char *dig = p + 2;
        int nd = 0;
        while (nd < 16 && fh_hex_digit((unsigned char)dig[nd]) >= 0) nd++;
        if (nd != 16) continue;

        uint64_t v = 0;
        if (fh_try_parse_hex_at(p, &v) != 0) continue;
        uint32_t msm = fh_msm_from_hwid64(v);
        if (msm == 0) continue;

        const char *win_start = p - 140;
        if (win_start < rx_buf) win_start = rx_buf;
        char chunk[200];
        size_t wl = (size_t)(p - win_start);
        if (wl >= sizeof(chunk)) wl = sizeof(chunk) - 1;
        memcpy(chunk, win_start, wl);
        chunk[wl] = '\0';
        if (strstr(chunk, "MSM") || strstr(chunk, "HWID") || strstr(chunk, "hwid") ||
            strstr(chunk, "Chip") || strstr(chunk, "chip") || strstr(chunk, "Soc") ||
            strstr(chunk, "soc")) {
            ctx->msm_hwid_hint = msm;
            return;
        }
    }
}

static const char *fh_stristr(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) return NULL;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (fh_strnicmp(p, needle, nlen) == 0)
            return p;
    }
    return NULL;
}

static int fh_parse_u32_decimal_after(const char *s, uint32_t *out)
{
    if (!s || !out) return 0;
    while (*s == ' ' || *s == '\t' || *s == ':' || *s == '=' || *s == '"' || *s == '\'')
        s++;
    if (!isdigit((unsigned char)*s))
        return 0;

    unsigned long v = strtoul(s, NULL, 10);
    if (v == 0 || v > 64)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static int fh_parse_u32_hex_near(const char *s, uint32_t *out)
{
    if (!s || !out) return 0;
    const char *p = fh_stristr(s, "0x");
    if (!p)
        return 0;

    uint64_t v = 0;
    if (fh_try_parse_hex_at(p, &v) != 0 || v == 0 || v > 0xFFFFFFFFu)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static int fh_lun_span_from_mask(uint32_t mask)
{
    int span = 0;
    while (mask != 0) {
        span++;
        mask >>= 1;
    }
    return span;
}

static int fh_lun_count_from_mask(uint32_t mask)
{
    int count = 0;
    while (mask != 0) {
        count += (mask & 1u) ? 1 : 0;
        mask >>= 1;
    }
    return count;
}

static uint32_t fh_parse_lun_list_mask(const char *s)
{
    if (!s || !s[0])
        return 0;

    uint32_t mask = 0;
    const char *p = s;
    int budget = 256;
    while (*p && budget-- > 0) {
        const char *tag = fh_stristr(p, "LUN");
        if (!tag)
            break;
        tag += 3;

        char *end = NULL;
        unsigned long lun = strtoul(tag, &end, 10);
        if (end && end != tag && lun < 32)
            mask |= (1u << (unsigned)lun);

        p = (end && end > tag) ? end : tag;
    }
    return mask;
}

static bool fh_is_gpt_read_request(int64_t start_sector, int num_sectors, const char *label)
{
    if (start_sector == 0 && num_sectors > 0 && num_sectors <= 4096)
        return true;
    return label && fh_stristr(label, "gpt");
}

static int fh_pick_read_timeout_ms(int64_t start_sector, int num_sectors,
                                   int total_bytes, const char *label)
{
    if (fh_is_gpt_read_request(start_sector, num_sectors, label))
        return FH_GPT_READ_TIMEOUT_MS;
    if (total_bytes < 256 * 1024)
        return FH_SMALL_READ_TIMEOUT_MS;
    return FH_ACK_TIMEOUT_MS;
}

static int fh_pick_tail_ack_timeout_ms(int64_t start_sector, int num_sectors,
                                       int total_bytes, const char *label)
{
    if (fh_is_gpt_read_request(start_sector, num_sectors, label))
        return FH_GPT_TAIL_ACK_MS;
    if (total_bytes < 256 * 1024)
        return FH_SMALL_TAIL_ACK_MS;
    return FH_READ_TAIL_ACK_MS;
}

static int fh_build_lun_scan_list(const edl_firehose_t *ctx, int max_lun,
                                  int *luns, int cap)
{
    if (!ctx || !luns || cap <= 0 || max_lun <= 0)
        return 0;

    int total = 0;
    if (ctx->reported_lun_enable_mask != 0) {
        for (int lun = 0; lun < 32 && lun < max_lun && total < cap; lun++) {
            if (ctx->reported_lun_enable_mask & (1u << (unsigned)lun))
                luns[total++] = lun;
        }
        if (total > 0)
            return total;
    }

    int limit = max_lun;
    if (ctx->reported_lun_count > 0 && ctx->reported_lun_count < limit)
        limit = ctx->reported_lun_count;
    if (limit > cap)
        limit = cap;

    for (int lun = 0; lun < limit; lun++)
        luns[total++] = lun;
    return total;
}

static void fh_report_gpt_scan_progress_units(edl_firehose_t *ctx,
                                              int64_t done_units,
                                              int64_t total_units)
{
    if (!ctx || total_units <= 0)
        return;

    int64_t total = total_units * (int64_t)FH_GPT_PROGRESS_UNIT;
    int64_t current = done_units * (int64_t)FH_GPT_PROGRESS_UNIT;
    if (current < 0)
        current = 0;
    if (current > total)
        current = total;
    if (ctx->cb.progress)
        ctx->cb.progress(current, total, ctx->cb.user_data);
}

static void fh_progress_remap_reset(edl_firehose_t *ctx)
{
    if (!ctx)
        return;
    ctx->progress_remap_active = false;
    ctx->progress_remap_base = 0;
    ctx->progress_remap_span = 0;
    ctx->progress_remap_total = 0;
}

static void fh_progress_remap_begin(edl_firehose_t *ctx,
                                    int64_t base,
                                    int64_t span,
                                    int64_t total)
{
    if (!ctx || total <= 0 || span <= 0) {
        fh_progress_remap_reset(ctx);
        return;
    }
    if (base < 0)
        base = 0;
    if (base > total)
        base = total;
    ctx->progress_remap_active = true;
    ctx->progress_remap_base = base;
    ctx->progress_remap_span = span;
    ctx->progress_remap_total = total;
}

static void fh_report_progress(edl_firehose_t *ctx, int64_t current, int64_t total)
{
    if (!ctx || !ctx->cb.progress || total <= 0)
        return;

    int64_t bounded_current = current;
    if (bounded_current < 0)
        bounded_current = 0;
    if (bounded_current > total)
        bounded_current = total;

    if (ctx->progress_remap_active
        && ctx->progress_remap_total > 0
        && ctx->progress_remap_span > 0) {
        const long double ratio =
            (total > 0)
                ? ((long double)bounded_current / (long double)total)
                : 0.0L;
        int64_t mapped = ctx->progress_remap_base
            + (int64_t)(ratio * (long double)ctx->progress_remap_span);
        const int64_t mapped_end = ctx->progress_remap_base + ctx->progress_remap_span;
        if (mapped < ctx->progress_remap_base)
            mapped = ctx->progress_remap_base;
        if (mapped > mapped_end)
            mapped = mapped_end;
        if (mapped > ctx->progress_remap_total)
            mapped = ctx->progress_remap_total;
        ctx->cb.progress(mapped, ctx->progress_remap_total, ctx->cb.user_data);
        return;
    }

    ctx->cb.progress(bounded_current, total, ctx->cb.user_data);
}

static void fh_update_storage_info_hints(edl_firehose_t *ctx, const char *rx_buf)
{
    if (!ctx || !rx_buf || !rx_buf[0])
        return;

    uint32_t value = 0;
    const char *p = strstr(rx_buf, "\"num_physical\":");
    if (p && fh_parse_u32_decimal_after(p + strlen("\"num_physical\":"), &value))
        ctx->reported_lun_count = (int)value;

    p = fh_stristr(rx_buf, "Physical Partition Count");
    if (!p)
        p = fh_stristr(rx_buf, "Number of LU");
    if (!p)
        p = fh_stristr(rx_buf, "number of lun");
    if (p) {
        const char *q = strchr(p, ':');
        if (!q)
            q = p;
        if (fh_parse_u32_decimal_after(q, &value))
            ctx->reported_lun_count = (int)value;
    }

    p = fh_stristr(rx_buf, "LUN Enable Bitmask");
    if (!p)
        p = fh_stristr(rx_buf, "Total Active LU");
    if (!p)
        p = fh_stristr(rx_buf, "UFS Total Active LU");
    if (p) {
        if (fh_parse_u32_hex_near(p, &value)) {
            ctx->reported_lun_enable_mask = value;
        } else {
            const char *q = strchr(p, ':');
            if (!q)
                q = p;
            if (fh_parse_u32_decimal_after(q, &value)) {
                ctx->reported_lun_count = (int)value;
                if (value < 32u)
                    ctx->reported_lun_enable_mask = (1u << value) - 1u;
            }
        }
    }

    if (ctx->reported_lun_enable_mask == 0) {
        p = fh_stristr(rx_buf, "Enabled LUN");
        if (!p)
            p = fh_stristr(rx_buf, "Enable LUN");
        if (!p)
            p = fh_stristr(rx_buf, "Active LUN");
        if (p) {
            value = fh_parse_lun_list_mask(p);
            if (value != 0)
                ctx->reported_lun_enable_mask = value;
        }
    }

    if (ctx->reported_lun_count <= 0 && ctx->reported_lun_enable_mask != 0) {
        int count = fh_lun_count_from_mask(ctx->reported_lun_enable_mask);
        if (count > 0)
            ctx->reported_lun_count = count;
    }
}

static void fh_update_storage_info_hints_shared(edl_firehose_t *ctx, const char *rx_buf)
{
    if (!ctx || !rx_buf || !rx_buf[0])
        return;

    fh_update_storage_info_hints(ctx, rx_buf);

    int parsed_lun_count = 0;
    uint32_t parsed_lun_mask = 0;
    if (edl_storage_extract_lun_hints(rx_buf, &parsed_lun_count, &parsed_lun_mask)) {
        if (parsed_lun_mask != 0)
            ctx->reported_lun_enable_mask = parsed_lun_mask;
        if (parsed_lun_count > 0)
            ctx->reported_lun_count = parsed_lun_count;
    }
}

/*
 * Read XML response from device.
 * Searches for <response .../> in the received data stream.
 * Returns 0 if ACK/NAK found, fills resp. Returns -1 on timeout.
 */
static edl_error_t fh_wait_response(edl_firehose_t *ctx, edl_xml_response_t *resp, int timeout_ms)
{
    char rx_buf[FH_RX_BUF_SIZE];
    int rx_pos = 0;
    int idle_rounds = 0;
    const uint64_t start_ms = fh_now_ms();

    while (1) {
        const char *last_log_end = NULL;

        if (fh_is_cancelled(ctx))
            return EDL_ERR_CANCELLED;
        if ((int)(fh_now_ms() - start_ms) >= timeout_ms)
            return EDL_ERR_TIMEOUT;

        int avail = edl_port_bytes_available(ctx->port);
        if (avail > 0) {
            idle_rounds = 0;
            if (rx_pos >= FH_RX_BUF_SIZE - 1)
                fh_compact_rx_window(rx_buf, &rx_pos);
            int to_read = avail;
            if (rx_pos + to_read >= FH_RX_BUF_SIZE - 1)
                to_read = FH_RX_BUF_SIZE - 1 - rx_pos;
            if (to_read <= 0) {
                fh_compact_rx_window(rx_buf, &rx_pos);
                continue;
            }
            int got = edl_port_read(ctx->port, (uint8_t *)(rx_buf + rx_pos), to_read, 1000);
            if (got > 0) {
                rx_pos += got;
                rx_buf[rx_pos] = '\0';

                /* Extract device log messages */
                const char *log_end = NULL;
                const char *log_start = edl_xml_find_element(rx_buf, "log", &log_end);
                while (log_start) {
                    edl_xml_response_t log_resp;
                    char element[512];
                    int elem_len = (int)(log_end - log_start);
                    if (elem_len > 0 && elem_len < (int)sizeof(element)) {
                        memcpy(element, log_start, elem_len);
                        element[elem_len] = '\0';
                        if (edl_xml_parse_response(element, &log_resp) == 0) {
                            const char *msg = edl_xml_get_attr(&log_resp, "value");
                            if (msg) fh_log_detail(ctx, "%s", msg);
                        }
                        last_log_end = log_end;
                    }
                    log_start = edl_xml_find_element(log_end, "log", &log_end);
                }

                /* Look for <response .../> */
                const char *resp_end = NULL;
                const char *resp_start = edl_xml_find_element(rx_buf, "response", &resp_end);
                if (resp_start) {
                    char element[1024];
                    int elem_len = (int)(resp_end - resp_start);
                    if (elem_len > 0 && elem_len < (int)sizeof(element)) {
                        memcpy(element, resp_start, elem_len);
                        element[elem_len] = '\0';
                        if (edl_xml_parse_response(element, resp) == 0) {
                            fh_extract_msm_hwid_from_rx(ctx, rx_buf);
                            fh_cache_xml_tail(ctx, resp_end, rx_pos, rx_buf);
                            return EDL_OK;
                        }
                    }
                }

                if (last_log_end)
                    fh_trim_rx_prefix(rx_buf, &rx_pos, last_log_end);
                else if (rx_pos >= FH_RX_BUF_SIZE - 512)
                    fh_compact_rx_window(rx_buf, &rx_pos);
            }
        } else {
            edl_error_t idle_err = fh_wait_idle_backoff(ctx, start_ms, &idle_rounds);
            if (idle_err != EDL_OK)
                return idle_err;
        }
    }
}

/*
 * 与 fh_wait_response 类似，但保留完整 rx 快照并拼接所有 <log value="..."/> 供 getddrtype 解析。
 */
static edl_error_t fh_wait_response_capture(edl_firehose_t *ctx, edl_xml_response_t *resp,
                                            char *rx_snapshot, size_t rx_cap,
                                            char *log_concat, size_t log_cap,
                                            int timeout_ms)
{
    char rx_buf[FH_RX_BUF_SIZE];
    int rx_pos = 0;
    int idle_rounds = 0;
    if (rx_snapshot && rx_cap > 0) rx_snapshot[0] = '\0';
    if (log_concat && log_cap > 0) log_concat[0] = '\0';

    const uint64_t start_ms = fh_now_ms();

    while (1) {
        const char *last_log_end = NULL;

        if (fh_is_cancelled(ctx))
            return EDL_ERR_CANCELLED;
        if ((int)(fh_now_ms() - start_ms) >= timeout_ms)
            return EDL_ERR_TIMEOUT;

        int avail = edl_port_bytes_available(ctx->port);
        if (avail > 0) {
            idle_rounds = 0;
            if (rx_pos >= FH_RX_BUF_SIZE - 1)
                fh_compact_rx_window(rx_buf, &rx_pos);
            int to_read = avail;
            if (rx_pos + to_read >= FH_RX_BUF_SIZE - 1)
                to_read = FH_RX_BUF_SIZE - 1 - rx_pos;
            if (to_read <= 0) {
                fh_compact_rx_window(rx_buf, &rx_pos);
                continue;
            }
            int got = edl_port_read(ctx->port, (uint8_t *)(rx_buf + rx_pos), to_read, 1000);
            if (got > 0) {
                rx_pos += got;
                rx_buf[rx_pos] = '\0';

                const char *log_end = NULL;
                const char *log_start = edl_xml_find_element(rx_buf, "log", &log_end);
                while (log_start) {
                    edl_xml_response_t log_resp;
                    char element[512];
                    int elem_len = (int)(log_end - log_start);
                    if (elem_len > 0 && elem_len < (int)sizeof(element)) {
                        memcpy(element, log_start, elem_len);
                        element[elem_len] = '\0';
                        if (edl_xml_parse_response(element, &log_resp) == 0) {
                            const char *msg = edl_xml_get_attr(&log_resp, "value");
                            if (msg) {
                                fh_log_detail(ctx, "%s", msg);
                                if (log_concat && log_cap > 0) {
                                    size_t cl = strlen(log_concat);
                                    if (cl > 0 && cl + 3 < log_cap)
                                        strncat(log_concat, " | ", log_cap - cl - 1);
                                    strncat(log_concat, msg, log_cap - strlen(log_concat) - 1);
                                }
                            }
                        }
                        last_log_end = log_end;
                    }
                    log_start = edl_xml_find_element(log_end, "log", &log_end);
                }

                const char *resp_end = NULL;
                const char *resp_start = edl_xml_find_element(rx_buf, "response", &resp_end);
                if (resp_start) {
                    char element[1024];
                    int elem_len = (int)(resp_end - resp_start);
                    if (elem_len > 0 && elem_len < (int)sizeof(element)) {
                        memcpy(element, resp_start, elem_len);
                        element[elem_len] = '\0';
                        if (edl_xml_parse_response(element, resp) == 0) {
                            fh_extract_msm_hwid_from_rx(ctx, rx_buf);
                            if (rx_snapshot && rx_cap > 0)
                                snprintf(rx_snapshot, rx_cap, "%s", rx_buf);
                            fh_cache_xml_tail(ctx, resp_end, rx_pos, rx_buf);
                            return EDL_OK;
                        }
                    }
                }

                if (last_log_end && (!rx_snapshot || rx_cap == 0) && (!log_concat || log_cap == 0))
                    fh_trim_rx_prefix(rx_buf, &rx_pos, last_log_end);
                else if (rx_pos >= FH_RX_BUF_SIZE - 512)
                    fh_compact_rx_window(rx_buf, &rx_pos);
            }
        } else {
            edl_error_t idle_err = fh_wait_idle_backoff(ctx, start_ms, &idle_rounds);
            if (idle_err != EDL_OK)
                return idle_err;
        }
    }
}

static int fh_substr_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            unsigned char c1 = (unsigned char)hay[i + j];
            unsigned char c2 = (unsigned char)needle[j];
            if (!c1) return 0;
            if (tolower(c1) != tolower(c2)) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

static void fh_ddr_hint_from_response(const edl_xml_response_t *resp, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!resp) return;
    for (int i = 0; i < resp->attr_count; i++) {
        #ifdef _WIN32
        if (_stricmp(resp->attrs[i].name, "ddrtype") == 0 ||
            _stricmp(resp->attrs[i].name, "DDRType") == 0 ||
            _stricmp(resp->attrs[i].name, "ddr_type") == 0)
        #else
        if (strcasecmp(resp->attrs[i].name, "ddrtype") == 0 ||
            strcasecmp(resp->attrs[i].name, "DDRType") == 0 ||
            strcasecmp(resp->attrs[i].name, "ddr_type") == 0)
        #endif
        {
            snprintf(out, out_size, "%s", resp->attrs[i].value);
            return;
        }
        if (fh_substr_ci(resp->attrs[i].value, "lpddr") ||
            fh_substr_ci(resp->attrs[i].value, "ddr")) {
            snprintf(out, out_size, "%s", resp->attrs[i].value);
            return;
        }
    }
}

static void fh_ddr_hint_from_text(const char *text, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!text || !*text) return;
    /* 较长 token 在前，避免 LPDDR5 抢先于 LPDDR5X */
    static const char *keys[] = {
        "LPDDR5X", "LPDDR5x", "LPDDR5",
        "LPDDR4X", "LPDDR4x", "LPDDR4",
        "DDR5", "DDR4"
    };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        if (fh_substr_ci(text, keys[k])) {
            snprintf(out, out_size, "%s", keys[k]);
            return;
        }
    }
}

/* 读 GPT 后先发 purge/discard，避免残留 XML 干扰 getddrtype 解析 */
static void fh_sync_line_before_getddr(edl_firehose_t *ctx)
{
    edl_port_purge(ctx->port);
    fh_discard_rx(ctx);
    edl_sleep_ms(120);
    fh_discard_rx(ctx);
    edl_sleep_ms(50);
}

static edl_error_t fh_try_get_ddr_type_once(edl_firehose_t *ctx, char *out, size_t out_size, int timeout_ms)
{
    char xml[256];
    int nx = edl_xml_build_getddrtype(xml, sizeof(xml));
    if (nx <= 0 || nx >= (int)sizeof(xml))
        return EDL_ERR_INVALID_PARAM;

    fh_send_xml(ctx, xml);
    fh_log_detail(ctx, "getddrtype cmd: %s", xml);

    edl_xml_response_t resp;
    char rx_snap[FH_RX_BUF_SIZE];
    char logs[4096];
    memset(&resp, 0, sizeof(resp));
    edl_error_t wait_err =
        fh_wait_response_capture(ctx, &resp, rx_snap, sizeof(rx_snap), logs, sizeof(logs), timeout_ms);
    if (wait_err == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (wait_err != EDL_OK)
        return EDL_ERR_TIMEOUT;

    if (resp.is_nak) {
        fh_log_detail(ctx, "getddrtype: NAK");
        return EDL_ERR_FH_NAK;
    }

    if (!resp.is_ack) {
        fh_log_detail(ctx, "getddrtype: 非 ACK");
        return EDL_ERR_FH_NAK;
    }

    fh_ddr_hint_from_response(&resp, out, out_size);
    if (out[0]) {
        fh_log_detail(ctx, "DDR 类型（getddrtype）: %s", out);
        return EDL_OK;
    }

    fh_ddr_hint_from_text(logs, out, out_size);
    if (out[0]) {
        fh_log_detail(ctx, "DDR 类型（getddrtype log）: %s", out);
        return EDL_OK;
    }

    fh_ddr_hint_from_text(rx_snap, out, out_size);
    if (out[0]) {
        fh_log_detail(ctx, "DDR 类型（getddrtype 应答）: %s", out);
        return EDL_OK;
    }

    snprintf(out, out_size, "ACK（Loader 未返回可解析的 DDR 描述）");
    fh_log_detail(ctx, "getddrtype: %s", out);
    return EDL_OK;
}

edl_error_t edl_firehose_try_get_ddr_type(edl_firehose_t *ctx, char *out, size_t out_size)
{
    if (!ctx || !out || out_size == 0) return EDL_ERR_INVALID_PARAM;
    out[0] = '\0';

    fh_sync_line_before_getddr(ctx);

    edl_error_t e = fh_try_get_ddr_type_once(ctx, out, out_size, FH_GETDDR_TIMEOUT_MS);
    if (e != EDL_ERR_TIMEOUT)
        return e;

    fh_log_detail(ctx, "getddrtype 首次超时，同步链路后重试一次…");
    fh_sync_line_before_getddr(ctx);
    e = fh_try_get_ddr_type_once(ctx, out, out_size, FH_GETDDR_RETRY_MS);
    if (e == EDL_ERR_TIMEOUT)
        fh_log_detail(ctx, "getddrtype: 仍无应答（部分旧 Programmer 不支持；可看 Sahara「DRAM 代际（推断）」）");
    return e;
}

/* log_nak_on_reject: fixgpt 等会多次尝试，静默 NAK 避免刷屏，由上层汇总一条说明 */
static edl_error_t fh_send_and_wait_ack_ex(edl_firehose_t *ctx, const char *xml, int timeout_ms,
                                           int log_nak_on_reject)
{
    fh_send_xml(ctx, xml);

    edl_xml_response_t resp;
    for (int retry = 0; retry < 5; retry++) {
        edl_error_t wait_err = fh_wait_response(ctx, &resp, timeout_ms);
        if (wait_err == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;
        if (wait_err == EDL_OK) {
            if (resp.is_ack) return EDL_OK;
            if (resp.is_nak) {
                const char *raw_err = edl_xml_get_attr(&resp, "rawmode");
                const char *detail  = edl_xml_get_attr(&resp, "value");
                if (log_nak_on_reject) {
                    fh_log(ctx, "设备拒绝 (NAK): %s", detail ? detail : "未知原因");
                    if (raw_err)
                        fh_log_detail(ctx, "设备原始错误: %s", raw_err);
                } else {
                    fh_log_detail(ctx, "NAK: %s", detail ? detail : "(无 value)");
                    if (raw_err)
                        fh_log_detail(ctx, "rawmode: %s", raw_err);
                }
                return EDL_ERR_FH_NAK;
            }
        }
        if (fh_sleep_cancelable(ctx, 50))
            return EDL_ERR_CANCELLED;
    }
    return EDL_ERR_TIMEOUT;
}

static edl_error_t fh_send_and_wait_ack(edl_firehose_t *ctx, const char *xml, int timeout_ms)
{
    return fh_send_and_wait_ack_ex(ctx, xml, timeout_ms, 1);
}

/*
 * Receive raw data after sending a read command.
 * The device sends sector_size * num_sectors bytes of raw data,
 * then an XML ACK/NAK.
 */
static edl_error_t fh_receive_data(edl_firehose_t *ctx, uint8_t *buf, int total_bytes, int timeout_ms)
{
    int n = edl_port_read_exact_ex(ctx->port, buf, total_bytes, timeout_ms,
                                   ctx->cb.is_cancelled, ctx->cb.user_data);
    if (n == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (n < 0 || n != total_bytes)
        return EDL_ERR_TIMEOUT;
    /* 进度由上层 read_partition / read_partition_mem 按整分区或整文件汇报；
     * 此处 total 仅为单次 read 载荷，与分区总大小不一致，上报会导致 UI 批量进度错乱 */
    return EDL_OK;
}

/* ===== Public API ===== */

edl_firehose_t *edl_firehose_create(edl_port_t *port, const edl_callbacks_t *cb)
{
    if (!port) return NULL;
    edl_firehose_t *ctx = (edl_firehose_t *)calloc(1, sizeof(edl_firehose_t));
    if (!ctx) return NULL;
    ctx->port = port;
    if (cb) ctx->cb = *cb;
    strcpy(ctx->storage_type, "ufs");
    ctx->sector_size = 4096;
    ctx->max_payload_size = FH_DEFAULT_PAYLOAD;
    edl_port_set_transfer_window(port, FH_DEFAULT_PAYLOAD);
    ctx->msm_hwid_hint = 0;
    ctx->reported_lun_count = 0;
    ctx->reported_lun_enable_mask = 0;
    ctx->pad_short_image_to_gpt = true;
    /* 默认关闭：部分 loader 对 read_back_verify 响应异常或写后校验失败导致用户误以为「写入成功仍不开机」 */
    ctx->program_read_back_verify = false;
    return ctx;
}

void edl_firehose_set_write_options(edl_firehose_t *ctx,
                                    bool pad_short_image_to_gpt,
                                    bool program_read_back_verify)
{
    if (!ctx) return;
    ctx->pad_short_image_to_gpt = pad_short_image_to_gpt;
    ctx->program_read_back_verify = program_read_back_verify;
}

void edl_firehose_destroy(edl_firehose_t *ctx)
{
    free(ctx);
}

edl_error_t edl_firehose_configure(edl_firehose_t *ctx, const char *storage_type,
                                    int preferred_payload_size)
{
    if (!ctx) return EDL_ERR_INVALID_PARAM;
    snprintf(ctx->storage_type, sizeof(ctx->storage_type), "%s", storage_type ? storage_type : "ufs");

#ifdef _WIN32
    ctx->sector_size = (_stricmp(ctx->storage_type, "emmc") == 0) ? 512 : 4096;
#else
    ctx->sector_size = (strcasecmp(ctx->storage_type, "emmc") == 0) ? 512 : 4096;
#endif

    {
        const int payload_candidates[] = {
            preferred_payload_size > 0 ? preferred_payload_size : FH_OPTIMAL_PAYLOAD,
            FH_FALLBACK_PAYLOAD_1,
            FH_FALLBACK_PAYLOAD_2,
            FH_FALLBACK_PAYLOAD_3,
        };

        for (int ci = 0; ci < (int)(sizeof(payload_candidates) / sizeof(payload_candidates[0])); ++ci) {
            const int req_payload = payload_candidates[ci];
            int seen = 0;
            if (req_payload <= 0)
                continue;
            for (int pi = 0; pi < ci; ++pi) {
                if (payload_candidates[pi] == req_payload) {
                    seen = 1;
                    break;
                }
            }
            if (seen)
                continue;

            char xml[FH_XML_BUF_SIZE];
            edl_xml_build_configure(xml, sizeof(xml), ctx->storage_type, req_payload);

            if (ci == 0) {
                fh_log_detail(ctx, "[Firehose] Configure request: %d KB", req_payload / 1024);
            } else {
                fh_log_detail(ctx, "[Firehose] Configure fallback: %d KB", req_payload / 1024);
            }

            edl_port_purge(ctx->port);
            fh_send_xml(ctx, xml);

            for (int i = 0; i < 5; i++) {
                edl_xml_response_t resp;
                edl_error_t wait_err = fh_wait_response(ctx, &resp, 3000);
                if (wait_err == EDL_ERR_CANCELLED)
                    return EDL_ERR_CANCELLED;
                if (wait_err == EDL_OK) {
                    if (resp.is_ack || resp.is_nak) {
                        int payload = edl_xml_get_attr_int(&resp, "MaxPayloadSizeToTargetInBytes", 0);
                        if (payload > 0) {
                            ctx->max_payload_size = payload < req_payload ? payload : req_payload;
                        } else {
                            ctx->max_payload_size = req_payload;
                        }
                        if (ctx->max_payload_size <= 0)
                            ctx->max_payload_size = req_payload;
                        edl_port_set_transfer_window(ctx->port, ctx->max_payload_size);

                        if (payload > 0 && payload < req_payload) {
                            fh_log_detail(ctx,
                                          "[Firehose] Configure ACK - sector:%d B | requested:%d KB | device_limit:%d KB",
                                          ctx->sector_size, req_payload / 1024, payload / 1024);
                        } else if (payload > req_payload) {
                            fh_log_detail(ctx,
                                          "[Firehose] Configure ACK - sector:%d B | requested:%d KB | device_max:%d KB | using:%d KB",
                                          ctx->sector_size,
                                          req_payload / 1024,
                                          payload / 1024,
                                          ctx->max_payload_size / 1024);
                        } else {
                            fh_log_detail(ctx, "[Firehose] Configure ACK - sector:%d B | payload:%d KB",
                                          ctx->sector_size, ctx->max_payload_size / 1024);
                        }
                        return EDL_OK;
                    }
                }
                if (fh_sleep_cancelable(ctx, 50))
                    return EDL_ERR_CANCELLED;
            }
        }

        fh_log(ctx, "Configure timeout: target may not be in Firehose mode");
        return EDL_ERR_FH_CONFIGURE;
    }

}

edl_error_t edl_firehose_ping(edl_firehose_t *ctx)
{
    if (!ctx) return EDL_ERR_INVALID_PARAM;
    char xml[FH_XML_BUF_SIZE];
    edl_xml_build_nop(xml, sizeof(xml));
    edl_port_purge(ctx->port);
    /*
     * 连接探测：慢速 USB / loader 忙时 XML 响应可能远超 3s；若仍用 fh_send_and_wait_ack(..., 3000)
     * 且内部 5 轮等待，易误判为超时。此处单发 NOP，单轮等待 25s，最多 3 轮。
     */
    fh_send_xml(ctx, xml);
    edl_xml_response_t resp;
    for (int retry = 0; retry < 3; retry++) {
        edl_error_t wait_err = fh_wait_response(ctx, &resp, 25000);
        if (wait_err == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;
        if (wait_err == EDL_OK) {
            if (resp.is_ack)
                return EDL_OK;
            if (resp.is_nak) {
                const char *raw_err = edl_xml_get_attr(&resp, "rawmode");
                const char *detail  = edl_xml_get_attr(&resp, "value");
                fh_log(ctx, "设备拒绝 (NAK): %s", detail ? detail : "未知原因");
                if (raw_err)
                    fh_log_detail(ctx, "设备原始错误: %s", raw_err);
                return EDL_ERR_FH_NAK;
            }
        }
        if (fh_sleep_cancelable(ctx, 80))
            return EDL_ERR_CANCELLED;
    }
    return EDL_ERR_TIMEOUT;
}

edl_error_t edl_firehose_get_storage_info(edl_firehose_t *ctx)
{
    const uint64_t start_ms = fh_now_ms();
    if (!ctx) return EDL_ERR_INVALID_PARAM;
    char xml[FH_XML_BUF_SIZE];
    edl_xml_build_getstorageinfo(xml, sizeof(xml));
    fh_discard_rx(ctx);
    edl_port_purge(ctx->port);
    edl_sleep_ms(40);
    fh_send_xml(ctx, xml);

    edl_xml_response_t resp;
    char full[FH_RX_BUF_SIZE];
    char logs[4096];
    for (int retry = 0; retry < 5; retry++) {
        memset(&resp, 0, sizeof(resp));
        full[0] = '\0';
        logs[0] = '\0';
        edl_error_t wait_err =
            fh_wait_response_capture(ctx, &resp, full, sizeof(full), logs, sizeof(logs), 5000);
        if (wait_err == EDL_ERR_CANCELLED) {
            fh_log_elapsed_detail(ctx, "getstorageinfo", EDL_ERR_CANCELLED, start_ms);
            return EDL_ERR_CANCELLED;
        }
        if (wait_err == EDL_OK) {
            const char *hint_src = full;
            if ((hint_src[0] == '\0' || strstr(hint_src, "<log") == NULL) && logs[0])
                hint_src = logs;
            fh_update_storage_info_hints_shared(ctx, hint_src);
            if (resp.is_ack) {
                fh_log_elapsed_detail(ctx, "getstorageinfo", EDL_OK, start_ms);
                return EDL_OK;
            }
            if (resp.is_nak) {
                fh_log_elapsed_detail(ctx, "getstorageinfo", EDL_ERR_FH_NAK, start_ms);
                return EDL_ERR_FH_NAK;
            }
        }
        if (fh_sleep_cancelable(ctx, 80)) {
            fh_log_elapsed_detail(ctx, "getstorageinfo", EDL_ERR_CANCELLED, start_ms);
            return EDL_ERR_CANCELLED;
        }
    }
    fh_log_elapsed_detail(ctx, "getstorageinfo", EDL_ERR_TIMEOUT, start_ms);
    return EDL_ERR_TIMEOUT;
}

edl_error_t edl_firehose_get_storage_device_report(edl_firehose_t *ctx, char *report, size_t report_size)
{
    const uint64_t start_ms = fh_now_ms();
    if (!ctx || !report || report_size < 128) return EDL_ERR_INVALID_PARAM;
    report[0] = '\0';

    char xml[FH_XML_BUF_SIZE];
    edl_xml_build_getstorageinfo(xml, sizeof(xml));
    fh_discard_rx(ctx);
    edl_port_purge(ctx->port);
    edl_sleep_ms(60);
    fh_send_xml(ctx, xml);
    fh_log_detail(ctx, "getstorageinfo（字库设备信息）");

    edl_xml_response_t resp;
    char full[FH_RX_BUF_SIZE];
    char logs[16384];
    memset(&resp, 0, sizeof(resp));
    logs[0] = '\0';
    edl_error_t wait_err =
        fh_wait_response_capture(ctx, &resp, full, sizeof(full), logs, sizeof(logs), 20000);
    if (wait_err == EDL_ERR_CANCELLED) {
        fh_log_elapsed_detail(ctx, "getstorageinfo(字库报告)", EDL_ERR_CANCELLED, start_ms);
        return EDL_ERR_CANCELLED;
    }
    if (wait_err != EDL_OK) {
        fh_log_elapsed_detail(ctx, "getstorageinfo(字库报告)", EDL_ERR_TIMEOUT, start_ms);
        return EDL_ERR_TIMEOUT;
    }
    {
        const char *hint_src = full;
        if ((hint_src[0] == '\0' || strstr(hint_src, "<log") == NULL) && logs[0])
            hint_src = logs;
        fh_update_storage_info_hints_shared(ctx, hint_src);
    }
    if (resp.is_nak) {
        fh_log_elapsed_detail(ctx, "getstorageinfo(字库报告)", EDL_ERR_FH_NAK, start_ms);
        return EDL_ERR_FH_NAK;
    }
    if (!resp.is_ack) {
        fh_log_elapsed_detail(ctx, "getstorageinfo(字库报告)", EDL_ERR_FH_NAK, start_ms);
        return EDL_ERR_FH_NAK;
    }

    {
        const char *report_src = full;
        if ((report_src[0] == '\0' || strstr(report_src, "<log") == NULL) && logs[0])
            report_src = logs;
        edl_storage_build_device_report(report_src, report, report_size);
    }
    fh_log_elapsed_detail(ctx, "getstorageinfo(字库报告)", EDL_OK, start_ms);
    return EDL_OK;
}

static edl_error_t fh_read_sectors_ex(edl_firehose_t *ctx, int lun,
                                      int64_t start_sector, int num_sectors,
                                      const char *label,
                                      uint8_t *out_data, int out_data_len,
                                      int *out_len)
{
    if (!ctx || !out_data || !out_len) return EDL_ERR_INVALID_PARAM;

    int total_bytes = num_sectors * ctx->sector_size;
    if (out_data_len < total_bytes) return EDL_ERR_INVALID_PARAM;
    *out_len = 0;

    char xml[FH_XML_BUF_SIZE];
    edl_xml_build_read(xml, sizeof(xml), ctx->sector_size, lun, start_sector, num_sectors,
                       NULL, label);

    edl_port_purge(ctx->port);

    fh_log_detail(ctx, "LUN%d read cmd: %.200s", lun, xml);
    fh_send_xml(ctx, xml);

    /*
     * Firehose read protocol – two variants observed:
     *
     * Modern (rawmode): Device → <response ACK rawmode="true"/>
     *                            raw-binary-data
     *                            <response ACK rawmode="false"/>
     *
     * Legacy (direct):  Device → [<log .../>]
     *                            raw-binary-data
     *                            <response ACK rawmode="false"/>
     *
     * On failure:       Device → <response NAK .../>
     *
     * Strategy: read initial data, check for rawmode="true", NAK, or binary data.
     */
    int read_timeout = fh_pick_read_timeout_ms(start_sector, num_sectors, total_bytes, label);
    int data_collected = 0;
    int trailing_ack_in_probe = 0;

    /* Phase 1: probe device response – handle rawmode ACK, NAK, or direct binary */
    {
        uint8_t probe[256 * 1024];
        int probe_len = 0;
        int max_rounds = 20;
        int header_done = 0;

        for (int round = 0; round < max_rounds && !header_done; round++) {
            int space = (int)sizeof(probe) - probe_len;
            if (space <= 0) { probe_len = 0; space = (int)sizeof(probe); }

            int got = edl_port_read(ctx->port, probe + probe_len, space, read_timeout);
            if (got <= 0) {
                fh_log(ctx, "LUN%d read: 无响应 (timeout %dms, round %d, buf %d)",
                       lun, read_timeout, round, probe_len);
                return EDL_ERR_TIMEOUT;
            }
            probe_len += got;

            if (round == 0) {
                fh_log_detail(ctx, "LUN%d read: 收到 %d 字节, first=0x%02X '%c'",
                       lun, got, probe[0],
                       (probe[0] >= 0x20 && probe[0] < 0x7F) ? (char)probe[0] : '.');
            }

            /* --- Check 1: NAK → fast fail (only scan XML portion, not binary) --- */
            {
                int nak_limit = probe_len < 2048 ? probe_len : 2048;
                for (int i = 0; i <= nak_limit - 3; i++) {
                    if (probe[i] == 'N' && probe[i+1] == 'A' && probe[i+2] == 'K') {
                        int z = probe_len < (int)sizeof(probe) - 1 ? probe_len : (int)sizeof(probe) - 1;
                        probe[z] = '\0';
                        /* 高通 UFS 常见仅 LUN0–5；LUN6/7 等设备会 NAK「无槽位」——属正常，勿当故障 */
                        bool lun_absent = (strstr((char *)probe, "UFS Device slot") != NULL ||
                                           strstr((char *)probe, "Failed to open") != NULL);
                        if (lun_absent) {
                            fh_log_detail(ctx, "LUN%d: 无此 UFS 槽位（多数机型仅 LUN0–5；LUN6/7 无槽位为正常）", lun);
                            fh_log_detail(ctx, "NAK 原文: %s", (char *)probe);
                        } else {
                            fh_log(ctx, "LUN%d read NAK: %.200s", lun, (char *)probe);
                        }
                        return lun_absent ? EDL_ERR_FH_LUN_ABSENT : EDL_ERR_FH_NAK;
                    }
                }
            }

            /* --- Check 2: rawmode="true" ACK (modern Firehose) --- */
            {
                static const char rm_pat[] = "rawmode=\"true\"";
                int rm_len = (int)sizeof(rm_pat) - 1;
                int scan_limit = probe_len < 4096 ? probe_len : 4096;
                for (int i = 0; i <= scan_limit - rm_len; i++) {
                    if (memcmp(probe + i, rm_pat, rm_len) == 0) {
                        static const char endtag[] = "</data>";
                        int elen = (int)sizeof(endtag) - 1;
                        int ds = -1;
                        for (int j = i + rm_len; j <= scan_limit && j <= probe_len - elen; j++) {
                            if (memcmp(probe + j, endtag, elen) == 0) { ds = j + elen; break; }
                        }
                        if (ds < 0) break;

                        while (ds < probe_len && (probe[ds] == '\n' || probe[ds] == '\r'))
                            ds++;

                        int leftover = probe_len - ds;
                        if (leftover > 0) {
                            int cp = leftover < total_bytes ? leftover : total_bytes;
                            memcpy(out_data, probe + ds, cp);
                            data_collected = cp;
                        }

                        /* Check if rawmode="false" ACK is also in probe (after binary data) */
                        if (leftover > total_bytes) {
                            int tail_start = ds + total_bytes;
                            int tail_len = probe_len - tail_start;
                            trailing_ack_in_probe =
                                fh_handle_read_tail_bytes(ctx, probe + tail_start, tail_len);
                        }

                        header_done = 1;
                        fh_log_detail(ctx, "LUN%d: rawmode ACK, ds=%d, left=%d, need=%d, tail_ack=%d",
                               lun, ds, leftover, total_bytes, trailing_ack_in_probe);
                        break;
                    }
                }
                if (header_done) break;
            }

            /* --- Check 3: XML log/response WITHOUT rawmode → legacy/direct mode --- */
            if (probe[0] == '<') {
                int bin_start = -1;
                for (int i = 0; i < probe_len - 1; i++) {
                    if (probe[i] == '>' &&
                        (probe[i + 1] != '<' && probe[i + 1] != ' ' &&
                         probe[i + 1] != '\t')) {
                        bin_start = i + 1;
                        while (bin_start < probe_len &&
                               (probe[bin_start] == '\n' || probe[bin_start] == '\r'))
                            bin_start++;
                        break;
                    }
                }

                if (round == 0) {
                    int log_len = (bin_start > 0) ? bin_start : (probe_len < 500 ? probe_len : 500);
                    probe[log_len < (int)sizeof(probe) ? log_len : (int)sizeof(probe)-1] = '\0';
                    fh_log_detail(ctx, "LUN%d XML resp: %.300s", lun, (char *)probe);
                }

                if (bin_start > 0 && bin_start < probe_len) {
                    int bin_len = probe_len - bin_start;
                    int cp = bin_len < total_bytes ? bin_len : total_bytes;
                    memcpy(out_data, probe + bin_start, cp);
                    data_collected = cp;
                    header_done = 1;
                    if (bin_len > total_bytes) {
                        int tail_start = bin_start + total_bytes;
                        int tail_len = probe_len - tail_start;
                        trailing_ack_in_probe =
                            fh_handle_read_tail_bytes(ctx, probe + tail_start, tail_len);
                    }
                    break;
                }
                continue;
            }

            /* --- Check 4: first byte is NOT '<' → direct binary data --- */
            if (probe[0] != '<') {
                int cp = probe_len < total_bytes ? probe_len : total_bytes;
                memcpy(out_data, probe, cp);
                data_collected = cp;
                header_done = 1;
                if (probe_len > total_bytes) {
                    int tail_len = probe_len - total_bytes;
                    trailing_ack_in_probe =
                        fh_handle_read_tail_bytes(ctx, probe + total_bytes, tail_len);
                }
                break;
            }
        }

        if (!header_done) {
            fh_log(ctx, "LUN%d read: 未收到有效响应 (probed %d bytes)", lun, probe_len);
            return EDL_ERR_FH_READ;
        }
    }

    /* Phase 2: read remaining binary data */
    if (ctx->progress_remap_active && data_collected > 0)
        fh_report_progress(ctx, data_collected, total_bytes);
    int remaining = total_bytes - data_collected;
    edl_error_t err = EDL_OK;
    if (remaining > 0) {
        err = fh_receive_data(ctx, out_data + data_collected, remaining, read_timeout);
    }
    if (err != EDL_OK) {
        return err;
    }
    if (ctx->progress_remap_active)
        fh_report_progress(ctx, total_bytes, total_bytes);

    /* Phase 3: rawmode="false" ACK – skip if already consumed in probe */
    if (!trailing_ack_in_probe) {
        edl_xml_response_t resp;
        const int tail_ack_timeout =
            fh_pick_tail_ack_timeout_ms(start_sector, num_sectors, total_bytes, label);
        edl_error_t wait_err = fh_wait_response(ctx, &resp, tail_ack_timeout);
        if (wait_err == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;
        int ack_ok = (wait_err == EDL_OK && resp.is_ack);
        if (!ack_ok)
            fh_log_detail(ctx, "LUN%d: rawmode=false ACK 未收到 (数据仍可用)", lun);
    }

    *out_len = total_bytes;
    return EDL_OK;
}

edl_error_t edl_firehose_read_sectors(edl_firehose_t *ctx, int lun,
                                       int64_t start_sector, int num_sectors,
                                       uint8_t **out_data, int *out_len)
{
    if (!ctx || !out_data || !out_len) return EDL_ERR_INVALID_PARAM;
    int total_bytes = num_sectors * ctx->sector_size;
    uint8_t *buf = (uint8_t *)malloc((size_t)total_bytes);
    if (!buf) return EDL_ERR_NO_MEMORY;
    edl_error_t err = fh_read_sectors_ex(ctx, lun, start_sector, num_sectors, NULL,
                                         buf, total_bytes, out_len);
    if (err != EDL_OK) {
        free(buf);
        *out_data = NULL;
        *out_len = 0;
        return err;
    }
    *out_data = buf;
    return EDL_OK;
}

static edl_error_t fh_read_gpt_with_progress(edl_firehose_t *ctx, int lun,
                                             edl_partition_info_t *parts, int *count,
                                             int64_t progress_base_units,
                                             int64_t progress_total_units)
{
    if (!ctx || !parts || !count || *count <= 0) return EDL_ERR_INVALID_PARAM;

    /* C# reference: Realme mode uses 6 (UFS) / 34 (eMMC) to avoid NAK
     * ("read on PrimaryGPT:0:N not allowed on external network") */
    int gpt_sectors = edl_gpt_firehose_gpt_region_sectors(ctx->sector_size);
    uint8_t *gpt_data = NULL;
    int gpt_len = 0;

    if (progress_total_units > 0)
        fh_progress_remap_begin(ctx, progress_base_units + 1, 1, progress_total_units);
    edl_error_t err = edl_firehose_read_sectors(ctx, lun, 0, gpt_sectors,
                                                &gpt_data, &gpt_len);
    fh_progress_remap_reset(ctx);
    if (err != EDL_OK) return err;
    if (progress_total_units > 0)
        fh_report_gpt_scan_progress_units(ctx, progress_base_units + 2, progress_total_units);

    /* 非标准 GPT（条目区超出 6/34 扇区）时按头信息扩大一次读取，避免「头在、条目截断→0 分区」 */
    {
        int need_bytes = 0;
        if (edl_gpt_bytes_needed_for_primary_parse(gpt_data, gpt_len, &need_bytes) == 0 &&
            need_bytes > gpt_len && ctx->sector_size > 0) {
            int need_sec = (need_bytes + ctx->sector_size - 1) / ctx->sector_size;
            /* 大分区表（多 OEM 条目）可能需 >256 扇区；与抓包工具可读全表对齐 */
            if (need_sec > gpt_sectors && need_sec <= 4096 && need_bytes <= 16 * 1024 * 1024) {
                fh_log_detail(ctx, "LUN%d: GPT 条目区需约 %d 字节（>%d），扩大读取至 %d 扇区",
                              lun, need_bytes, gpt_len, need_sec);
                free(gpt_data);
                gpt_data = NULL;
                if (progress_total_units > 0)
                    fh_progress_remap_begin(ctx, progress_base_units + 2, 1, progress_total_units);
                err = edl_firehose_read_sectors(ctx, lun, 0, need_sec, &gpt_data, &gpt_len);
                fh_progress_remap_reset(ctx);
                if (err != EDL_OK) return err;
            }
        }
    }
    if (progress_total_units > 0)
        fh_report_gpt_scan_progress_units(ctx, progress_base_units + 3, progress_total_units);

    int max_parts = *count;
    int parsed = edl_gpt_parse(gpt_data, gpt_len, lun, ctx->sector_size, parts, max_parts, 0);
    if (parsed < 0) {
        /* 常见：头 CRC 与固件/读长不一致；静默用 IGNORE_HEADER_CRC 再解析，成功则不刷屏 */
        parsed = edl_gpt_parse(gpt_data, gpt_len, lun, ctx->sector_size, parts, max_parts,
                               EDL_GPT_PARSE_IGNORE_HEADER_CRC);
    }

    /*
     * 不在此补充 PrimaryGPT/BackupGPT 合成行：gpttool 等工具只统计 GPT 条目数组内「type+GUID 非全 0」
     * 的分区；合成元数据区行会导致每 LUN 多 0～2 条、与抓包工具分区数不一致。
     * 刷写 PrimaryGPT/BackupGPT 仍由 rawprogram XML 或专用流程提供。
     */

    free(gpt_data);

    if (parsed < 0) {
        *count = 0;
        return EDL_ERR_GPT_NO_SIGNATURE;
    }

    *count = parsed;
    return EDL_OK;
}

edl_error_t edl_firehose_read_gpt(edl_firehose_t *ctx, int lun,
                                   edl_partition_info_t *parts, int *count)
{
    return fh_read_gpt_with_progress(ctx, lun, parts, count, 0, 0);
}

/* Scan all requested LUNs and aggregate GPT entries into the caller buffer. */
static edl_error_t fh_gpt_scan_all_luns(edl_firehose_t *ctx,
                                        edl_partition_info_t *parts, int capacity,
                                        int max_lun, int *out_total)
{
    if (!ctx || !parts || !out_total || capacity <= 0 || max_lun <= 0)
        return EDL_ERR_INVALID_PARAM;

    *out_total = 0;

    /* Drop any stale XML/data fragments before starting a full multi-LUN scan. */
    fh_discard_rx(ctx);

    int total = 0;
    int scan_luns[32];
    int scan_count = fh_build_lun_scan_list(ctx, max_lun, scan_luns,
                                            (int)(sizeof(scan_luns) / sizeof(scan_luns[0])));
    if (scan_count <= 0)
        return EDL_ERR_GPT_SCAN_EMPTY;

    const int64_t total_progress_units =
        (int64_t)scan_count * (int64_t)FH_GPT_PROGRESS_STEPS;
    fh_report_gpt_scan_progress_units(ctx, 0, total_progress_units);

    if (ctx->reported_lun_enable_mask != 0) {
        char list[128];
        size_t pos = 0;
        list[0] = '\0';
        for (int i = 0; i < scan_count; i++) {
            int n = snprintf(list + pos, sizeof(list) - pos, "%sLUN%d",
                             i == 0 ? "" : " ", scan_luns[i]);
            if (n < 0 || (size_t)n >= sizeof(list) - pos) {
                pos = sizeof(list) - 1;
                break;
            }
            pos += (size_t)n;
        }
        fh_log_detail(ctx, "开始读取分区表：共 %d 个启用 LUN（%s）", scan_count, list);
    } else if (ctx->reported_lun_count > 0 && ctx->reported_lun_count < max_lun) {
        fh_log_detail(ctx, "开始读取分区表：共 %d 个 LUN（LUN0-LUN%d）",
                      scan_count,
                      ctx->reported_lun_count - 1);
    } else {
        fh_log_detail(ctx, "开始读取分区表：准备扫描 %d 个 LUN", scan_count);
    }

    int consecutive_absent = 0;
    for (int index = 0; index < scan_count; index++) {
        int lun = scan_luns[index];
        if (fh_is_cancelled(ctx))
            return EDL_ERR_CANCELLED;

        int remaining = capacity - total;
        if (remaining <= 0)
            break;

        if (index > 0 && fh_sleep_cancelable(ctx, FH_GPT_LUN_GAP_MS))
            return EDL_ERR_CANCELLED;

        edl_partition_info_t *slot = parts + total;
        int part_count = remaining;
        const int64_t base_units =
            (int64_t)index * (int64_t)FH_GPT_PROGRESS_STEPS;

        fh_log_detail(ctx, "读取 LUN%d GPT（%d/%d）...", lun, index + 1, scan_count);
        fh_report_gpt_scan_progress_units(ctx, base_units + 1, total_progress_units);
        edl_error_t err = fh_read_gpt_with_progress(ctx, lun, slot, &part_count,
                                                    base_units, total_progress_units);
        if (err == EDL_OK) {
            total += part_count;
            consecutive_absent = 0;
            fh_log_detail(ctx, "解析 LUN%d GPT 完成：%d 个分区", lun, part_count);
            fh_report_gpt_scan_progress_units(ctx, base_units + FH_GPT_PROGRESS_STEPS,
                                              total_progress_units);
            continue;
        }

        if (err == EDL_ERR_FH_LUN_ABSENT) {
            fh_log_detail(ctx, "LUN%d 不存在，已跳过（%d/%d）", lun, index + 1, scan_count);
            consecutive_absent++;
            fh_report_gpt_scan_progress_units(ctx, base_units + FH_GPT_PROGRESS_STEPS,
                                              total_progress_units);
            if (ctx->reported_lun_enable_mask == 0 && ctx->reported_lun_count <= 0
                && total > 0 && lun >= 5 && consecutive_absent >= 3) {
                fh_log_detail(ctx, "高位 LUN 连续 %d 次不存在，提前结束 GPT 扫描",
                              consecutive_absent);
                break;
            }
            continue;
        }

        consecutive_absent = 0;
        fh_log_detail(ctx, "LUN%d GPT 读取或解析失败（%s，%d/%d），继续后续 LUN",
                      lun, edl_error_str(err), index + 1, scan_count);
        fh_report_gpt_scan_progress_units(ctx, base_units + FH_GPT_PROGRESS_STEPS,
                                          total_progress_units);
    }

    *out_total = total;
    return total > 0 ? EDL_OK : EDL_ERR_GPT_SCAN_EMPTY;
}

edl_error_t edl_firehose_read_all_gpt(edl_firehose_t *ctx,
                                      edl_partition_info_t *parts, int *count, int max_lun)
{
    const uint64_t start_ms = fh_now_ms();
    if (!ctx || !parts || !count || *count <= 0 || max_lun <= 0)
        return EDL_ERR_INVALID_PARAM;

    int capacity = *count;
    int total = 0;
    edl_error_t scan_err = fh_gpt_scan_all_luns(ctx, parts, capacity, max_lun, &total);

#ifdef _WIN32
    const bool is_emmc = (_stricmp(ctx->storage_type, "emmc") == 0);
#else
    const bool is_emmc = (strcasecmp(ctx->storage_type, "emmc") == 0);
#endif

    /* Retry empty UFS scans by refreshing storage info only; do not set bootable storage here. */
    for (int attempt = 0;
         scan_err == EDL_ERR_GPT_SCAN_EMPTY && !is_emmc && attempt < 2;
         attempt++) {
        if (fh_is_cancelled(ctx)) {
            fh_log_elapsed_detail(ctx, "全 LUN 读取 GPT", EDL_ERR_CANCELLED, start_ms);
            return EDL_ERR_CANCELLED;
        }

        fh_log_detail(ctx,
                      "UFS 上 GPT 扫描为空，正在刷新 storage info 并重试 (%d/2)",
                      attempt + 1);
        fh_discard_rx(ctx);
        (void)edl_firehose_ping(ctx);
        (void)edl_firehose_get_storage_info(ctx);
        if (fh_sleep_cancelable(ctx, FH_GPT_RETRY_SETTLE_MS + attempt * 20)) {
            fh_log_elapsed_detail(ctx, "全 LUN 读取 GPT", EDL_ERR_CANCELLED, start_ms);
            return EDL_ERR_CANCELLED;
        }
        scan_err = fh_gpt_scan_all_luns(ctx, parts, capacity, max_lun, &total);
    }

    *count = total;
    if (total > 0)
        fh_log_detail(ctx, "GPT 分区总数: %d", total);

    {
        edl_error_t result = total > 0 ? EDL_OK : EDL_ERR_GPT_SCAN_EMPTY;
        fh_log_elapsed_detail(ctx, "全 LUN 读取 GPT", result, start_ms);
        return result;
    }
}

edl_error_t edl_firehose_read_partition(edl_firehose_t *ctx,
                                         const edl_partition_info_t *part,
                                         const char *save_path)
{
    if (!ctx || !part || !save_path) return EDL_ERR_INVALID_PARAM;
    if (part->num_sectors <= 0 || !fh_sector_range_valid(part->start_sector, part->num_sectors))
        return EDL_ERR_INVALID_PARAM;
    if (ctx->sector_size > 0 && part->num_sectors > INT64_MAX / (int64_t)ctx->sector_size)
        return EDL_ERR_INVALID_PARAM;

    FILE *fp = fopen(save_path, "wb");
    if (!fp) return EDL_ERR_FILE_IO;
    setvbuf(fp, NULL, _IOFBF, FH_FILE_BUF_SIZE);

    int64_t remaining_sectors = part->num_sectors;
    int64_t current_sector = part->start_sector;
    int64_t total_bytes = remaining_sectors * ctx->sector_size;
    int64_t written = 0;
    int max_sectors_per_read = ctx->max_payload_size / ctx->sector_size;
    if (max_sectors_per_read <= 0) {
        fclose(fp);
        return EDL_ERR_INVALID_PARAM;
    }
    uint8_t *chunk_buf = (uint8_t *)malloc((size_t)ctx->max_payload_size);
    if (!chunk_buf) {
        fclose(fp);
        return EDL_ERR_NO_MEMORY;
    }

    fh_log_detail(ctx, "正在读取 %s (%lld 扇区)...", part->name, (long long)part->num_sectors);

    while (remaining_sectors > 0) {
        if (fh_is_cancelled(ctx)) {
            free(chunk_buf);
            fclose(fp);
            fh_log_detail(ctx, "读取 %s 已取消", part->name);
            return EDL_ERR_CANCELLED;
        }

        int chunk = (int)(remaining_sectors > max_sectors_per_read ? max_sectors_per_read : remaining_sectors);

        int data_len = 0;
        edl_error_t err = fh_read_sectors_ex(ctx, part->lun, current_sector, chunk, NULL,
                                             chunk_buf, chunk * ctx->sector_size, &data_len);
        if (err != EDL_OK) {
            free(chunk_buf);
            fclose(fp);
            return err;
        }

        fwrite(chunk_buf, 1, data_len, fp);

        written += data_len;
        current_sector += chunk;
        remaining_sectors -= chunk;

        fh_report_progress(ctx, written, total_bytes);
    }

    free(chunk_buf);
    fclose(fp);
    fh_log_detail(ctx, "读取完成: %s", part->name);
    return EDL_OK;
}

/*
 * SakuraEDL WriteSparsePartitionSmartAsync / SparseStream.BuildChunkIndex：
 * - 仅写入 RAW、FILL；DONT_CARE 不编程；CRC32 不占用展开偏移（跳过文件内 payload）。
 * - 无实际数据时按展开大小擦除分区（与 C# EraseSectorsAsync 一致）。
 */
static int64_t fh_sparse_real_data_bytes(FILE *fp, const edl_sparse_header_t *hdr)
{
    if (fseek(fp, hdr->file_hdr_sz, SEEK_SET) != 0)
        return -1;

    int64_t real = 0;
    for (uint32_t i = 0; i < hdr->total_chunks; i++) {
        edl_sparse_chunk_header_t ch;
        if (fread(&ch, sizeof(ch), 1, fp) != 1)
            return -1;
        if (hdr->chunk_hdr_sz > sizeof(ch)) {
            if (fseek(fp, (long)(hdr->chunk_hdr_sz - sizeof(ch)), SEEK_CUR) != 0)
                return -1;
        }
        uint32_t payload = (ch.total_sz >= hdr->chunk_hdr_sz)
            ? (ch.total_sz - hdr->chunk_hdr_sz) : 0;
        int64_t out_bytes = (int64_t)ch.chunk_sz * (int64_t)hdr->blk_sz;

        if (ch.chunk_type == SPARSE_CHUNK_CRC32) {
            if (payload && fseek(fp, payload, SEEK_CUR) != 0)
                return -1;
            continue;
        }
        if (ch.chunk_type == SPARSE_CHUNK_RAW || ch.chunk_type == SPARSE_CHUNK_FILL)
            real += out_bytes;
        if (payload && fseek(fp, payload, SEEK_CUR) != 0)
            return -1;
    }
    return real;
}

static edl_error_t fh_write_partition_sparse_smart(edl_firehose_t *ctx,
                                                    const edl_partition_info_t *part,
                                                    const char *path)
{
    edl_error_t se = fh_require_part_sector_matches_session(ctx, part);
    if (se != EDL_OK)
        return se;

    if (part->num_sectors > 0 && !fh_sector_range_valid(part->start_sector, part->num_sectors))
        return EDL_ERR_INVALID_PARAM;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return EDL_ERR_FILE_NOT_FOUND;
    setvbuf(fp, NULL, _IOFBF, FH_FILE_BUF_SIZE);

    edl_sparse_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return EDL_ERR_FILE_IO;
    }
    if (hdr.magic != SPARSE_HEADER_MAGIC) {
        fclose(fp);
        return EDL_ERR_INVALID_PARAM;
    }

    if (hdr.blk_sz != (uint32_t)ctx->sector_size) {
        fh_log(ctx,
               "sparse 块大小 (%u B) 与 Firehose 扇区 (%d B) 不一致，无法按 SakuraEDL 方式写入。"
               "请使用与设备一致的扇区大小（UFS 常见 4096）。",
               (unsigned)hdr.blk_sz, ctx->sector_size);
        fclose(fp);
        return EDL_ERR_INVALID_PARAM;
    }

    int64_t expanded = (int64_t)hdr.total_blks * (int64_t)hdr.blk_sz;
    int64_t real_total = fh_sparse_real_data_bytes(fp, &hdr);
    if (real_total < 0) {
        fclose(fp);
        return EDL_ERR_FILE_IO;
    }

    if (real_total == 0) {
        fclose(fp);
        fh_log_detail(ctx, "sparse 无 RAW/FILL 数据，按 SakuraEDL 擦除展开区域...");
        int64_t num_sec = (expanded + ctx->sector_size - 1) / ctx->sector_size;
        edl_partition_info_t er = *part;
        if (part->num_sectors > 0 && num_sec > part->num_sectors) {
            fh_log_detail(ctx, "sparse 展开区域大于 GPT 分区，按分区大小擦除");
            num_sec = part->num_sectors;
        }
        er.num_sectors = num_sec;
        return edl_firehose_erase_partition(ctx, &er);
    }

    /* PrimaryGPT/BackupGPT：sparse 若有效数据不足声明扇区，展开会用 0 填洞，等同毁表 */
    if (fh_is_primary_or_backup_gpt_name(part->name) && part->num_sectors > 0) {
        int64_t need = part->num_sectors * (int64_t)ctx->sector_size;
        if (real_total < need) {
            fclose(fp);
            fh_log(ctx, "%s: sparse 内 RAW/FILL 有效数据 %lld 字节 < 需 %lld 字节，禁止用 0 展开；"
                      "请换完整 GPT 镜像或非 sparse。",
                   part->name, (long long)real_total, (long long)need);
            return EDL_ERR_INVALID_PARAM;
        }
    }

    if (fseek(fp, hdr.file_hdr_sz, SEEK_SET) != 0) {
        fclose(fp);
        return EDL_ERR_FILE_IO;
    }

    int chunk_size = ctx->max_payload_size;
    if (chunk_size < ctx->sector_size) {
        fclose(fp);
        return EDL_ERR_INVALID_PARAM;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)chunk_size);
    if (!buf) {
        fclose(fp);
        return EDL_ERR_NO_MEMORY;
    }

    fh_log_detail(ctx, "正在写入 %s（sparse 智能写入，跳过 DONT_CARE）...", part->name);

    int64_t current_output_offset = 0;
    int64_t total_written_prog = 0;
    int64_t max_output_bytes = -1;
    if (part->num_sectors > 0)
        max_output_bytes = (int64_t)part->num_sectors * (int64_t)ctx->sector_size;

    for (uint32_t ci = 0; ci < hdr.total_chunks; ci++) {
        if (fh_is_cancelled(ctx)) {
            free(buf);
            fclose(fp);
            fh_log_detail(ctx, "写入 %s（sparse）已取消", part->name);
            return EDL_ERR_CANCELLED;
        }

        edl_sparse_chunk_header_t ch;
        if (fread(&ch, sizeof(ch), 1, fp) != 1)
            break;
        if (hdr.chunk_hdr_sz > sizeof(ch)) {
            if (fseek(fp, (long)(hdr.chunk_hdr_sz - sizeof(ch)), SEEK_CUR) != 0) {
                free(buf);
                fclose(fp);
                return EDL_ERR_FILE_IO;
            }
        }
        uint32_t payload = (ch.total_sz >= hdr.chunk_hdr_sz)
            ? (ch.total_sz - hdr.chunk_hdr_sz) : 0;
        int64_t output_bytes = (int64_t)ch.chunk_sz * (int64_t)hdr.blk_sz;

        if (ch.chunk_type == SPARSE_CHUNK_CRC32) {
            if (payload && fseek(fp, payload, SEEK_CUR) != 0) {
                free(buf);
                fclose(fp);
                return EDL_ERR_FILE_IO;
            }
            continue;
        }

        if (max_output_bytes >= 0 && current_output_offset >= max_output_bytes) {
            fh_log_detail(ctx, "已达到分区末尾，跳过 sparse 剩余块");
            break;
        }
        int64_t eff_out = output_bytes;
        if (max_output_bytes >= 0) {
            int64_t room = max_output_bytes - current_output_offset;
            if (room <= 0) {
                fh_log_detail(ctx, "已达到分区末尾，跳过 sparse 剩余块");
                break;
            }
            if (eff_out > room) {
                fh_log_detail(ctx, "sparse 输出在分区边界截断 (%lld -> %lld 字节)",
                              (long long)eff_out, (long long)room);
                eff_out = room;
            }
        }

        if (ch.chunk_type == SPARSE_CHUNK_RAW) {
            int64_t range_off = current_output_offset;
            int64_t range_sz = eff_out;
            int64_t rangeWritten = 0;
            int64_t rangeStartSector = part->start_sector + (range_off / ctx->sector_size);

            while (rangeWritten < range_sz) {
                if (fh_is_cancelled(ctx)) {
                    free(buf); fclose(fp);
                    return EDL_ERR_CANCELLED;
                }
                int to_read = chunk_size;
                if (range_sz - rangeWritten < (int64_t)to_read)
                    to_read = (int)(range_sz - rangeWritten);
                if (fread(buf, 1, (size_t)to_read, fp) != (size_t)to_read) {
                    free(buf);
                    fclose(fp);
                    return EDL_ERR_FILE_IO;
                }
                int padded = ((to_read + ctx->sector_size - 1) / ctx->sector_size) * ctx->sector_size;
                if (padded > chunk_size) {
                    free(buf);
                    fclose(fp);
                    return EDL_ERR_INVALID_PARAM;
                }
                if (padded > to_read)
                    memset(buf + to_read, 0, (size_t)(padded - to_read));

                int64_t current_sector = rangeStartSector + (rangeWritten / ctx->sector_size);
                edl_error_t e = fh_write_sectors_program(ctx, part->lun, current_sector, NULL,
                                                         buf, padded, part->name);
                if (e != EDL_OK) {
                    free(buf);
                    fclose(fp);
                    return e;
                }
                rangeWritten += to_read;
                total_written_prog += to_read;
                fh_report_progress(ctx,
                                   total_written_prog > real_total ? real_total : total_written_prog,
                                   real_total);
            }
            if (eff_out < output_bytes) {
                int64_t skip = output_bytes - eff_out;
                if (fseek(fp, skip, SEEK_CUR) != 0) {
                    free(buf);
                    fclose(fp);
                    return EDL_ERR_FILE_IO;
                }
            }
        } else if (ch.chunk_type == SPARSE_CHUNK_FILL) {
            uint32_t fillv;
            if (payload < 4 || fread(&fillv, 4, 1, fp) != 1) {
                free(buf);
                fclose(fp);
                return EDL_ERR_FILE_IO;
            }
            if (payload > 4) {
                if (fseek(fp, (long)(payload - 4), SEEK_CUR) != 0) {
                    free(buf);
                    fclose(fp);
                    return EDL_ERR_FILE_IO;
                }
            }

            int64_t range_off = current_output_offset;
            int64_t range_sz = eff_out;
            int64_t rangeWritten = 0;
            int64_t rangeStartSector = part->start_sector + (range_off / ctx->sector_size);

            while (rangeWritten < range_sz) {
                if (fh_is_cancelled(ctx)) {
                    free(buf); fclose(fp);
                    return EDL_ERR_CANCELLED;
                }
                int to_gen = chunk_size;
                if (range_sz - rangeWritten < (int64_t)to_gen)
                    to_gen = (int)(range_sz - rangeWritten);
                for (int i = 0; i < to_gen; i += 4)
                    memcpy(buf + i, &fillv, (size_t)((to_gen - i >= 4) ? 4 : (size_t)(to_gen - i)));

                int padded = ((to_gen + ctx->sector_size - 1) / ctx->sector_size) * ctx->sector_size;
                if (padded > chunk_size) {
                    free(buf);
                    fclose(fp);
                    return EDL_ERR_INVALID_PARAM;
                }
                if (padded > to_gen)
                    memset(buf + to_gen, 0, (size_t)(padded - to_gen));

                int64_t current_sector = rangeStartSector + (rangeWritten / ctx->sector_size);
                edl_error_t e = fh_write_sectors_program(ctx, part->lun, current_sector, NULL,
                                                         buf, padded, part->name);
                if (e != EDL_OK) {
                    free(buf);
                    fclose(fp);
                    return e;
                }
                rangeWritten += to_gen;
                total_written_prog += to_gen;
                fh_report_progress(ctx,
                                   total_written_prog > real_total ? real_total : total_written_prog,
                                   real_total);
            }
        } else if (ch.chunk_type == SPARSE_CHUNK_DONT_CARE) {
            if (payload && fseek(fp, payload, SEEK_CUR) != 0) {
                free(buf);
                fclose(fp);
                return EDL_ERR_FILE_IO;
            }
        } else {
            if (payload && fseek(fp, payload, SEEK_CUR) != 0) {
                free(buf);
                fclose(fp);
                return EDL_ERR_FILE_IO;
            }
            fh_log_detail(ctx, "sparse: 未知 chunk 类型 0x%04x，已跳过", (unsigned)ch.chunk_type);
        }

        current_output_offset += eff_out;
    }

    free(buf);
    fclose(fp);
    fh_log_detail(ctx, "写入完成: %s", part->name);
    return EDL_OK;
}

edl_error_t edl_firehose_write_partition(edl_firehose_t *ctx,
                                          const edl_partition_info_t *part,
                                          const char *image_path)
{
    if (!ctx || !part || !image_path) return EDL_ERR_INVALID_PARAM;

    edl_error_t se = fh_require_part_sector_matches_session(ctx, part);
    if (se != EDL_OK)
        return se;

    edl_partition_info_t wpart = *part;
    fh_apply_gpt_capture_num_sectors(ctx, &wpart);

    if (edl_sparse_is_sparse(image_path))
        return fh_write_partition_sparse_smart(ctx, &wpart, image_path);

    FILE *fp = fopen(image_path, "rb");
    if (!fp) return EDL_ERR_FILE_NOT_FOUND;
    setvbuf(fp, NULL, _IOFBF, FH_FILE_BUF_SIZE);

#ifdef _WIN32
    _fseeki64(fp, 0, SEEK_END);
    int64_t total_file_bytes = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return EDL_ERR_FILE_IO;
    }
    int64_t total_file_bytes = (int64_t)ftello(fp);
#endif

    int ss = ctx->sector_size;
    if (ss <= 0) {
        fclose(fp);
        return EDL_ERR_INVALID_PARAM;
    }

    int64_t off_sec = wpart.file_sector_offset;
    if (off_sec < 0) off_sec = 0;
    int64_t off_bytes = off_sec * (int64_t)ss;
    if (off_bytes > total_file_bytes) {
        fclose(fp);
        fh_log_detail(ctx, "file_sector_offset 超出镜像大小，跳过写入");
        return EDL_ERR_INVALID_PARAM;
    }

#ifdef _WIN32
    _fseeki64(fp, off_bytes, SEEK_SET);
#else
    if (fseeko(fp, (off_t)off_bytes, SEEK_SET) != 0) {
        fclose(fp);
        return EDL_ERR_FILE_IO;
    }
#endif

    int64_t file_size = total_file_bytes - off_bytes;

    if (file_size <= 0) {
        fclose(fp);
        fh_log_detail(ctx, "镜像文件大小为 0（在 file_sector_offset 之后），跳过写入（不填充）");
        return EDL_OK;
    }

    int64_t image_sectors = (file_size + ss - 1) / (int64_t)ss;
    int64_t num_sectors;
    if (wpart.num_sectors > 0) {
        num_sectors = wpart.num_sectors;
        /*
         * PrimaryGPT/BackupGPT：禁止「短镜像 + 0 补足」。旧逻辑与 write_partition_mem 不一致，
         * 会把最后一扇区写成 0，直接毁掉主/备 GPT（读表 LUN0 变 0 分区等）。
         */
        if (fh_is_primary_or_backup_gpt_name(wpart.name)) {
            int64_t need_bytes = num_sectors * (int64_t)ss;
            int std_sec = edl_gpt_firehose_gpt_region_sectors(ss);
            /* BackupGPT：若镜像恰少 1 扇区，允许从设备读回当前备份区最后一扇区合并 */
            if (fh_is_backup_gpt_name(wpart.name) && file_size == need_bytes - ss &&
                num_sectors == (int64_t)std_sec && !wpart.start_sector_expr[0]) {
                /* ok — 在下方 gpt_preload 分支合并 */
            } else if (file_size < need_bytes) {
                fclose(fp);
                fh_log(ctx, "%s: 镜像在 file_sector_offset 后仅 %lld 字节，需至少 %lld 字节（%lld 扇区×%d）。"
                          " PrimaryGPT 必须完整；BackupGPT 若恰少 1 扇区且起始扇区为数值时将尝试从设备合并。",
                       wpart.name, (long long)file_size, (long long)need_bytes,
                       (long long)num_sectors, ss);
                return EDL_ERR_INVALID_PARAM;
            }
        } else {
            /*
             * 普通分区：默认镜像短于 GPT 时用 0 补满（rawprogram 习惯）。
             * pad_short_image_to_gpt==false 时仅按镜像长度（ceil），与 SakuraEDL FlashPartitionFromFileAsync 一致，
             * 可减少误将分区尾部抹零导致不开机。
             */
            if (!ctx->pad_short_image_to_gpt) {
                if (image_sectors < num_sectors) {
                    fh_log_detail(ctx,
                                  "正在写入 %s（仅镜像长度 / SakuraEDL）：编程 %lld 扇区，GPT 声明 %lld 扇区（不补零）",
                                  wpart.name, (long long)image_sectors, (long long)num_sectors);
                    num_sectors = image_sectors;
                }
            } else if (image_sectors < num_sectors) {
                fh_log_detail(ctx, "正在写入 %s：镜像扇区 %lld < 声明 %lld，不足部分填 0",
                              wpart.name, (long long)image_sectors, (long long)num_sectors);
            }
            if (image_sectors > num_sectors)
                fh_log_detail(ctx, "正在写入 %s：镜像扇区 %lld > 声明 %lld，将截断",
                              wpart.name, (long long)image_sectors, (long long)num_sectors);
        }
    } else {
        num_sectors = image_sectors;
    }

    if (!fh_sector_range_valid(wpart.start_sector, num_sectors)) {
        fclose(fp);
        fh_log(ctx, "写入 %s：起始扇区与扇区数超出可表示范围", wpart.name);
        return EDL_ERR_INVALID_PARAM;
    }

    int64_t progress_total_bytes = num_sectors * (int64_t)ss;
    int chunk_size = ctx->max_payload_size;
    int chunk_sectors = chunk_size / ss;

    fh_log_detail(ctx, "正在写入 %s (镜像可用 %lld 字节, 编程 %lld 扇区)...",
                  wpart.name, (long long)file_size, (long long)num_sectors);

    /* PrimaryGPT / BackupGPT：整段载入内存、重算 CRC，再按块写入 */
    uint8_t *gpt_preload = NULL;
    int gpt_preload_len = 0;
    if (fh_is_primary_gpt_name(wpart.name)) {
        gpt_preload_len = (int)(num_sectors * (int64_t)ss);
        if (gpt_preload_len < 1 || gpt_preload_len > INT_MAX) {
            fclose(fp);
            return EDL_ERR_INVALID_PARAM;
        }
        gpt_preload = (uint8_t *)malloc((size_t)gpt_preload_len);
        if (!gpt_preload) {
            fclose(fp);
            return EDL_ERR_NO_MEMORY;
        }
        size_t n = fread(gpt_preload, 1, (size_t)gpt_preload_len, fp);
        fclose(fp);
        fp = NULL;
        if ((int64_t)n < (int64_t)gpt_preload_len) {
            free(gpt_preload);
            fh_log(ctx, "%s: 读取镜像失败或长度不足", wpart.name);
            return EDL_ERR_FILE_IO;
        }
        if (edl_gpt_update_primary_crcs(gpt_preload, gpt_preload_len, ss) == 0)
            fh_log_detail(ctx, "%s: 已重算 GPT 头与分区条目表 CRC32（写入前）", wpart.name);
        else
            fh_log_detail(ctx, "%s: 未重算 CRC（可能不是标准主 GPT 布局）", wpart.name);
    } else if (fh_is_backup_gpt_name(wpart.name)) {
        int64_t need_bytes = num_sectors * (int64_t)ss;
        gpt_preload_len = (int)need_bytes;
        if (gpt_preload_len < 1 || gpt_preload_len > INT_MAX) {
            fclose(fp);
            return EDL_ERR_INVALID_PARAM;
        }
        gpt_preload = (uint8_t *)malloc((size_t)gpt_preload_len);
        if (!gpt_preload) {
            fclose(fp);
            return EDL_ERR_NO_MEMORY;
        }

        if ((int64_t)file_size >= need_bytes) {
            size_t n = fread(gpt_preload, 1, (size_t)gpt_preload_len, fp);
            fclose(fp);
            fp = NULL;
            if ((int64_t)n < need_bytes) {
                free(gpt_preload);
                fh_log(ctx, "%s: 读取镜像失败或长度不足", wpart.name);
                return EDL_ERR_FILE_IO;
            }
        } else {
            /* 仅数值起始扇区可从设备回读并补齐末扇区；动态表达式无法安全合并 */
            uint8_t *dev_buf = NULL;
            int dev_len = 0;
            edl_error_t rerr = edl_firehose_read_sectors(ctx, wpart.lun, wpart.start_sector,
                                                         (int)num_sectors, &dev_buf, &dev_len);
            if (rerr != EDL_OK || !dev_buf || dev_len < gpt_preload_len) {
                free(gpt_preload);
                free(dev_buf);
                fclose(fp);
                fh_log(ctx, "%s: 无法从设备读回当前备份区以合并尾扇区（%s），请提供完整镜像",
                       wpart.name, edl_error_str(rerr));
                return rerr != EDL_OK ? rerr : EDL_ERR_INVALID_PARAM;
            }
            memcpy(gpt_preload, dev_buf, (size_t)gpt_preload_len);
            free(dev_buf);

            size_t n = fread(gpt_preload, 1, (size_t)(need_bytes - ss), fp);
            fclose(fp);
            fp = NULL;
            if ((int64_t)n < need_bytes - ss) {
                free(gpt_preload);
                fh_log(ctx, "%s: 读取镜像失败", wpart.name);
                return EDL_ERR_FILE_IO;
            }
            fh_log_detail(ctx, "%s: 镜像仅 %lld 字节（少 1 扇区），已从设备合并尾扇区后写入",
                          wpart.name, (long long)file_size);
        }

        if (edl_gpt_update_backup_region_crcs(gpt_preload, gpt_preload_len, ss,
                                               wpart.start_sector) == 0)
            fh_log_detail(ctx, "%s: 已重算备份 GPT 头与分区条目表 CRC32（写入前）", wpart.name);
        else
            fh_log_detail(ctx, "%s: 未重算备份 CRC（起始扇区为表达式或镜像布局非标准）",
                          wpart.name);
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)chunk_size);
    if (!buf) {
        free(gpt_preload);
        if (fp)
            fclose(fp);
        return EDL_ERR_NO_MEMORY;
    }

    int64_t sector_offset = 0;
    int64_t total_written = 0;

    while (sector_offset < num_sectors) {
        if (fh_is_cancelled(ctx)) {
            free(buf);
            free(gpt_preload);
            if (fp) fclose(fp);
            fh_log_detail(ctx, "写入 %s 已取消", wpart.name);
            return EDL_ERR_CANCELLED;
        }

        int64_t remaining = num_sectors - sector_offset;
        int this_chunk_sectors = (int)(remaining > chunk_sectors ? chunk_sectors : remaining);
        int this_chunk_bytes = this_chunk_sectors * ss;

        if (gpt_preload) {
            memcpy(buf, gpt_preload + sector_offset * (int64_t)ss, (size_t)this_chunk_bytes);
        } else {
            size_t nread = fread(buf, 1, (size_t)this_chunk_bytes, fp);
            if (ferror(fp)) {
                free(buf);
                free(gpt_preload);
                if (fp)
                    fclose(fp);
                fh_log(ctx, "读取镜像失败（磁盘/文件错误）: %s", wpart.name);
                return EDL_ERR_FILE_IO;
            }
            if ((int64_t)nread < (int64_t)this_chunk_bytes)
                memset(buf + nread, 0, (size_t)this_chunk_bytes - nread);
        }

        const char *start_expr = (sector_offset == 0 && wpart.start_sector_expr[0]) ? wpart.start_sector_expr : NULL;
        edl_error_t werr = fh_write_sectors_program(ctx, wpart.lun,
            wpart.start_sector + sector_offset, start_expr, buf, this_chunk_bytes, wpart.name);
        if (werr != EDL_OK) {
            free(buf);
            free(gpt_preload);
            if (fp)
                fclose(fp);
            return werr;
        }

        sector_offset += this_chunk_sectors;
        total_written += this_chunk_bytes;

        fh_report_progress(ctx,
                           total_written > progress_total_bytes ? progress_total_bytes : total_written,
                           progress_total_bytes);
    }

    free(buf);
    free(gpt_preload);
    if (fp)
        fclose(fp);
    fh_log_detail(ctx, "写入完成: %s", wpart.name);
    return EDL_OK;
}

edl_error_t edl_firehose_erase_partition(edl_firehose_t *ctx,
                                          const edl_partition_info_t *part)
{
    if (!ctx || !part) return EDL_ERR_INVALID_PARAM;
    if (part->num_sectors <= 0) {
        fh_log(ctx, "擦除 %s：num_sectors 无效（<=0），已中止", part->name);
        return EDL_ERR_INVALID_PARAM;
    }

    fh_log_detail(ctx, "正在擦除 %s...", part->name);

    int64_t total_bytes = 0;
    if (ctx->sector_size > 0
        && part->num_sectors > 0
        && part->num_sectors <= INT64_MAX / (int64_t)ctx->sector_size) {
        total_bytes = part->num_sectors * (int64_t)ctx->sector_size;
    }
    if (total_bytes > 0)
        fh_report_progress(ctx, 0, total_bytes);

    char xml[FH_XML_BUF_SIZE];
    edl_xml_build_erase(xml, sizeof(xml), ctx->sector_size, part->lun,
                         part->start_sector, part->num_sectors);

    edl_error_t err = fh_send_and_wait_ack(ctx, xml, FH_ACK_TIMEOUT_MS);
    if (err == EDL_OK)
        fh_log_detail(ctx, "擦除成功: %s", part->name);
    if (err == EDL_OK && total_bytes > 0)
        fh_report_progress(ctx, total_bytes, total_bytes);
    return err;
}

edl_error_t edl_firehose_write_partition_mem(edl_firehose_t *ctx,
                                              const edl_partition_info_t *part,
                                              const uint8_t *data, int data_len)
{
    if (!ctx || !part || !data || data_len <= 0) return EDL_ERR_INVALID_PARAM;

    edl_error_t se = fh_require_part_sector_matches_session(ctx, part);
    if (se != EDL_OK)
        return se;

    edl_partition_info_t wpart = *part;
    fh_apply_gpt_capture_num_sectors(ctx, &wpart);

    int ss = ctx->sector_size;
    if (ss <= 0) return EDL_ERR_INVALID_PARAM;

    int64_t num_sectors;
    int64_t total_bytes;
    if (fh_is_primary_or_backup_gpt_name(wpart.name)) {
        num_sectors = wpart.num_sectors;
        total_bytes = num_sectors * (int64_t)ss;
        if (num_sectors <= 0) {
            fh_log(ctx, "%s: num_sectors 无效", wpart.name);
            return EDL_ERR_INVALID_PARAM;
        }
    } else {
        num_sectors = ((int64_t)data_len + ss - 1) / (int64_t)ss;
        total_bytes = num_sectors * (int64_t)ss;
    }

    if (!fh_sector_range_valid(wpart.start_sector, num_sectors))
        return EDL_ERR_INVALID_PARAM;
    if (total_bytes > INT_MAX)
        return EDL_ERR_INVALID_PARAM;

    uint8_t *heap_buf = NULL;
    uint8_t *backup_merge_buf = NULL;
    const uint8_t *payload = data;
    int send_len = data_len;

    /*
     * PrimaryGPT/BackupGPT：禁止用 0 补足短镜像。
     * BackupGPT：若镜像恰少 1 扇区（常见 20480=5×4096），从设备读回尾扇区合并后再写。
     */
    if (fh_is_primary_or_backup_gpt_name(wpart.name)) {
        int std_sec = edl_gpt_firehose_gpt_region_sectors(ss);
        if (fh_is_backup_gpt_name(wpart.name) &&
            (int64_t)data_len == total_bytes - ss &&
            num_sectors == (int64_t)std_sec && !wpart.start_sector_expr[0]) {
            send_len = (int)total_bytes;
            uint8_t *dev_buf = NULL;
            int dev_len = 0;
            edl_error_t rerr = edl_firehose_read_sectors(ctx, wpart.lun, wpart.start_sector,
                                                         (int)num_sectors, &dev_buf, &dev_len);
            if (rerr != EDL_OK || !dev_buf || dev_len < send_len) {
                free(dev_buf);
                fh_log(ctx, "%s: 无法从设备读回当前备份区以合并尾扇区（%s），请提供完整镜像",
                       wpart.name, edl_error_str(rerr));
                return rerr != EDL_OK ? rerr : EDL_ERR_INVALID_PARAM;
            }
            backup_merge_buf = (uint8_t *)malloc((size_t)send_len);
            if (!backup_merge_buf) {
                free(dev_buf);
                return EDL_ERR_NO_MEMORY;
            }
            memcpy(backup_merge_buf, dev_buf, (size_t)send_len);
            free(dev_buf);
            memcpy(backup_merge_buf, data, (size_t)(total_bytes - ss));
            fh_log_detail(ctx, "%s: 镜像仅 %d 字节（少 1 扇区），已从设备合并尾扇区后写入",
                            wpart.name, data_len);
            payload = backup_merge_buf;
            if (edl_gpt_update_backup_region_crcs(backup_merge_buf, send_len, ss, wpart.start_sector) == 0)
                fh_log_detail(ctx, "%s: 已重算备份 GPT 头与分区条目表 CRC32（写入前）", wpart.name);
            else
            fh_log_detail(ctx, "%s: 未重算备份 CRC（起始扇区为表达式或镜像布局非标准）",
                              wpart.name);
        } else if ((int64_t)data_len < total_bytes) {
            fh_log(ctx, "%s: 镜像仅 %d 字节，必须至少完整 GPT 区域 %lld 字节（扇区大小 %d × %lld 扇区），"
                      " 或为 BackupGPT 恰少 1 扇区且起始扇区为数值以便从设备合并。",
                   wpart.name, data_len, (long long)total_bytes, ss, (long long)num_sectors);
            return EDL_ERR_INVALID_PARAM;
        } else {
            send_len = (int)total_bytes;
            if ((int64_t)data_len > total_bytes)
                fh_log_detail(ctx, "%s: 镜像长于 GPT 区域，仅写入前 %d 字节", wpart.name, send_len);
        }
    } else if ((int64_t)data_len < total_bytes) {
        heap_buf = (uint8_t *)calloc(1, (size_t)total_bytes);
        if (!heap_buf) return EDL_ERR_NO_MEMORY;
        memcpy(heap_buf, data, (size_t)data_len);
        payload = heap_buf;
        send_len = (int)total_bytes;
        fh_log_detail(ctx, "%s: 按抓包标准将缓冲区补齐至 %lld 字节（原 %d）",
                      wpart.name, (long long)total_bytes, data_len);
    }

    /* PrimaryGPT / 完整 BackupGPT：写入前重算 CRC（合并路径已在上方处理备份 CRC） */
    uint8_t *gpt_crc_copy = NULL;
    if (fh_is_primary_gpt_name(wpart.name)) {
        gpt_crc_copy = (uint8_t *)malloc((size_t)send_len);
        if (!gpt_crc_copy) {
            free(heap_buf);
            free(backup_merge_buf);
            return EDL_ERR_NO_MEMORY;
        }
        memcpy(gpt_crc_copy, heap_buf ? heap_buf : data, (size_t)send_len);
        if (edl_gpt_update_primary_crcs(gpt_crc_copy, send_len, ss) == 0)
            fh_log_detail(ctx, "%s: 已重算 GPT 头与分区条目表 CRC32（写入前）", wpart.name);
        else
            fh_log_detail(ctx, "%s: 未重算 CRC（可能不是标准主 GPT 布局）", wpart.name);
        payload = gpt_crc_copy;
    } else if (fh_is_backup_gpt_name(wpart.name) && !backup_merge_buf) {
        gpt_crc_copy = (uint8_t *)malloc((size_t)send_len);
        if (!gpt_crc_copy) {
            free(heap_buf);
            free(backup_merge_buf);
            return EDL_ERR_NO_MEMORY;
        }
        memcpy(gpt_crc_copy, heap_buf ? heap_buf : data, (size_t)send_len);
        if (edl_gpt_update_backup_region_crcs(gpt_crc_copy, send_len, ss, wpart.start_sector) == 0)
            fh_log_detail(ctx, "%s: 已重算备份 GPT 头与分区条目表 CRC32（写入前）", wpart.name);
        else
            fh_log_detail(ctx, "%s: 未重算备份 CRC（可能不是标准备份区布局）", wpart.name);
        payload = gpt_crc_copy;
    }

    int chunk_size = ctx->max_payload_size;
    int chunk_sectors = chunk_size / ss;
    if (chunk_size < ss || chunk_sectors <= 0) {
        free(heap_buf);
        free(backup_merge_buf);
        free(gpt_crc_copy);
        return EDL_ERR_INVALID_PARAM;
    }

    uint8_t *chunk = (uint8_t *)malloc((size_t)chunk_size);
    if (!chunk) {
        free(heap_buf);
        free(backup_merge_buf);
        free(gpt_crc_copy);
        return EDL_ERR_NO_MEMORY;
    }

    int64_t sector_offset = 0;
    int64_t user_off = 0;
    const char *label = wpart.name[0] ? wpart.name : "Partition";

    while (sector_offset < num_sectors) {
        if (fh_is_cancelled(ctx)) {
            free(chunk);
            free(heap_buf);
            free(backup_merge_buf);
            free(gpt_crc_copy);
            fh_log_detail(ctx, "写入 %s（mem）已取消", wpart.name);
            return EDL_ERR_CANCELLED;
        }

        int64_t remaining = num_sectors - sector_offset;
        int this_chunk_sectors = (int)(remaining > chunk_sectors ? chunk_sectors : remaining);
        int this_chunk_bytes = this_chunk_sectors * ss;

        int copy_len = send_len - (int)user_off;
        if (copy_len > this_chunk_bytes) copy_len = this_chunk_bytes;
        if (copy_len < 0) copy_len = 0;
        if (copy_len < this_chunk_bytes)
            memset(chunk, 0, (size_t)this_chunk_bytes);
        if (copy_len > 0)
            memcpy(chunk, payload + user_off, (size_t)copy_len);

        const char *start_expr = (sector_offset == 0 && wpart.start_sector_expr[0]) ? wpart.start_sector_expr : NULL;
        edl_error_t werr = fh_write_sectors_program(ctx, wpart.lun,
            wpart.start_sector + sector_offset, start_expr, chunk, this_chunk_bytes, label);
        if (werr != EDL_OK) {
            free(chunk);
            free(heap_buf);
            free(backup_merge_buf);
            free(gpt_crc_copy);
            return werr;
        }

        sector_offset += this_chunk_sectors;
        user_off += copy_len;

        {
            int64_t done = user_off;
            if (done > send_len) done = send_len;
            fh_report_progress(ctx, done, send_len);
        }
    }
    free(chunk);
    free(heap_buf);
    free(backup_merge_buf);
    free(gpt_crc_copy);
    return EDL_OK;
}

edl_error_t edl_firehose_read_partition_mem(edl_firehose_t *ctx,
                                             const edl_partition_info_t *part,
                                             uint8_t **out_data, int *out_len)
{
    if (!ctx || !part || !out_data || !out_len) return EDL_ERR_INVALID_PARAM;
    if (part->num_sectors <= 0 || !fh_sector_range_valid(part->start_sector, part->num_sectors))
        return EDL_ERR_INVALID_PARAM;
    if (ctx->sector_size > 0 && part->num_sectors > INT64_MAX / (int64_t)ctx->sector_size)
        return EDL_ERR_INVALID_PARAM;

    int64_t tb64 = part->num_sectors * (int64_t)ctx->sector_size;
    if (tb64 > INT_MAX || tb64 < 1)
        return EDL_ERR_INVALID_PARAM;
    int total_bytes = (int)tb64;
    *out_data = (uint8_t *)malloc(total_bytes);
    if (!*out_data) return EDL_ERR_NO_MEMORY;

    int64_t remaining_sectors = part->num_sectors;
    int64_t current_sector = part->start_sector;
    int max_sectors_per_read = ctx->max_payload_size / ctx->sector_size;
    int received = 0;
    if (max_sectors_per_read <= 0) {
        free(*out_data);
        *out_data = NULL;
        return EDL_ERR_INVALID_PARAM;
    }

    fh_log_detail(ctx, "正在读取 %s 到内存 (%lld 扇区)...", part->name, (long long)part->num_sectors);

    while (remaining_sectors > 0) {
        if (fh_is_cancelled(ctx)) {
            free(*out_data);
            *out_data = NULL;
            *out_len = 0;
            fh_log_detail(ctx, "读取 %s（mem）已取消", part->name);
            return EDL_ERR_CANCELLED;
        }

        int chunk = (int)(remaining_sectors > max_sectors_per_read ? max_sectors_per_read : remaining_sectors);

        int chunk_len = 0;
        edl_error_t err = fh_read_sectors_ex(ctx, part->lun, current_sector, chunk, NULL,
                                             *out_data + received, total_bytes - received, &chunk_len);
        if (err != EDL_OK) {
            free(*out_data);
            *out_data = NULL;
            *out_len = 0;
            return err;
        }

        if (chunk_len < 0 || received + chunk_len > total_bytes) {
            free(*out_data);
            *out_data = NULL;
            *out_len = 0;
            fh_log_detail(ctx, "read_partition_mem: 单包长度异常 (%d)，已读 %d / 预期 %d",
                          chunk_len, received, total_bytes);
            return EDL_ERR_FH_READ;
        }
        received += chunk_len;
        current_sector += chunk;
        remaining_sectors -= chunk;

        fh_report_progress(ctx, received, total_bytes);
    }

    *out_len = received;
    fh_log_detail(ctx, "读取完成: %s (%d 字节)", part->name, received);
    return EDL_OK;
}

edl_error_t edl_firehose_apply_patch(edl_firehose_t *ctx, int lun,
                                      int64_t start_sector, int byte_offset,
                                      int size_in_bytes, const char *value)
{
    if (!ctx) return EDL_ERR_INVALID_PARAM;
    char xml[FH_XML_BUF_SIZE];
    edl_xml_build_patch(xml, sizeof(xml), ctx->sector_size, lun, start_sector,
                         byte_offset, size_in_bytes, value);
    return fh_send_and_wait_ack(ctx, xml, 5000);
}

/* fixgpt 部分机型较慢；lun=all 在 OPPO/部分 UFS 上可能 NAK，需回退按 LUN 或变体属性 */
#define FH_FIXGPT_TIMEOUT_MS 30000

static edl_error_t fh_fix_gpt_send_tag(edl_firehose_t *ctx, const char *fixgpt_inner_tag)
{
    char xml[512];
    int n = snprintf(xml, sizeof(xml), "<?xml version=\"1.0\" ?><data>%s</data>", fixgpt_inner_tag);
    if (n < 0 || n >= (int)sizeof(xml))
        return EDL_ERR_INVALID_PARAM;
    edl_port_purge(ctx->port);
    edl_sleep_ms(25);
    return fh_send_and_wait_ack_ex(ctx, xml, FH_FIXGPT_TIMEOUT_MS, 0);
}

static void fh_try_set_bootable_lun(edl_firehose_t *ctx, int lun)
{
    if (!ctx || lun < 0 || lun > 255)
        return;
    char xb[256];
    int n = edl_xml_build_setbootablestoragedrive(xb, sizeof(xb), lun);
    if (n <= 0 || n >= (int)sizeof(xb))
        return;
    edl_port_purge(ctx->port);
    edl_sleep_ms(30);
    edl_error_t e = fh_send_and_wait_ack_ex(ctx, xb, 8000, 0);
    /* 常规 UFS 激活会多次调用，勿刷屏主日志；需要时开「详细日志」 */
    if (e == EDL_OK)
        fh_log_detail(ctx, "setbootablestoragedrive(%d) 已确认（可启动存储路径）", lun);
    else
        fh_log_detail(ctx, "setbootablestoragedrive(%d) 未确认（部分机型不支持或已忽略）", lun);
}

void edl_firehose_try_set_bootable_storage_drive(edl_firehose_t *ctx, int lun)
{
    fh_try_set_bootable_lun(ctx, lun);
}

edl_error_t edl_firehose_fix_gpt(edl_firehose_t *ctx, int lun, bool grow_last_partition)
{
    if (!ctx) return EDL_ERR_INVALID_PARAM;

    if (lun >= 0) {
        char tag[160];
        snprintf(tag, sizeof(tag), "<fixgpt lun=\"%d\" grow_last_partition=\"%d\" />",
                 lun, grow_last_partition ? 1 : 0);
        fh_log_detail(ctx, "修复 GPT（主备同步 + CRC，lun=%d）...", lun);
        edl_error_t err = fh_fix_gpt_send_tag(ctx, tag);
        if (err == EDL_OK)
            fh_log_detail(ctx, "GPT 修复成功");
        else
            fh_log(ctx, "GPT 修复失败");
        return err;
    }

    /* lun=all：多策略 + 按 LUN 回退（与部分 OEM loader 兼容）
     * 注意：勿在此处抢先 setbootablestoragedrive(0)。UFS 上可启动 LUN 常为 1/2（槽位）或与机型相关；
     * 强行 LUN0 可能导致刷机后无法开机；与 SakuraEDL「无 A/B 则不发 setbootable」一致。
     * 若需激活，由上层在回读 GPT 后调用 edl_service_activate_boot_lun_sakura 或用户勾选流程。 */
    fh_log_detail(ctx, "修复 GPT（主备同步 + CRC，lun=all）...");

    static const char *const all_tries[] = {
        "<fixgpt lun=\"all\" grow_last_partition=\"1\" />",
        "<fixgpt lun=\"all\" grow_last_partition=\"0\" />",
        "<fixgpt lun=\"all\" />",
        "<fixgpt />",
    };
    (void)grow_last_partition;

    for (size_t ti = 0; ti < sizeof(all_tries) / sizeof(all_tries[0]); ti++) {
        if (fh_is_cancelled(ctx)) return EDL_ERR_CANCELLED;
        edl_error_t err = fh_fix_gpt_send_tag(ctx, all_tries[ti]);
        if (err == EDL_OK) {
            fh_log_detail(ctx, "GPT 修复成功（%s）", all_tries[ti]);
            return EDL_OK;
        }
        fh_log_detail(ctx, "fixgpt 尝试失败: %s", all_tries[ti]);
    }

    fh_log_detail(ctx, "fixgpt lun=all 不可用，按 LUN 逐次修复（UFS 常见 LUN0–5）…");
    int max_lun = 6;
#ifdef _WIN32
    if (_stricmp(ctx->storage_type, "emmc") == 0) max_lun = 1;
#else
    if (strcasecmp(ctx->storage_type, "emmc") == 0) max_lun = 1;
#endif

    int ok_count = 0;
    edl_error_t last_err = EDL_ERR_FH_NAK;
    for (int L = 0; L < max_lun; L++) {
        if (fh_is_cancelled(ctx)) return EDL_ERR_CANCELLED;
        {
            char tag_min[96];
            snprintf(tag_min, sizeof(tag_min), "<fixgpt lun=\"%d\" />", L);
            edl_error_t err = fh_fix_gpt_send_tag(ctx, tag_min);
            if (err == EDL_OK) {
                fh_log_detail(ctx, "GPT 修复成功（lun=%d，无 grow 属性）", L);
                ok_count++;
                continue;
            }
            last_err = err;
        }
        for (int g = 0; g <= 1; g++) {
            char tag[160];
            snprintf(tag, sizeof(tag), "<fixgpt lun=\"%d\" grow_last_partition=\"%d\" />", L, g);
            edl_error_t err = fh_fix_gpt_send_tag(ctx, tag);
            if (err == EDL_OK) {
                fh_log_detail(ctx, "GPT 修复成功（lun=%d grow=%d）", L, g);
                ok_count++;
                break;
            }
            last_err = err;
            fh_log_detail(ctx, "fixgpt lun=%d grow=%d: %s", L, g, edl_error_str(err));
        }
    }

    if (ok_count > 0) {
        fh_log_detail(ctx, "GPT 修复完成（已对 %d 个 LUN 成功执行 fixgpt）", ok_count);
        return EDL_OK;
    }

    fh_log(ctx, "GPT 修复：本 Firehose 可能不支持 fixgpt 命令（Realme/OPPO 常见），"
                "分区数据已写入；若启动异常可换官方 Programmer 或冷启动 Sahara 后重试");
    return EDL_ERR_FH_FIXGPT_UNSUPPORTED;
}

edl_error_t edl_firehose_reboot(edl_firehose_t *ctx, const char *mode)
{
    if (!ctx) return EDL_ERR_INVALID_PARAM;
    fh_log_detail(ctx, "正在重启 (%s)...", mode ? mode : "reset");
    char xml[FH_XML_BUF_SIZE];
    edl_xml_build_power(xml, sizeof(xml), mode ? mode : "reset");
    fh_send_xml(ctx, xml);
    /* Don't wait for ACK - device may reboot immediately */
    edl_sleep_ms(500);
    return EDL_OK;
}

edl_error_t edl_firehose_send_xml(edl_firehose_t *ctx, const char *xml,
                                   char *response_buf, size_t response_buf_size)
{
    if (!ctx || !xml) return EDL_ERR_INVALID_PARAM;
    fh_send_xml(ctx, xml);

    edl_xml_response_t resp;
    edl_error_t wait_err = fh_wait_response(ctx, &resp, FH_ACK_TIMEOUT_MS);
    if (wait_err == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (wait_err == EDL_OK) {
        if (response_buf && response_buf_size > 0)
            snprintf(response_buf, response_buf_size, "%s", resp.raw_value);
        return resp.is_ack ? EDL_OK : EDL_ERR_FH_NAK;
    }
    return EDL_ERR_TIMEOUT;
}

int edl_firehose_sector_size(const edl_firehose_t *ctx)
{
    return ctx ? ctx->sector_size : 4096;
}

int edl_firehose_max_payload(const edl_firehose_t *ctx)
{
    return ctx ? ctx->max_payload_size : FH_DEFAULT_PAYLOAD;
}

const char *edl_firehose_storage_type(const edl_firehose_t *ctx)
{
    return ctx ? ctx->storage_type : "ufs";
}

int edl_firehose_max_lun_hint(const edl_firehose_t *ctx)
{
    if (!ctx)
        return 0;
    if (ctx->reported_lun_enable_mask != 0)
        return fh_lun_span_from_mask(ctx->reported_lun_enable_mask);
    return ctx->reported_lun_count > 0 ? ctx->reported_lun_count : 0;
}

uint32_t edl_firehose_msm_hwid_hint(const edl_firehose_t *ctx)
{
    return ctx ? ctx->msm_hwid_hint : 0;
}
