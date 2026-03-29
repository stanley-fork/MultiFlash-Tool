#include "edl/xml_helper.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

/* ===== Internal Helpers ===== */

static void skip_whitespace(const char **p)
{
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

static int parse_attr_value(const char **p, char *out, size_t out_size)
{
    if (**p != '"' && **p != '\'') return -1;
    char quote = *(*p)++;
    size_t i = 0;
    while (**p && **p != quote && i < out_size - 1) {
        if (**p == '&') {
            if (strncmp(*p, "&amp;", 5) == 0) { out[i++] = '&'; *p += 5; }
            else if (strncmp(*p, "&lt;", 4) == 0) { out[i++] = '<'; *p += 4; }
            else if (strncmp(*p, "&gt;", 4) == 0) { out[i++] = '>'; *p += 4; }
            else if (strncmp(*p, "&quot;", 6) == 0) { out[i++] = '"'; *p += 6; }
            else { out[i++] = **p; (*p)++; }
        } else {
            out[i++] = **p;
            (*p)++;
        }
    }
    out[i] = '\0';
    if (**p == quote) (*p)++;
    return 0;
}

/* ===== Response Parsing ===== */

const char *edl_xml_find_element(const char *buf, const char *tag, const char **end)
{
    if (!buf || !tag) return NULL;
    size_t tag_len = strlen(tag);

    const char *p = buf;
    while ((p = strchr(p, '<')) != NULL) {
        p++;
        skip_whitespace(&p);
        if (strncmp(p, tag, tag_len) == 0 && (isspace((unsigned char)p[tag_len]) || p[tag_len] == '/' || p[tag_len] == '>')) {
            const char *start = p - 1;
            const char *close = strstr(p, "/>");
            if (!close) close = strstr(p, ">");
            if (close) {
                if (end) *end = (*close == '/') ? close + 2 : close + 1;
                return start;
            }
        }
    }
    return NULL;
}

int edl_xml_parse_response(const char *xml, edl_xml_response_t *resp)
{
    if (!xml || !resp) return -1;
    memset(resp, 0, sizeof(*resp));

    const char *p = xml;
    while (*p && *p != '<') p++;
    if (!*p) return -1;
    p++;

    skip_whitespace(&p);

    /* Read tag name */
    int ti = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '/' && *p != '>' && ti < (int)sizeof(resp->tag) - 1)
        resp->tag[ti++] = *p++;
    resp->tag[ti] = '\0';

    /* Parse attributes */
    while (*p && *p != '>' && *p != '/') {
        skip_whitespace(&p);
        if (*p == '/' || *p == '>') break;

        /* Attribute name */
        if (resp->attr_count >= EDL_XML_MAX_ATTRS) break;
        edl_xml_attr_t *a = &resp->attrs[resp->attr_count];
        int ni = 0;
        while (*p && *p != '=' && !isspace((unsigned char)*p) && ni < (int)sizeof(a->name) - 1)
            a->name[ni++] = *p++;
        a->name[ni] = '\0';

        skip_whitespace(&p);
        if (*p != '=') continue;
        p++;
        skip_whitespace(&p);

        if (parse_attr_value(&p, a->value, sizeof(a->value)) == 0) {
            resp->attr_count++;
        }
    }

    /* Set convenience fields */
    const char *val = edl_xml_get_attr(resp, "value");
    if (val) {
        snprintf(resp->raw_value, sizeof(resp->raw_value), "%s", val);
        #ifdef _WIN32
        resp->is_ack = (_stricmp(val, "ACK") == 0);
        resp->is_nak = (_stricmp(val, "NAK") == 0);
        #else
        resp->is_ack = (strcasecmp(val, "ACK") == 0);
        resp->is_nak = (strcasecmp(val, "NAK") == 0);
        #endif
    }

    return 0;
}

const char *edl_xml_get_attr(const edl_xml_response_t *resp, const char *name)
{
    if (!resp || !name) return NULL;
    for (int i = 0; i < resp->attr_count; i++) {
        #ifdef _WIN32
        if (_stricmp(resp->attrs[i].name, name) == 0)
        #else
        if (strcasecmp(resp->attrs[i].name, name) == 0)
        #endif
            return resp->attrs[i].value;
    }
    return NULL;
}

int edl_xml_get_attr_int(const edl_xml_response_t *resp, const char *name, int default_val)
{
    const char *v = edl_xml_get_attr(resp, name);
    if (!v) return default_val;
    return atoi(v);
}

/* ===== XML Building ===== */

int edl_xml_build_configure(char *buf, size_t buf_size,
                             const char *storage_type, int max_payload_size)
{
    /* ZLPAwareHost：与 Bus Hound / 常见 QDLoader 抓包一致（大小写与部分 Loader 兼容） */
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><configure MemoryName=\"%s\" Verbose=\"0\" "
        "AlwaysValidate=\"0\" MaxPayloadSizeToTargetInBytes=\"%d\" "
        "MaxPayloadSizeFromTargetInBytes=\"%d\" "
        "AckRawDataEveryNumPackets=\"0\" ZLPAwareHost=\"1\" "
        "SkipStorageInit=\"0\" /></data>",
        storage_type, max_payload_size, max_payload_size);
}

int edl_xml_build_read(char *buf, size_t buf_size,
                        int sector_size, int lun, int64_t start_sector,
                        int64_t num_sectors, const char *filename,
                        const char *label)
{
    /*
     * Bus Hound / other tools (e.g. 44444444444 capture):
     *   <read SECTOR_SIZE_IN_BYTES="4096" num_partition_sectors="6"
     *        start_sector="0" physical_partition_number="0"/>
     * Attribute order matters for some loaders; omit filename when empty
     * (rawmode read to host — no path on device).
     */
    char lbl_attr[256] = "";
    if (label && label[0])
        snprintf(lbl_attr, sizeof(lbl_attr), " label=\"%s\"", label);
    char fn_attr[768] = "";
    if (filename && filename[0])
        snprintf(fn_attr, sizeof(fn_attr), " filename=\"%s\"", filename);

    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><read SECTOR_SIZE_IN_BYTES=\"%d\" "
        "num_partition_sectors=\"%lld\" start_sector=\"%lld\" "
        "physical_partition_number=\"%d\"%s%s /></data>",
        sector_size,
        (long long)num_sectors, (long long)start_sector,
        lun, lbl_attr, fn_attr);
}

int edl_xml_build_program(char *buf, size_t buf_size,
                           int sector_size, int lun, int64_t start_sector,
                           int64_t num_sectors, const char *filename,
                           int64_t file_sector_offset, bool is_sparse)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><program SECTOR_SIZE_IN_BYTES=\"%d\" "
        "physical_partition_number=\"%d\" start_sector=\"%lld\" "
        "num_partition_sectors=\"%lld\" filename=\"%s\" "
        "file_sector_offset=\"%lld\" Sparse=\"%s\" /></data>",
        sector_size, lun, (long long)start_sector,
        (long long)num_sectors, filename ? filename : "",
        (long long)file_sector_offset, is_sparse ? "true" : "false");
}

static void xml_escape_label(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    if (!in || !out || out_sz < 2) {
        if (out && out_sz) out[0] = '\0';
        return;
    }
    for (; *in && o + 1 < out_sz; in++) {
        if (*in == '&') {
            if (o + 6 >= out_sz) break;
            memcpy(out + o, "&amp;", 5);
            o += 5;
        } else if (*in == '"') {
            if (o + 7 >= out_sz) break;
            memcpy(out + o, "&quot;", 6);
            o += 6;
        } else if (*in == '<' || *in == '>') {
            if (o + 5 >= out_sz) break;
            out[o++] = '_';
        } else
            out[o++] = (char)*in;
    }
    out[o] = '\0';
}

int edl_xml_build_program_label(char *buf, size_t buf_size,
                                 int sector_size, int lun, int64_t start_sector,
                                 int64_t num_sectors, const char *label,
                                 bool read_back_verify, bool include_filename)
{
    char safe[96];
    xml_escape_label(label ? label : "", safe, sizeof(safe));
    const char *rb = read_back_verify ? " read_back_verify=\"true\"" : "";
    /*
     * 属性顺序：与常用抓包 / SakuraEDL 接近（file_sector_offset 在前）。
     * include_filename：SakuraEDL 使用 filename+label 同名；旧行为可仅 label。
     */
    /*
     * 与 SakuraEDL FlashPartitionFromFileAsync 一致：无 file_sector_offset；
     * 属性顺序 num_partition_sectors → physical_partition_number → start_sector → filename → label。
     */
    /*
     * 属性顺序与 Qualcomm rawprogram*.xml / 线刷包一致：
     * file_sector_offset → filename → label → num_partition_sectors → physical_partition_number → start_sector
     * （与旧 Sakura 顺序不同，部分 Loader 对属性顺序敏感）
     */
    if (include_filename) {
        return snprintf(buf, buf_size,
            "<?xml version=\"1.0\" ?><data><program SECTOR_SIZE_IN_BYTES=\"%d\" "
            "file_sector_offset=\"0\" filename=\"%s\" label=\"%s\" "
            "num_partition_sectors=\"%lld\" physical_partition_number=\"%d\" "
            "start_sector=\"%lld\"%s /></data>",
            sector_size, safe, safe, (long long)num_sectors, lun, (long long)start_sector, rb);
    }
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><program SECTOR_SIZE_IN_BYTES=\"%d\" "
        "file_sector_offset=\"0\" num_partition_sectors=\"%lld\" "
        "physical_partition_number=\"%d\" start_sector=\"%lld\" label=\"%s\"%s /></data>",
        sector_size, (long long)num_sectors, lun, (long long)start_sector, safe, rb);
}

int edl_xml_build_program_raw(char *buf, size_t buf_size,
                               int sector_size, int lun, int64_t start_sector,
                               int64_t num_sectors, bool read_back_verify)
{
    const char *rb = read_back_verify ? " read_back_verify=\"true\"" : "";
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><program SECTOR_SIZE_IN_BYTES=\"%d\" "
        "file_sector_offset=\"0\" num_partition_sectors=\"%lld\" "
        "physical_partition_number=\"%d\" start_sector=\"%lld\"%s /></data>",
        sector_size, (long long)num_sectors, lun, (long long)start_sector, rb);
}

int edl_xml_build_program_raw_expr(char *buf, size_t buf_size,
                                   int sector_size, int lun, const char *start_sector_expr,
                                   int64_t num_sectors, bool read_back_verify)
{
    const char *rb = read_back_verify ? " read_back_verify=\"true\"" : "";
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><program SECTOR_SIZE_IN_BYTES=\"%d\" "
        "file_sector_offset=\"0\" num_partition_sectors=\"%lld\" "
        "physical_partition_number=\"%d\" start_sector=\"%s\"%s /></data>",
        sector_size, (long long)num_sectors, lun,
        (start_sector_expr && start_sector_expr[0]) ? start_sector_expr : "0", rb);
}

int edl_xml_build_erase(char *buf, size_t buf_size,
                         int sector_size, int lun, int64_t start_sector,
                         int64_t num_sectors)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><erase SECTOR_SIZE_IN_BYTES=\"%d\" "
        "physical_partition_number=\"%d\" start_sector=\"%lld\" "
        "num_partition_sectors=\"%lld\" /></data>",
        sector_size, lun, (long long)start_sector, (long long)num_sectors);
}

int edl_xml_build_zeroout(char *buf, size_t buf_size,
                           int sector_size, int lun, int64_t start_sector,
                           int64_t num_sectors)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><zeroout SECTOR_SIZE_IN_BYTES=\"%d\" "
        "physical_partition_number=\"%d\" start_sector=\"%lld\" "
        "num_partition_sectors=\"%lld\" /></data>",
        sector_size, lun, (long long)start_sector, (long long)num_sectors);
}

int edl_xml_build_patch(char *buf, size_t buf_size,
                         int sector_size, int lun, int64_t start_sector,
                         int byte_offset, int size_in_bytes, const char *value)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><patch SECTOR_SIZE_IN_BYTES=\"%d\" "
        "physical_partition_number=\"%d\" start_sector=\"%lld\" "
        "byte_offset=\"%d\" size_in_bytes=\"%d\" value=\"%s\" /></data>",
        sector_size, lun, (long long)start_sector,
        byte_offset, size_in_bytes, value ? value : "0");
}

int edl_xml_build_nop(char *buf, size_t buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><nop value=\"ping\" /></data>");
}

int edl_xml_build_power(char *buf, size_t buf_size, const char *mode)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><power value=\"%s\" /></data>",
        mode ? mode : "reset");
}

int edl_xml_build_getstorageinfo(char *buf, size_t buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><getstorageinfo physical_partition_number=\"0\" /></data>");
}

int edl_xml_build_getddrtype(char *buf, size_t buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><getddrtype /></data>");
}

int edl_xml_build_setbootablestoragedrive(char *buf, size_t buf_size, int lun)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><setbootablestoragedrive value=\"%d\" /></data>", lun);
}
