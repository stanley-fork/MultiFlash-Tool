#include "edl/erofs_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

struct erofs_parser {
    erofs_read_fn  read_fn;
    void          *read_ctx;
    erofs_log_fn   log_fn;
    void          *log_ctx;
    erofs_superblock_t sb;
    int            block_size;
    bool           valid;
    bool           has_chunks;
    bool           has_fragments;
    char           vol_name[17];
};

static void ef_log(erofs_parser_t *p, const char *fmt, ...)
{
    if (!p || !p->log_fn) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    p->log_fn(buf, p->log_ctx);
}

static int ef_read(erofs_parser_t *p, int64_t off, uint8_t *buf, int len)
{
    return p->read_fn(off, buf, len, p->read_ctx);
}

static int64_t get_inode_offset(erofs_parser_t *p, uint64_t nid)
{
    if (p->sb.meta_blkaddr == 0)
        return EROFS_SUPERBLOCK_OFF + 128 + (int64_t)nid * 32;
    return (int64_t)p->sb.meta_blkaddr * p->block_size + (int64_t)nid * 32;
}

static erofs_data_layout_t get_data_layout(uint16_t i_format)
{
    return (erofs_data_layout_t)((i_format >> 1) & 0x7);
}

static bool is_extended_inode(uint16_t i_format)
{
    return (i_format & 0x1) == 1;
}

static bool parse_superblock(erofs_parser_t *p)
{
    uint8_t raw[128];
    if (ef_read(p, EROFS_SUPERBLOCK_OFF, raw, 128) < 128) return false;
    memcpy(&p->sb, raw, sizeof(p->sb));

    if (p->sb.magic != EROFS_SUPER_MAGIC) {
        ef_log(p, "[EROFS] \xe6\x97\xa0\xe6\x95\x88\xe9\xad\x94\xe6\x95\xb0: 0x%08X", p->sb.magic);
        return false;
    }

    p->block_size = 1 << p->sb.blkszbits;

    uint32_t ic = p->sb.feature_incompat;
    p->has_chunks = (ic & EROFS_FI_CHUNKED_FILE) != 0;
    p->has_fragments = (ic & EROFS_FI_FRAGMENTS) != 0;

    memcpy(p->vol_name, p->sb.volume_name, 16);
    p->vol_name[16] = '\0';

    bool legacy = !(p->has_chunks || p->has_fragments || (ic & EROFS_FI_DEDUPE));
    ef_log(p, "[EROFS] \xe8\xa7\xa3\xe6\x9e\x90\xe6\x88\x90\xe5\x8a\x9f - "
              "\xe7\x89\x88\xe6\x9c\xac: %s, \xe5\x9d\x97\xe5\xa4\xa7\xe5\xb0\x8f: %d, "
              "\xe5\x8d\xb7\xe5\x90\x8d: %s, root_nid: %u",
           legacy ? "Legacy" : "Modern", p->block_size, p->vol_name, p->sb.root_nid);

    return true;
}

erofs_parser_t *erofs_open(erofs_read_fn read_fn, void *read_ctx,
                            erofs_log_fn log_fn, void *log_ctx)
{
    if (!read_fn) return NULL;
    erofs_parser_t *p = (erofs_parser_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->read_fn = read_fn;
    p->read_ctx = read_ctx;
    p->log_fn = log_fn;
    p->log_ctx = log_ctx;
    p->valid = parse_superblock(p);
    if (!p->valid) { free(p); return NULL; }
    return p;
}

void erofs_close(erofs_parser_t *p) { free(p); }
bool erofs_is_valid(const erofs_parser_t *p) { return p && p->valid; }
const char *erofs_volume_name(const erofs_parser_t *p) { return p ? p->vol_name : ""; }
int erofs_block_size(const erofs_parser_t *p) { return p ? p->block_size : 0; }

bool erofs_detect(erofs_read_fn read_fn, void *read_ctx)
{
    uint8_t raw[4];
    if (read_fn(EROFS_SUPERBLOCK_OFF, raw, 4, read_ctx) < 4) return false;
    uint32_t m;
    memcpy(&m, raw, 4);
    return m == EROFS_SUPER_MAGIC;
}

/* ===== Inode 读取 ===== */

static bool read_inode_compact(erofs_parser_t *p, uint64_t nid, erofs_inode_compact_t *out)
{
    int64_t off = get_inode_offset(p, nid);
    uint8_t raw[32];
    if (ef_read(p, off, raw, 32) < 32) return false;
    memcpy(out, raw, sizeof(*out));
    return true;
}

static bool read_inode_extended(erofs_parser_t *p, uint64_t nid, erofs_inode_extended_t *out)
{
    int64_t off = get_inode_offset(p, nid);
    uint8_t raw[64];
    if (ef_read(p, off, raw, 64) < 64) return false;
    memcpy(out, raw, sizeof(*out));
    return true;
}

/* ===== 数据读取 ===== */

static int read_flat_plain(erofs_parser_t *p, const erofs_inode_compact_t *ino, uint8_t *buf, int size)
{
    int64_t off = (int64_t)ino->i_u * p->block_size;
    return ef_read(p, off, buf, size);
}

static int read_flat_inline(erofs_parser_t *p, uint64_t nid, const erofs_inode_compact_t *ino,
                             uint8_t *buf, int size)
{
    int64_t ioff = get_inode_offset(p, nid);
    int ino_sz = is_extended_inode(ino->i_format) ? 64 : 32;
    int xattr_sz = ino->i_xattr_icount > 0 ? 12 + (ino->i_xattr_icount - 1) * 4 : 0;
    int aligned = (ino_sz + xattr_sz + 3) & ~3;
    int64_t data_off = ioff + aligned;

    int tail_size = p->block_size - (int)(data_off % p->block_size);

    if (size <= tail_size) {
        return ef_read(p, data_off, buf, size);
    }

    int n1 = ef_read(p, data_off, buf, tail_size);
    if (n1 < tail_size) return n1;

    int64_t ext_off = (int64_t)ino->i_u * p->block_size;
    int n2 = ef_read(p, ext_off, buf + tail_size, size - tail_size);
    return n1 + (n2 > 0 ? n2 : 0);
}

static int read_chunk_based(erofs_parser_t *p, uint64_t nid, const erofs_inode_compact_t *ino,
                              uint8_t *buf, int size)
{
    int64_t ioff = get_inode_offset(p, nid);
    int ino_sz = is_extended_inode(ino->i_format) ? 64 : 32;
    int xattr_sz = ino->i_xattr_icount > 0 ? 12 + (ino->i_xattr_icount - 1) * 4 : 0;
    int ci_off = ino_sz + xattr_sz;

    int chunk_bits = p->sb.blkszbits;
    int chunk_size = 1 << chunk_bits;
    int chunk_count = (size + chunk_size - 1) / chunk_size;
    int written = 0;

    for (int i = 0; i < chunk_count && written < size; i++) {
        uint8_t idx[8];
        ef_read(p, ioff + ci_off + (int64_t)i * 8, idx, 8);
        uint32_t blk_addr;
        memcpy(&blk_addr, idx, 4);

        int to_read = chunk_size;
        if (written + to_read > size) to_read = size - written;

        if (blk_addr == 0xFFFFFFFF) {
            memset(buf + written, 0, to_read);
        } else {
            int64_t doff = (int64_t)blk_addr * p->block_size;
            ef_read(p, doff, buf + written, to_read);
        }
        written += to_read;
    }
    return written;
}

static int read_compressed_raw(erofs_parser_t *p, const erofs_inode_compact_t *ino,
                                 uint8_t *buf, int size)
{
    int64_t off = (int64_t)ino->i_u * p->block_size;
    int read_size = size * 2;
    if (read_size > 128 * 1024) read_size = 128 * 1024;
    if (read_size > size) {
        uint8_t *tmp = (uint8_t *)malloc(read_size);
        if (!tmp) return -1;
        int n = ef_read(p, off, tmp, read_size);
        if (n > 0) {
            int copy = n > size ? size : n;
            memcpy(buf, tmp, copy);
            free(tmp);
            return copy;
        }
        free(tmp);
        return -1;
    }
    return ef_read(p, off, buf, size);
}

static int read_inode_data(erofs_parser_t *p, uint64_t nid, const erofs_inode_compact_t *ino_in,
                            uint8_t *buf, int buf_size)
{
    erofs_inode_compact_t cin = *ino_in;
    uint64_t fsize64 = cin.i_size;
    if (is_extended_inode(cin.i_format)) {
        erofs_inode_extended_t ext;
        if (!read_inode_extended(p, nid, &ext)) return -1;
        fsize64 = ext.i_size;
        cin.i_u = ext.i_u;
        cin.i_format = ext.i_format;
        cin.i_xattr_icount = ext.i_xattr_icount;
    }

    if (fsize64 == 0 || fsize64 > (uint64_t)64 * 1024 * 1024) return -1;
    int size = (fsize64 > (uint64_t)buf_size) ? buf_size : (int)fsize64;
    if (size <= 0) return -1;

    erofs_data_layout_t dl = get_data_layout(cin.i_format);
    switch (dl) {
    case EROFS_DL_FLAT_PLAIN:
        return read_flat_plain(p, &cin, buf, size);
    case EROFS_DL_FLAT_INLINE:
        return read_flat_inline(p, nid, &cin, buf, size);
    case EROFS_DL_CHUNK_BASED:
        return read_chunk_based(p, nid, &cin, buf, size);
    case EROFS_DL_COMPRESSED_FULL:
    case EROFS_DL_COMPRESSED_COMPACT:
        ef_log(p, "[EROFS] \xe5\x8e\x8b\xe7\xbc\xa9\xe6\x95\xb0\xe6\x8d\xae\xe5\xb8\x83\xe5\xb1\x80"
                   "\xef\xbc\x8c\xe5\xb0\x9d\xe8\xaf\x95\xe8\xaf\xbb\xe5\x8f\x96\xe5\x8e\x9f\xe5\xa7\x8b\xe6\x95\xb0\xe6\x8d\xae...");
        return read_compressed_raw(p, &cin, buf, size);
    default:
        return -1;
    }
}

/* ===== 目录遍历 ===== */

bool erofs_read_dir(erofs_parser_t *p, uint64_t nid, erofs_dir_cb callback, void *ctx)
{
    if (!p || !p->valid || !callback) return false;

    erofs_inode_compact_t ino;
    if (!read_inode_compact(p, nid, &ino)) return false;
    if ((ino.i_mode & 0xF000) != 0x4000) return false;

    uint64_t dsize64 = ino.i_size;
    if (is_extended_inode(ino.i_format)) {
        erofs_inode_extended_t ext;
        if (!read_inode_extended(p, nid, &ext)) return false;
        dsize64 = ext.i_size;
    }
    if (dsize64 == 0 || dsize64 > (uint64_t)4 * 1024 * 1024) return false;
    int dsize = (int)dsize64;

    uint8_t *data = (uint8_t *)malloc((size_t)dsize);
    if (!data) return false;

    int n = read_inode_data(p, nid, &ino, data, dsize);
    if (n < 12) { free(data); return false; }

    int off = 0;
    while (off + 12 <= n) {
        uint64_t entry_nid;
        uint16_t nameoff;
        uint8_t file_type;
        uint8_t name_len;
        memcpy(&entry_nid, data + off, 8);
        memcpy(&nameoff, data + off + 8, 2);
        file_type = data[off + 10];
        name_len = data[off + 11];

        if (entry_nid == 0 && nameoff == 0) break;

        if (name_len == 0 && nameoff < n) {
            int k = 0;
            while (nameoff + k < n && k < 255 && data[nameoff + k] != '\0') k++;
            name_len = (uint8_t)k;
        }

        if (name_len > 0 && (int)nameoff + (int)name_len <= n) {
            char name[256];
            int nl = name_len > 255 ? 255 : name_len;
            memcpy(name, data + nameoff, nl);
            while (nl > 0 && name[nl - 1] == '\0') nl--;
            name[nl] = '\0';
            if (name[0] != '\0')
                callback(name, entry_nid, (erofs_file_type_t)file_type, ctx);
        }
        off += 12;
    }

    free(data);
    return true;
}

/* ===== 文件查找 ===== */

typedef struct {
    const char *target;
    uint64_t    found_nid;
} erofs_find_ctx_t;

static void erofs_find_cb(const char *name, uint64_t nid, erofs_file_type_t type, void *ctx)
{
    erofs_find_ctx_t *fc = (erofs_find_ctx_t *)ctx;
    (void)type;
    if (fc->found_nid != 0) return;
#ifdef _WIN32
    if (_stricmp(name, fc->target) == 0)
#else
    if (strcasecmp(name, fc->target) == 0)
#endif
        fc->found_nid = nid;
}

uint64_t erofs_find_file(erofs_parser_t *p, const char *path)
{
    if (!p || !path) return 0;

    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    uint64_t cur = p->sb.root_nid;

    char *tok = buf;
    while (*tok == '/') tok++;

    char *next;
    while (*tok) {
        next = strchr(tok, '/');
        if (next) *next = '\0';

        erofs_find_ctx_t fc = { tok, 0 };
        erofs_read_dir(p, cur, erofs_find_cb, &fc);
        if (fc.found_nid == 0) return 0;
        cur = fc.found_nid;

        if (next) { tok = next + 1; while (*tok == '/') tok++; }
        else break;
    }
    return cur;
}

/* ===== 文件读取 ===== */

int erofs_read_file(erofs_parser_t *p, uint64_t nid, uint8_t *buf, int buf_size)
{
    if (!p || !p->valid || !buf || buf_size <= 0) return -1;
    erofs_inode_compact_t ino;
    if (!read_inode_compact(p, nid, &ino)) return -1;
    return read_inode_data(p, nid, &ino, buf, buf_size);
}

int erofs_read_text(erofs_parser_t *p, const char *path, char *buf, int buf_size)
{
    uint64_t nid = erofs_find_file(p, path);
    if (nid == 0) return -1;
    int n = erofs_read_file(p, nid, (uint8_t *)buf, buf_size - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

bool erofs_get_prop(erofs_parser_t *p, const char *path, const char *key,
                     char *value, int value_size)
{
    if (!key || !value || value_size <= 0) return false;

    static const char *search_paths[] = {
        "/build.prop",
        "/etc/build.prop",
        "/system/build.prop",
        "/system/etc/build.prop",
        "/product/build.prop",
        "/system_ext/etc/build.prop",
        "/vendor/build.prop",
        "/odm/etc/build.prop",
        NULL
    };

    char text[256 * 1024];
    int n = -1;

    if (path) {
        n = erofs_read_text(p, path, text, sizeof(text));
    } else {
        for (int i = 0; search_paths[i]; i++) {
            n = erofs_read_text(p, search_paths[i], text, sizeof(text));
            if (n > 0) break;
        }
    }
    if (n <= 0) return false;

    int klen = (int)strlen(key);
    char *line = text;
    while (*line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (*trimmed != '#' && *trimmed != '\0') {
            char *eq = strchr(trimmed, '=');
            if (eq) {
                int cur_klen = (int)(eq - trimmed);
                while (cur_klen > 0 && (trimmed[cur_klen - 1] == ' ' || trimmed[cur_klen - 1] == '\t'))
                    cur_klen--;
                if (cur_klen == klen && strncmp(trimmed, key, klen) == 0) {
                    char *val = eq + 1;
                    while (*val == ' ' || *val == '\t') val++;
                    snprintf(value, value_size, "%s", val);
                    return true;
                }
            }
        }

        if (eol) line = eol + 1; else break;
    }
    return false;
}
