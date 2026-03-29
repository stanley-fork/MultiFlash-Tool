#include "edl/storage_report.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_LOG_LINES   160
#define LINE_CAP        640

typedef struct {
    char product_model[128];
    char vendor[128];
    char vendor_id[48];
    char storage_type[32];
    char firmware[48];
    char serial_hex[48];
    char serial_dec_str[32];
    uint64_t total_logical_blocks;
    int      has_total_blocks;
    uint32_t block_size_bytes;
    int      has_block_size;
    int      lun_count;
    int      has_lun_count;
    uint32_t lun_enable_mask;
    int      has_lun_enable_mask;
    uint64_t erase_bytes;
    int      has_erase;
    uint64_t erase_sectors;
    char     write_protect[32];
    char     config_lock[32];
    char     boot_part[32];
    char     lu_size[8][32];
    int      has_lu_size[8];
} parsed_t;

static int line_has_ci(const char *line, const char *sub);

static void copy_trimmed_value(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    while (*src == ' ' || *src == '\t') src++;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' ||
                     src[n - 1] == '\r' || src[n - 1] == '\n'))
        n--;
    if (n >= cap)
        n = cap - 1;
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static int parse_lu_size_index(const char *line)
{
    if (!line) return -1;
    for (const char *p = line; *p; p++) {
        if ((p[0] == 'L' || p[0] == 'l') &&
            (p[1] == 'U' || p[1] == 'u' || p[1] == 'N' || p[1] == 'n') &&
            isdigit((unsigned char)p[2])) {
            int idx = p[2] - '0';
            if (idx < 0 || idx >= 8)
                return -1;
            if (line_has_ci(p, "Size"))
                return idx;
        }
    }
    return -1;
}

static int append_report_line(char *report, size_t report_size, int w,
                              const char *label, const char *value)
{
    if (!report || !label || !value || w < 0 || (size_t)w >= report_size)
        return w;
    return w + snprintf(report + w, report_size - (size_t)w, "%-16s: %s\n", label, value);
}

static const char *json_field_after_colon(const char *line, const char *key)
{
    if (!line || !key)
        return NULL;

    const size_t key_len = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + key_len;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != ':') {
            p += key_len;
            continue;
        }
        q++;
        while (*q == ' ' || *q == '\t')
            q++;
        return q;
    }

    return NULL;
}

static void copy_json_quoted_field(const char *line, const char *key, char *dst, size_t cap)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!line || !key) return;
    const char *p = json_field_after_colon(line, key);
    if (!p) return;
    size_t i = 0;
    if (*p == '"')
        p++;
    while (*p && *p != '"' && i < cap - 1)
        dst[i++] = (char)*p++;
    dst[i] = '\0';
}

/* JSON 鐗囨褰㈠ "total_blocks":14606336 鎴?"serial_num":2037279096锛堥敭宸插惈鍐掑彿鍒欏€肩揣璺熸暟瀛楋級 */
static int json_u64_field(const char *line, const char *key, uint64_t *out)
{
    if (!line || !key || !out) return 0;
    const char *p = json_field_after_colon(line, key);
    if (!p) return 0;
    *out = (uint64_t)strtoull(p, NULL, 0);
    return 1;
}

/* Firehose 甯稿湪鍗曡 JSON 涓粰鍑?storage_info锛堟潈濞佸瓧娈碉級 */
static void overlay_storage_info_json(const char *line, parsed_t *o)
{
    if (!line || !o || !strstr(line, "storage_info"))
        return;

    char q[128];
    copy_json_quoted_field(line, "\"mem_type\"", q, sizeof(q));
    if (q[0]) {
        for (size_t i = 0; q[i]; i++)
            q[i] = (char)tolower((unsigned char)q[i]);
        if (strcmp(q, "ufs") == 0)
            snprintf(o->storage_type, sizeof(o->storage_type), "UFS");
        else if (strcmp(q, "emmc") == 0)
            snprintf(o->storage_type, sizeof(o->storage_type), "eMMC");
        else
            snprintf(o->storage_type, sizeof(o->storage_type), "%s", q);
    }

    copy_json_quoted_field(line, "\"fw_version\"", o->firmware, sizeof(o->firmware));
    if (!o->firmware[0])
        copy_json_quoted_field(line, "\"fw_rev\"", o->firmware, sizeof(o->firmware));
    if (!o->firmware[0])
        copy_json_quoted_field(line, "\"firmware_version\"", o->firmware, sizeof(o->firmware));

    copy_json_quoted_field(line, "\"prod_name\"", o->product_model, sizeof(o->product_model));
    if (!o->product_model[0])
        copy_json_quoted_field(line, "\"product_name\"", o->product_model, sizeof(o->product_model));
    if (!o->product_model[0])
        copy_json_quoted_field(line, "\"product_model\"", o->product_model, sizeof(o->product_model));

    uint64_t u = 0;
    if ((json_u64_field(line, "\"total_blocks\"", &u)
            || json_u64_field(line, "\"total_logical_blocks\"", &u))
        && u > 0) {
        o->total_logical_blocks = u;
        o->has_total_blocks = 1;
    }
    if ((json_u64_field(line, "\"block_size\"", &u)
            || json_u64_field(line, "\"block_size_bytes\"", &u))
        && u > 0) {
        o->block_size_bytes = (uint32_t)u;
        o->has_block_size = 1;
    }
    if (json_u64_field(line, "\"serial_num\"", &u) || json_u64_field(line, "\"serial\"", &u)) {
        snprintf(o->serial_hex, sizeof(o->serial_hex), "0x%llx", (unsigned long long)u);
        snprintf(o->serial_dec_str, sizeof(o->serial_dec_str), "%llu",
                 (unsigned long long)u);
    }
    if ((json_u64_field(line, "\"num_physical\"", &u)
            || json_u64_field(line, "\"lun_count\"", &u)
            || json_u64_field(line, "\"physical_count\"", &u))
        && u > 0 && u <= 64) {
        o->lun_count = (int)u;
        o->has_lun_count = 1;
    }
    if (json_u64_field(line, "\"manufacturer_id\"", &u)
        || json_u64_field(line, "\"manufacturerid\"", &u)) {
        snprintf(o->vendor_id, sizeof(o->vendor_id), "0x%lx", (unsigned long)u);
    }
}

static void trim_crlf(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' '))
        s[--n] = '\0';
}

static int line_has_ci(const char *line, const char *sub)
{
    if (!line || !sub) return 0;
    size_t sl = strlen(sub);
    for (size_t i = 0; line[i]; i++) {
        size_t j = 0;
        for (; j < sl; j++) {
            unsigned char c1 = (unsigned char)line[i + j];
            unsigned char c2 = (unsigned char)sub[j];
            if (!c1) return 0;
            if (tolower(c1) != tolower(c2)) break;
        }
        if (j == sl) return 1;
    }
    return 0;
}

/* UFS 鎻忚堪绗︺€孲tring Index銆嶈锛氬嬁褰撲綔鍨嬪彿/搴忓垪鍙?鍘傚晢鍚?*/
static int is_ufs_string_index_line(const char *L)
{
    if (!L) return 0;
    return line_has_ci(L, "String Index") || line_has_ci(L, "StringIndex");
}

static const char *after_colon(const char *line)
{
    const char *p = strchr(line, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static uint64_t parse_hex_u64(const char *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint64_t)strtoull(s, NULL, 16);
    return (uint64_t)strtoull(s, NULL, 0);
}

/* 浠呮彁鍙?<log ... value="..."/> 涓殑鏂囨湰锛岄伩鍏嶈鎶?<response value="ACK"/> */
static void extract_log_values(const char *xml, char lines[][LINE_CAP], int *n_lines)
{
    *n_lines = 0;
    const char *p = xml;
    while (p && *p && *n_lines < MAX_LOG_LINES) {
        p = strstr(p, "<log");
        if (!p) break;
        const char *end_tag = strstr(p, "/>");
        if (!end_tag) break;
        const char *vq = strstr(p, "value=\"");
        if (!vq || vq > end_tag) {
            p = p + 4;
            continue;
        }
        vq += 7;
        char *dst = lines[*n_lines];
        size_t i = 0;
        while (*vq && *vq != '"' && i < LINE_CAP - 1) {
            if (strncmp(vq, "&amp;", 5) == 0) {
                dst[i++] = '&';
                vq += 5;
            } else if (strncmp(vq, "&lt;", 4) == 0) {
                dst[i++] = '<';
                vq += 4;
            } else if (strncmp(vq, "&gt;", 4) == 0) {
                dst[i++] = '>';
                vq += 4;
            } else if (strncmp(vq, "&quot;", 6) == 0) {
                dst[i++] = '"';
                vq += 6;
            } else
                dst[i++] = (char)*vq++;
        }
        dst[i] = '\0';
        trim_crlf(dst);
        if (i > 0)
            (*n_lines)++;
        p = end_tag + 2;
    }
}

static void split_plain_lines(const char *text, char lines[][LINE_CAP], int *n_lines)
{
    const char *start = text;

    if (!lines || !n_lines)
        return;

    *n_lines = 0;
    if (!text || !text[0])
        return;

    for (const char *p = text; ; p++) {
        const int at_end = (*p == '\0');
        const int at_line_break = (*p == '\r' || *p == '\n');
        const int at_pipe_break =
            (p[0] == ' ' && p[1] != '\0' && p[1] == '|'
             && p[2] != '\0' && p[2] == ' ');
        if (!at_end && !at_line_break && !at_pipe_break)
            continue;

        if (*n_lines < MAX_LOG_LINES) {
            char tmp[LINE_CAP];
            size_t len = (size_t)(p - start);
            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            if (len > 0)
                memcpy(tmp, start, len);
            tmp[len] = '\0';
            copy_trimmed_value(lines[*n_lines], LINE_CAP, tmp);
            if (lines[*n_lines][0])
                (*n_lines)++;
        }

        if (at_end || *n_lines >= MAX_LOG_LINES)
            break;

        if (at_line_break && p[0] == '\r' && p[1] == '\n')
            p++;
        else if (at_pipe_break)
            p += 2;
        start = p + 1;
    }
}

static void parse_lines(char lines[][LINE_CAP], int n_lines, parsed_t *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < n_lines; i++) {
        const char *L = lines[i];
        if (!L[0]) continue;

        if (line_has_ci(L, "INFO:")) {
            const char *q = strchr(L, ':');
            if (q) {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                L = q;
            }
        }

        if (line_has_ci(L, "Storage type") && line_has_ci(L, "UFS"))
            snprintf(o->storage_type, sizeof(o->storage_type), "UFS");
        else if (line_has_ci(L, "Storage type") && line_has_ci(L, "eMMC"))
            snprintf(o->storage_type, sizeof(o->storage_type), "eMMC");
        else if (line_has_ci(L, " set to value UFS") || line_has_ci(L, "value UFS"))
            snprintf(o->storage_type, sizeof(o->storage_type), "UFS");
        else if (strstr(L, "\"mem_type\":\"UFS\"") || strstr(L, "\"mem_type\":\"ufs\""))
            snprintf(o->storage_type, sizeof(o->storage_type), "UFS");
        else if (strstr(L, "\"mem_type\":\"eMMC\"") || strstr(L, "\"mem_type\":\"emmc\""))
            snprintf(o->storage_type, sizeof(o->storage_type), "eMMC");

        if (line_has_ci(L, "Inquiry Command Output")) {
            const char *v = after_colon(L);
            if (v) {
                char first[96];
                if (sscanf(v, "%95s", first) == 1)
                    snprintf(o->vendor, sizeof(o->vendor), "%s", first);
            }
        }

        if ((line_has_ci(L, "Manufacturer") || line_has_ci(L, "Vendor")) &&
            !line_has_ci(L, "ID") && !is_ufs_string_index_line(L)) {
            const char *v = after_colon(L);
            if (v) snprintf(o->vendor, sizeof(o->vendor), "%s", v);
        }
        if (!is_ufs_string_index_line(L) &&
            (line_has_ci(L, "Manufacturer ID") || line_has_ci(L, "OEM ID") ||
             line_has_ci(L, "Device Manufacturer ID") ||
             line_has_ci(L, "wManufacturerID"))) {
            const char *hx = strstr(L, "0x");
            if (!hx) hx = strstr(L, "0X");
            if (hx) snprintf(o->vendor_id, sizeof(o->vendor_id), "%s", hx);
        }

        /* 鍕夸粠鏁存 storage_info JSON 琛岀敤鍐掑彿鍚彂寮忓彇銆屽瀷鍙枫€嶏紝鏄撹鎶撲负 }} 绛?*/
        if (strstr(L, "\"storage_info\"") || strstr(L, "\"prod_name\""))
            ;
        else if ((line_has_ci(L, "Product") || line_has_ci(L, "Model")) &&
                 !line_has_ci(L, "Logical") && !line_has_ci(L, "Block") &&
                 !is_ufs_string_index_line(L)) {
            const char *v = after_colon(L);
            if (v) snprintf(o->product_model, sizeof(o->product_model), "%s", v);
        }

        if (!is_ufs_string_index_line(L) &&
            (line_has_ci(L, "Firmware") || line_has_ci(L, "FW Revision") || line_has_ci(L, "FW version"))) {
            const char *v = after_colon(L);
            if (v) snprintf(o->firmware, sizeof(o->firmware), "%s", v);
        }

        if (line_has_ci(L, "Serial") && !is_ufs_string_index_line(L)) {
            const char *v = after_colon(L);
            if (v) {
                snprintf(o->serial_hex, sizeof(o->serial_hex), "%s", v);
                uint64_t dec = parse_hex_u64(v);
                if (dec > 0)
                    snprintf(o->serial_dec_str, sizeof(o->serial_dec_str), "%llu",
                             (unsigned long long)dec);
            }
        }

        if (line_has_ci(L, "Total Logical Blocks") || line_has_ci(L, "Device Total Logical Blocks")) {
            const char *hx = strstr(L, "0x");
            if (!hx) hx = strstr(L, "0X");
            uint64_t b = hx ? parse_hex_u64(hx) : 0;
            if (b == 0) {
                const char *v = after_colon(L);
                if (v) b = parse_hex_u64(v);
            }
            if (b > 0) {
                o->total_logical_blocks = b;
                o->has_total_blocks = 1;
            }
        }

        if (line_has_ci(L, "Block Size in Bytes") || line_has_ci(L, "Device Block Size")) {
            const char *hx = strstr(L, "0x");
            if (!hx) hx = strstr(L, "0X");
            uint32_t bs = hx ? (uint32_t)parse_hex_u64(hx) : 0;
            if (bs == 0) {
                const char *v = after_colon(L);
                if (v) bs = (uint32_t)parse_hex_u64(v);
            }
            if (bs > 0) {
                o->block_size_bytes = bs;
                o->has_block_size = 1;
            }
        }

        if (line_has_ci(L, "Device Total Physical Partitions") ||
            line_has_ci(L, "Total Physical Partitions")) {
            const char *hx = strstr(L, "0x");
            if (!hx) hx = strstr(L, "0X");
            int v = 0;
            if (hx)
                v = (int)parse_hex_u64(hx);
            else {
                const char *ac = after_colon(L);
                if (ac) v = (int)strtol(ac, NULL, 0);
            }
            if (v > 0 && v <= 64) {
                o->lun_count = v;
                o->has_lun_count = 1;
            }
        }
        if (line_has_ci(L, "Physical Partition") && line_has_ci(L, "Count")) {
            const char *v = after_colon(L);
            if (v) {
                o->lun_count = (int)strtol(v, NULL, 0);
                if (o->lun_count > 0) o->has_lun_count = 1;
            }
        }
        if (line_has_ci(L, "Total Active LU") || line_has_ci(L, "UFS Total Active LU")) {
            const char *hx = strstr(L, "0x");
            if (!hx) hx = strstr(L, "0X");
            int v = 0;
            if (hx)
                v = (int)parse_hex_u64(hx);
            else {
                const char *ac = after_colon(L);
                if (ac) v = (int)strtol(ac, NULL, 0);
            }
            if (v > 0 && v <= 64) {
                o->lun_count = v;
                o->has_lun_count = 1;
            }
        }
        if (line_has_ci(L, "Number of LU") || line_has_ci(L, "number of lun")) {
            const char *v = after_colon(L);
            if (v) {
                o->lun_count = (int)strtol(v, NULL, 0);
                if (o->lun_count > 0) o->has_lun_count = 1;
            }
        }
        if (line_has_ci(L, "LUN Enable Bitmask")) {
            const char *hx = strstr(L, "0x");
            if (!hx) hx = strstr(L, "0X");
            if (hx) {
                uint32_t mask = (uint32_t)parse_hex_u64(hx);
                if (mask != 0) {
                    o->lun_enable_mask = mask;
                    o->has_lun_enable_mask = 1;
                }
            }
        }

        {
            int lu_idx = parse_lu_size_index(L);
            if (lu_idx >= 0 && lu_idx < 8) {
                const char *v = after_colon(L);
                if (v && v[0]) {
                    copy_trimmed_value(o->lu_size[lu_idx], sizeof(o->lu_size[lu_idx]), v);
                    if (o->lu_size[lu_idx][0])
                        o->has_lu_size[lu_idx] = 1;
                }
            }
        }

        if (line_has_ci(L, "Erase") && (line_has_ci(L, "block") || line_has_ci(L, "size"))) {
            const char *hx = strstr(L, "0x");
            uint64_t e = hx ? parse_hex_u64(hx) : 0;
            if (e == 0) {
                const char *v = after_colon(L);
                if (v) e = (uint64_t)strtoull(v, NULL, 10);
            }
            if (e > 0) {
                o->erase_bytes = e;
                o->has_erase = 1;
                if (o->block_size_bytes > 0)
                    o->erase_sectors = e / (uint64_t)o->block_size_bytes;
            }
        }

        if (line_has_ci(L, "Write protect") || line_has_ci(L, "WriteProtect")) {
            const char *v = after_colon(L);
            if (v) snprintf(o->write_protect, sizeof(o->write_protect), "%s", v);
        }
        if (line_has_ci(L, "Config") && line_has_ci(L, "lock")) {
            const char *v = after_colon(L);
            if (v) snprintf(o->config_lock, sizeof(o->config_lock), "%s", v);
        }
        if (line_has_ci(L, "Boot") && line_has_ci(L, "partition")) {
            const char *v = after_colon(L);
            if (v) snprintf(o->boot_part, sizeof(o->boot_part), "%s", v);
        }
    }
}

static void fmt_gb(char *out, size_t cap, uint64_t sectors, uint32_t bsize)
{
    if (sectors == 0 || bsize == 0) {
        out[0] = '\0';
        return;
    }
    long double gb = (long double)sectors * (long double)bsize / (1024.0L * 1024.0L * 1024.0L);
    snprintf(out, cap, "%.2f GB", (double)gb);
}

static void parse_storage_info_payload(const char *firehose_rx_xml,
                                       parsed_t *out,
                                       char lines[][LINE_CAP],
                                       int *n_lines)
{
    char local_lines[MAX_LOG_LINES][LINE_CAP];
    int local_n = 0;
    char (*work_lines)[LINE_CAP] = lines;
    int *work_n = n_lines;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    if (!work_lines || !work_n) {
        work_lines = local_lines;
        work_n = &local_n;
    }

    *work_n = 0;
    if (!firehose_rx_xml || !firehose_rx_xml[0])
        return;

    extract_log_values(firehose_rx_xml, work_lines, work_n);
    if (*work_n == 0)
        split_plain_lines(firehose_rx_xml, work_lines, work_n);
    parse_lines(work_lines, *work_n, out);

    if (strstr(firehose_rx_xml, "storage_info"))
        overlay_storage_info_json(firehose_rx_xml, out);
    for (int i = 0; i < *work_n; i++)
        overlay_storage_info_json(work_lines[i], out);

    if (lines && n_lines && work_lines != lines) {
        *n_lines = *work_n;
        for (int i = 0; i < *work_n; i++)
            memcpy(lines[i], work_lines[i], LINE_CAP);
    }
}

static int build_contiguous_lun_mask(int lun_count, uint32_t *mask)
{
    if (!mask || lun_count <= 0 || lun_count > 32)
        return 0;
    if (lun_count == 32)
        *mask = 0xFFFFFFFFu;
    else
        *mask = (1u << (unsigned)lun_count) - 1u;
    return 1;
}

int edl_storage_extract_lun_hints(const char *firehose_rx_xml,
                                  int *lun_count,
                                  uint32_t *lun_enable_mask)
{
    parsed_t p;

    if (lun_count)
        *lun_count = 0;
    if (lun_enable_mask)
        *lun_enable_mask = 0;
    if (!firehose_rx_xml || !firehose_rx_xml[0])
        return 0;

    parse_storage_info_payload(firehose_rx_xml, &p, NULL, NULL);

    if (p.has_lun_enable_mask && p.lun_enable_mask != 0) {
        int count = 0;

        if (lun_enable_mask)
            *lun_enable_mask = p.lun_enable_mask;
        for (int bit = 0; bit < 32; bit++) {
            if (p.lun_enable_mask & (1u << (unsigned)bit))
                count++;
        }
        if (lun_count)
            *lun_count = count;
        return 1;
    }

    if (p.has_lun_count && p.lun_count > 0) {
        if (lun_count)
            *lun_count = p.lun_count;
        if (lun_enable_mask)
            (void)build_contiguous_lun_mask(p.lun_count, lun_enable_mask);
        return 1;
    }

    return 0;
}

int edl_storage_build_device_report(const char *firehose_rx_xml, char *report, size_t report_size)
{
    if (!report || report_size < 128) return -1;
    report[0] = '\0';
    if (!firehose_rx_xml || !firehose_rx_xml[0]) {
        snprintf(report, report_size, "无 Firehose 应答内容");
        return 0;
    }

    char lines[MAX_LOG_LINES][LINE_CAP];
    int n = 0;
    parsed_t p;
    parse_storage_info_payload(firehose_rx_xml, &p, lines, &n);

    char cap_gb[48];
    fmt_gb(cap_gb, sizeof(cap_gb), p.total_logical_blocks, p.block_size_bytes);

    char lun_list[256];
    lun_list[0] = '\0';
    if (p.has_lun_enable_mask && p.lun_enable_mask != 0) {
        size_t pos = 0;
        for (int bit = 0; bit < 32 && pos + 16 < sizeof(lun_list); bit++) {
            if (p.lun_enable_mask & (1u << bit)) {
                if (pos) lun_list[pos++] = ' ';
                pos += (size_t)snprintf(lun_list + pos, sizeof(lun_list) - pos, "LUN%d", bit);
            }
        }
    } else if (p.has_lun_count && p.lun_count > 0 && p.lun_count <= 32) {
        size_t pos = 0;
        for (int k = 0; k < p.lun_count && pos + 8 < sizeof(lun_list); k++) {
            if (k) lun_list[pos++] = ' ';
            pos += (size_t)snprintf(lun_list + pos, sizeof(lun_list) - pos, "LUN%d", k);
        }
    }

    const char *dash = "--";
    int w = 0;
    char tmp[128];

    w = append_report_line(report, report_size, w, "存储类型",
                           p.storage_type[0] ? p.storage_type : dash);
    w = append_report_line(report, report_size, w, "产品名称",
                           p.product_model[0] ? p.product_model : dash);
    w = append_report_line(report, report_size, w, "厂商 ID",
                           p.vendor_id[0] ? p.vendor_id : dash);
    w = append_report_line(report, report_size, w, "厂商",
                           p.vendor[0] ? p.vendor : dash);
    w = append_report_line(report, report_size, w, "固件版本",
                           p.firmware[0] ? p.firmware : dash);

    if (p.serial_hex[0] && p.serial_dec_str[0]) {
        snprintf(tmp, sizeof(tmp), "%s (%s)", p.serial_hex, p.serial_dec_str);
        w = append_report_line(report, report_size, w, "序列号", tmp);
    } else {
        w = append_report_line(report, report_size, w, "序列号",
                               p.serial_hex[0] ? p.serial_hex : dash);
    }

    if (p.has_total_blocks && p.has_block_size)
        w = append_report_line(report, report_size, w, "总容量", cap_gb[0] ? cap_gb : dash);
    else
        w = append_report_line(report, report_size, w, "总容量", dash);

    if (p.has_lun_count) {
        snprintf(tmp, sizeof(tmp), "%d", p.lun_count);
        w = append_report_line(report, report_size, w, "LUN 数量", tmp);
    } else {
        w = append_report_line(report, report_size, w, "LUN 数量", dash);
    }

    for (int i = 0; i < 8; i++) {
        char label[32];
        if (!p.has_lu_size[i])
            continue;
        snprintf(label, sizeof(label), "LU%d 大小", i);
        w = append_report_line(report, report_size, w, label, p.lu_size[i]);
    }

    if (p.has_block_size) {
        snprintf(tmp, sizeof(tmp), "%u 字节", p.block_size_bytes);
        w = append_report_line(report, report_size, w, "扇区大小", tmp);
    }
    if (p.boot_part[0])
        w = append_report_line(report, report_size, w, "启动分区", p.boot_part);
    if (p.has_erase) {
        if (p.block_size_bytes > 0 && p.erase_sectors > 0)
            snprintf(tmp, sizeof(tmp), "%llu 字节 (%llu 扇区)",
                     (unsigned long long)p.erase_bytes,
                     (unsigned long long)p.erase_sectors);
        else
            snprintf(tmp, sizeof(tmp), "%llu 字节", (unsigned long long)p.erase_bytes);
        w = append_report_line(report, report_size, w, "擦除块大小", tmp);
    }
    if (lun_list[0])
        w = append_report_line(report, report_size, w, "启用 LUN", lun_list);
    if (p.write_protect[0])
        w = append_report_line(report, report_size, w, "写保护", p.write_protect);
    if (p.config_lock[0])
        w = append_report_line(report, report_size, w, "配置锁", p.config_lock);

    if (w == 0 || (p.storage_type[0] == '\0' && p.product_model[0] == '\0' &&
                   p.vendor[0] == '\0' && p.vendor_id[0] == '\0' &&
                   p.serial_hex[0] == '\0')) {
        report[0] = '\0';
        for (int i = 0; i < n && (size_t)strlen(report) + 4 < report_size; i++) {
            if (!lines[i][0])
                continue;
            strncat(report, lines[i], report_size - strlen(report) - 1);
            strncat(report, "\n", report_size - strlen(report) - 1);
        }
        if (!report[0])
            snprintf(report, report_size, "未能从 Firehose 日志中解析到可显示的设备信息");
    }

    (void)w;
    return 1;

}
