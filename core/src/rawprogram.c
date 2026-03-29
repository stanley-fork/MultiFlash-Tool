#include "edl/rawprogram.h"
#include "edl/sparse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <io.h>
#endif

/* ===== Internal XML Mini-Parser ===== */

#define RP_LINE_MAX 4096
#define RP_TAG_BUF_MAX 65536

static void rp_trim(char *s)
{
    char *e = s + strlen(s) - 1;
    while (e >= s && isspace((unsigned char)*e)) *e-- = '\0';
    char *b = s;
    while (*b && isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static int rp_get_attr_str(const char *line, const char *attr, char *out, size_t out_size)
{
    char search[128];
    snprintf(search, sizeof(search), "%s=\"", attr);
    const char *p = strstr(line, search);
    if (!p) {
        snprintf(search, sizeof(search), "%s='", attr);
        p = strstr(line, search);
        if (!p) return -1;
    }
    p += strlen(search);
    char quote = *(p - 1);
    size_t i = 0;
    while (*p && *p != quote && i < out_size - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static int64_t rp_get_attr_int64(const char *line, const char *attr, int64_t def)
{
    char val[64];
    if (rp_get_attr_str(line, attr, val, sizeof(val)) != 0) return def;
    if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))
        return (int64_t)strtoull(val, NULL, 16);
    return (int64_t)strtoll(val, NULL, 10);
}

static int rp_get_attr_int(const char *line, const char *attr, int def)
{
    return (int)rp_get_attr_int64(line, attr, def);
}

static bool rp_get_attr_bool(const char *line, const char *attr, bool def)
{
    char val[16];
    if (rp_get_attr_str(line, attr, val, sizeof(val)) != 0) return def;
#ifdef _WIN32
    return _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
#else
    return strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0;
#endif
}

/* 多行 <program/> 合并后，将换行/制表符压成空格，便于属性查找（含 physical_partition_number） */
static void rp_collapse_ws(char *s)
{
    for (char *p = s; *p; ++p) {
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    }
}

static int rp_line_opens_program_tag(const char *line)
{
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "<program", 8) == 0) return 1;
    if (strncmp(p, "<erase", 6) == 0) return 1;
    if (strncmp(p, "<zeroout", 8) == 0) return 1;
    if (strncmp(p, "<Program", 8) == 0) return 1;
    if (strncmp(p, "<Erase", 6) == 0) return 1;
    if (strncmp(p, "<Zeroout", 8) == 0) return 1;
    if (strncmp(p, "<PROGRAM", 8) == 0) return 1;
    if (strncmp(p, "<ERASE", 6) == 0) return 1;
    if (strncmp(p, "<ZEROOUT", 8) == 0) return 1;
    return 0;
}

static int rp_append_tag(char *buf, size_t buf_sz, size_t *len_io, const char *chunk)
{
    size_t len = *len_io;
    size_t cl = strlen(chunk);
    if (len + cl + 1 >= buf_sz)
        return -1;
    memcpy(buf + len, chunk, cl);
    len += cl;
    buf[len] = '\0';
    *len_io = len;
    return 0;
}

/* 处理一条完整 program/erase/zeroout 标签（可含多行合并后的内容） */
static int rp_process_program_tag(const char *line, const char *base_dir, edl_flash_task_t *t)
{
    memset(t, 0, sizeof(*t));

    bool is_program = (strstr(line, "<program") != NULL || strstr(line, "<Program") != NULL
                       || strstr(line, "<PROGRAM") != NULL);
    bool is_erase = (strstr(line, "<erase") != NULL) || (strstr(line, "<zeroout") != NULL)
                      || (strstr(line, "<Erase") != NULL) || (strstr(line, "<ERASE") != NULL)
                      || (strstr(line, "<Zeroout") != NULL) || (strstr(line, "<ZEROOUT") != NULL);

    if (!is_program && !is_erase)
        return 0;

    rp_get_attr_str(line, "label", t->label, sizeof(t->label));
    rp_get_attr_str(line, "filename", t->filename, sizeof(t->filename));
    rp_get_attr_str(line, "start_sector", t->start_sector_expr, sizeof(t->start_sector_expr));

    t->lun                = rp_get_attr_int(line, "physical_partition_number", 0);
    t->start_sector       = rp_get_attr_int64(line, "start_sector", 0);
    t->num_sectors        = rp_get_attr_int64(line, "num_partition_sectors", 0);
    t->sector_size        = rp_get_attr_int(line, "SECTOR_SIZE_IN_BYTES", 4096);
    t->file_offset        = rp_get_attr_int64(line, "file_sector_offset", 0);
    t->file_sector_offset = t->file_offset;
    t->is_sparse          = rp_get_attr_bool(line, "sparse", false);

    if (is_erase) {
        t->type = strstr(line, "<zeroout") || strstr(line, "<Zeroout") || strstr(line, "<ZEROOUT")
                      ? EDL_TASK_ZEROOUT
                      : EDL_TASK_ERASE;
    } else {
        t->type = EDL_TASK_PROGRAM;
        if (t->filename[0] == '\0')
            return 0;
    }

    if (base_dir && base_dir[0] && t->filename[0]) {
        snprintf(t->filepath, sizeof(t->filepath), "%s/%s", base_dir, t->filename);
#ifdef _WIN32
        for (char *p = t->filepath; *p; p++)
            if (*p == '/') *p = '\\';
#endif
    } else {
        snprintf(t->filepath, sizeof(t->filepath), "%s", t->filename);
    }

    return 1;
}

/* ===== Rawprogram Parser ===== */

int edl_rawprogram_parse(const char *xml_path, const char *base_dir,
                          edl_flash_task_t *tasks, int max_tasks)
{
    if (!xml_path || !tasks || max_tasks <= 0) return -1;

    FILE *fp = fopen(xml_path, "r");
    if (!fp) return -1;

    char line[RP_LINE_MAX];
    char tag_buf[RP_TAG_BUF_MAX];
    size_t tag_len = 0;
    bool in_tag = false;
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < max_tasks) {
        if (!in_tag) {
            rp_trim(line);
            if (line[0] == '\0')
                continue;
            if (!rp_line_opens_program_tag(line))
                continue;
            in_tag = true;
            tag_len = 0;
            if (rp_append_tag(tag_buf, sizeof(tag_buf), &tag_len, line) != 0) {
                fclose(fp);
                return -1;
            }
        } else {
            rp_trim(line);
            if (rp_append_tag(tag_buf, sizeof(tag_buf), &tag_len, line) != 0) {
                fclose(fp);
                return -1;
            }
        }

        if (!in_tag)
            continue;

        if (!strstr(tag_buf, "/>"))
            continue;

        rp_collapse_ws(tag_buf);
        edl_flash_task_t *t = &tasks[count];
        int pr = rp_process_program_tag(tag_buf, base_dir, t);
        in_tag = false;
        if (pr == 1)
            count++;
    }

    fclose(fp);
    if (in_tag)
        return -1; /* 未闭合的 <program … */

    return count;
}

static int64_t rp_file_size_bytes(const char *path)
{
    if (!path || !path[0]) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
#ifdef _WIN32
    _fseeki64(fp, 0, SEEK_END);
    int64_t sz = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    int64_t sz = (int64_t)ftello(fp);
#endif
    fclose(fp);
    return sz;
}

void edl_flash_task_infer_sectors_from_image(edl_flash_task_t *t)
{
    if (!t || t->type != EDL_TASK_PROGRAM) return;
    if (t->num_sectors > 0) return;
    if (!t->filepath[0]) return;

    int ss = t->sector_size > 0 ? t->sector_size : 4096;

    int64_t img_bytes = 0;
    if (edl_sparse_is_sparse(t->filepath)) {
        edl_sparse_reader_t *r = edl_sparse_open(t->filepath);
        if (r) {
            img_bytes = edl_sparse_total_size(r);
            edl_sparse_close(r);
        }
    }
    if (img_bytes <= 0)
        img_bytes = rp_file_size_bytes(t->filepath);

    if (img_bytes > 0 && ss > 0)
        t->num_sectors = (img_bytes + (int64_t)ss - 1) / (int64_t)ss;
}

/* ===== Patch Parser（支持多行 <patch … />）===== */

static int rp_line_opens_patch_tag(const char *line)
{
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "<patch", 6) == 0) return 1;
    if (strncmp(p, "<Patch", 6) == 0) return 1;
    if (strncmp(p, "<PATCH", 6) == 0) return 1;
    return 0;
}

static int rp_process_patch_tag(const char *line, edl_patch_entry_t *p)
{
    memset(p, 0, sizeof(*p));

    if (strstr(line, "<patch") == NULL && strstr(line, "<Patch") == NULL
        && strstr(line, "<PATCH") == NULL)
        return 0;

    p->lun           = rp_get_attr_int(line, "physical_partition_number", 0);
    p->start_sector  = rp_get_attr_int64(line, "start_sector", 0);
    p->byte_offset   = rp_get_attr_int(line, "byte_offset", 0);
    p->size_in_bytes = rp_get_attr_int(line, "size_in_bytes", 0);

    rp_get_attr_str(line, "value", p->value, sizeof(p->value));
    rp_get_attr_str(line, "what", p->what, sizeof(p->what));
    rp_get_attr_str(line, "filename", p->filename, sizeof(p->filename));

    if (p->size_in_bytes == 0)
        return 0;
    return 1;
}

int edl_patch_parse(const char *xml_path,
                     edl_patch_entry_t *patches, int max_patches)
{
    if (!xml_path || !patches || max_patches <= 0) return -1;

    FILE *fp = fopen(xml_path, "r");
    if (!fp) return -1;

    char line[RP_LINE_MAX];
    char tag_buf[RP_TAG_BUF_MAX];
    size_t tag_len = 0;
    bool in_tag = false;
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < max_patches) {
        if (!in_tag) {
            rp_trim(line);
            if (line[0] == '\0')
                continue;
            if (!rp_line_opens_patch_tag(line))
                continue;
            in_tag = true;
            tag_len = 0;
            if (rp_append_tag(tag_buf, sizeof(tag_buf), &tag_len, line) != 0) {
                fclose(fp);
                return -1;
            }
        } else {
            rp_trim(line);
            if (rp_append_tag(tag_buf, sizeof(tag_buf), &tag_len, line) != 0) {
                fclose(fp);
                return -1;
            }
        }

        if (!in_tag)
            continue;

        if (!strstr(tag_buf, "/>"))
            continue;

        rp_collapse_ws(tag_buf);
        edl_patch_entry_t *p = &patches[count];
        int pr = rp_process_patch_tag(tag_buf, p);
        in_tag = false;
        if (pr == 1)
            count++;
    }

    fclose(fp);
    if (in_tag)
        return -1;

    return count;
}
