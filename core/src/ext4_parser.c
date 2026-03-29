#include "edl/ext4_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

struct ext4_parser {
    ext4_read_fn  read_fn;
    void         *read_ctx;
    ext4_log_fn   log_fn;
    void         *log_ctx;
    ext4_superblock_t sb;
    int           block_size;
    int           inode_size;
    int           bg_desc_size; /* 32 或 64（64bit 卷） */
    bool          valid;
    bool          has_extents;
    bool          has_64bit;
    bool          has_filetype;
    bool          has_inline;
    char          vol_name[17];
};

static void e4_log(ext4_parser_t *p, const char *fmt, ...)
{
    if (!p || !p->log_fn) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    p->log_fn(buf, p->log_ctx);
}

static int e4_read(ext4_parser_t *p, int64_t off, uint8_t *buf, int len)
{
    return p->read_fn(off, buf, len, p->read_ctx);
}

static bool parse_superblock(ext4_parser_t *p)
{
    uint8_t raw[EXT4_SUPERBLOCK_SIZE];
    int n = e4_read(p, EXT4_SUPERBLOCK_OFF, raw, EXT4_SUPERBLOCK_SIZE);
    if (n < 264) return false;

    memcpy(&p->sb, raw, sizeof(p->sb));
    if (p->sb.s_magic != EXT4_SUPER_MAGIC) {
        e4_log(p, "[EXT4] \xe6\x97\xa0\xe6\x95\x88\xe9\xad\x94\xe6\x95\xb0: 0x%04X", p->sb.s_magic);
        return false;
    }

    p->block_size = 1024 << p->sb.s_log_block_size;
    if (p->sb.s_rev_level == 0)
        p->inode_size = 128;
    else
        p->inode_size = p->sb.s_inode_size > 0 ? p->sb.s_inode_size : 128;

    uint32_t ic = p->sb.s_feature_incompat;
    p->has_extents = (ic & EXT4_FI_EXTENTS) != 0;
    p->has_64bit   = (ic & EXT4_FI_64BIT) != 0;
    p->has_filetype = (ic & EXT4_FI_FILETYPE) != 0;
    p->has_inline  = (ic & EXT4_FI_INLINE_DATA) != 0;
    p->bg_desc_size = p->has_64bit ? 64 : 32;

    memcpy(p->vol_name, p->sb.s_volume_name, 16);
    p->vol_name[16] = '\0';

    const char *fs = p->has_extents ? "EXT4" :
                     (p->sb.s_feature_compat & EXT4_FC_JOURNAL) ? "EXT3" : "EXT2";
    e4_log(p, "[%s] \xe8\xa7\xa3\xe6\x9e\x90\xe6\x88\x90\xe5\x8a\x9f - "
              "\xe5\x9d\x97\xe5\xa4\xa7\xe5\xb0\x8f: %d, Inode: %d, \xe5\x8d\xb7\xe6\xa0\x87: %s",
           fs, p->block_size, p->inode_size, p->vol_name);

    return true;
}

ext4_parser_t *ext4_open(ext4_read_fn read_fn, void *read_ctx,
                          ext4_log_fn log_fn, void *log_ctx)
{
    if (!read_fn) return NULL;
    ext4_parser_t *p = (ext4_parser_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->read_fn = read_fn;
    p->read_ctx = read_ctx;
    p->log_fn = log_fn;
    p->log_ctx = log_ctx;
    p->valid = parse_superblock(p);
    if (!p->valid) { free(p); return NULL; }
    return p;
}

void ext4_close(ext4_parser_t *p)
{
    free(p);
}

bool ext4_is_valid(const ext4_parser_t *p) { return p && p->valid; }
const char *ext4_volume_name(const ext4_parser_t *p) { return p ? p->vol_name : ""; }
int ext4_block_size(const ext4_parser_t *p) { return p ? p->block_size : 0; }

bool ext4_detect(ext4_read_fn read_fn, void *read_ctx)
{
    uint8_t magic[2];
    if (read_fn(EXT4_MAGIC_FILE_OFFSET, magic, 2, read_ctx) < 2) return false;
    uint16_t m = magic[0] | ((uint16_t)magic[1] << 8);
    return m == EXT4_SUPER_MAGIC;
}

/* ===== Inode 读取 ===== */

static bool read_inode(ext4_parser_t *p, uint32_t ino, ext4_inode_t *out)
{
    if (ino < 1) return false;
    uint32_t bg = (ino - 1) / p->sb.s_inodes_per_group;
    uint32_t li = (ino - 1) % p->sb.s_inodes_per_group;

    int64_t bgdt_off = p->block_size >= 2048 ? p->block_size : 2048;
    int ds = p->bg_desc_size;
    uint8_t bgd[64];
    if (e4_read(p, bgdt_off + (int64_t)bg * ds, bgd, ds) < ds) return false;

    uint32_t itable_lo;
    memcpy(&itable_lo, bgd + 8, 4);
    uint64_t itable_blk = itable_lo;
    if (p->has_64bit && ds >= 64) {
        uint32_t itable_hi;
        memcpy(&itable_hi, bgd + 40, 4);
        itable_blk |= (uint64_t)itable_hi << 32;
    }

    int64_t inode_off = (int64_t)itable_blk * p->block_size + (int64_t)li * p->inode_size;
    int need = p->inode_size;
    if (need < 128) need = 128;
    if (need > 1024) need = 1024;

    uint8_t *raw = (uint8_t *)malloc((size_t)need);
    if (!raw) return false;
    int got = e4_read(p, inode_off, raw, need);
    if (got < 128) {
        free(raw);
        return false;
    }
    memcpy(out, raw, sizeof(*out));
    free(raw);
    return true;
}

/* ===== 块数据读取 ===== */

typedef struct {
    ext4_parser_t *p;
    uint8_t *buf;
    int buf_size;
    int written;
    int64_t file_size;
} block_reader_t;

static void br_read_block(block_reader_t *r, uint32_t block_num)
{
    if (block_num == 0 || r->written >= r->file_size || r->written >= r->buf_size) return;
    int to_read = r->p->block_size;
    if (r->written + to_read > r->file_size) to_read = (int)(r->file_size - r->written);
    if (r->written + to_read > r->buf_size) to_read = r->buf_size - r->written;
    if (to_read <= 0) return;
    e4_read(r->p, (int64_t)block_num * r->p->block_size, r->buf + r->written, to_read);
    r->written += to_read;
}

static void br_read_indirect(block_reader_t *r, uint32_t blk, int level)
{
    if (blk == 0 || r->written >= r->file_size || r->written >= r->buf_size || level < 1) return;
    int bs = r->p->block_size;
    int ptrs = bs / 4;
    uint8_t *pbuf = (uint8_t *)malloc(bs);
    if (!pbuf) return;
    e4_read(r->p, (int64_t)blk * bs, pbuf, bs);
    for (int i = 0; i < ptrs && r->written < r->file_size && r->written < r->buf_size; i++) {
        uint32_t next;
        memcpy(&next, pbuf + i * 4, 4);
        if (next == 0) continue;
        if (level == 1)
            br_read_block(r, next);
        else
            br_read_indirect(r, next, level - 1);
    }
    free(pbuf);
}

static int read_direct_data(ext4_parser_t *p, const ext4_inode_t *ino,
                             uint8_t *buf, int buf_size)
{
    int64_t fsize = ino->i_size_lo | ((int64_t)ino->i_size_high << 32);
    if (fsize > buf_size) fsize = buf_size;

    block_reader_t r = { p, buf, buf_size, 0, fsize };

    for (int i = 0; i < 12; i++) {
        uint32_t bn;
        memcpy(&bn, ino->i_block + i * 4, 4);
        br_read_block(&r, bn);
    }
    if (r.written >= fsize) return r.written;

    uint32_t ind1; memcpy(&ind1, ino->i_block + 48, 4);
    if (ind1) br_read_indirect(&r, ind1, 1);
    if (r.written >= fsize) return r.written;

    uint32_t ind2; memcpy(&ind2, ino->i_block + 52, 4);
    if (ind2) br_read_indirect(&r, ind2, 2);
    if (r.written >= fsize) return r.written;

    uint32_t ind3; memcpy(&ind3, ino->i_block + 56, 4);
    if (ind3) br_read_indirect(&r, ind3, 3);

    return r.written;
}

/* extent 头 + 叶/索引节点（与 Linux ext4_extent_header 一致） */
static int read_extent_leaf_buf(ext4_parser_t *p, const uint8_t *eh_buf, int buf_len,
                                int64_t fsize, uint8_t *out, int out_cap, int *written)
{
    uint16_t magic, entries, depth;
    memcpy(&magic, eh_buf, 2);
    memcpy(&entries, eh_buf + 2, 2);
    memcpy(&depth, eh_buf + 6, 2);
    if (magic != EXT4_EXT_MAGIC || depth != 0)
        return -1;
    for (int i = 0; i < (int)entries; i++) {
        int off = 12 + i * 12;
        if (off + 12 > buf_len) break;
        uint16_t ee_len, ee_start_hi;
        uint32_t ee_start_lo;
        memcpy(&ee_len, eh_buf + off + 4, 2);
        memcpy(&ee_start_hi, eh_buf + off + 6, 2);
        memcpy(&ee_start_lo, eh_buf + off + 8, 4);
        uint64_t start = ee_start_lo | ((uint64_t)ee_start_hi << 32);
        int blocks = ee_len > 32768 ? ee_len - 32768 : ee_len;
        for (int b = 0; b < blocks && *written < fsize && *written < out_cap; b++) {
            int to_read = p->block_size;
            if (*written + to_read > fsize) to_read = (int)(fsize - *written);
            if (*written + to_read > out_cap) to_read = out_cap - *written;
            if (to_read <= 0) break;
            e4_read(p, (int64_t)(start + (uint64_t)b) * p->block_size,
                    out + *written, to_read);
            *written += to_read;
        }
    }
    return 0;
}

static int read_extent_node(ext4_parser_t *p, const uint8_t *eh_buf, int buf_len,
                            int64_t fsize, uint8_t *out, int out_cap, int *written)
{
    uint16_t magic, entries, depth;
    if (buf_len < 12) return -1;
    memcpy(&magic, eh_buf, 2);
    memcpy(&entries, eh_buf + 2, 2);
    memcpy(&depth, eh_buf + 6, 2);
    if (magic != EXT4_EXT_MAGIC) return -1;
    if (depth == 0)
        return read_extent_leaf_buf(p, eh_buf, buf_len, fsize, out, out_cap, written);

    for (uint16_t i = 0; i < entries; i++) {
        int off = 12 + (int)i * 12;
        if (off + 12 > buf_len) break;
        uint32_t ei_leaf_lo;
        uint16_t ei_leaf_hi;
        memcpy(&ei_leaf_lo, eh_buf + off + 4, 4);
        memcpy(&ei_leaf_hi, eh_buf + off + 8, 2);
        uint64_t child = ei_leaf_lo | ((uint64_t)ei_leaf_hi << 32);
        uint8_t *blk = (uint8_t *)malloc((size_t)p->block_size);
        if (!blk) return -1;
        if (e4_read(p, (int64_t)child * p->block_size, blk, p->block_size) < p->block_size) {
            free(blk);
            continue;
        }
        read_extent_node(p, blk, p->block_size, fsize, out, out_cap, written);
        free(blk);
        if (*written >= fsize || *written >= out_cap) break;
    }
    return 0;
}

static int read_extent_data(ext4_parser_t *p, const ext4_inode_t *ino,
                             uint8_t *buf, int buf_size)
{
    uint16_t magic;
    memcpy(&magic, ino->i_block, 2);
    if (magic != EXT4_EXT_MAGIC)
        return read_direct_data(p, ino, buf, buf_size);

    int64_t fsize = ino->i_size_lo | ((int64_t)ino->i_size_high << 32);
    if (fsize > buf_size) fsize = buf_size;
    int written = 0;
    (void)read_extent_node(p, ino->i_block, 60, fsize, buf, buf_size, &written);
    return written;
}

static int read_inode_data(ext4_parser_t *p, const ext4_inode_t *ino,
                            uint8_t *buf, int buf_size)
{
    bool use_ext = (ino->i_flags & EXT4_EXTENTS_FL) && p->has_extents;

    if (p->has_inline && (ino->i_flags & EXT4_INLINE_DATA_FL)) {
        int64_t fsize = ino->i_size_lo;
        int to_copy = fsize > 60 ? 60 : (int)fsize;
        if (to_copy > buf_size) to_copy = buf_size;
        memcpy(buf, ino->i_block, to_copy);
        return to_copy;
    }

    if (use_ext)
        return read_extent_data(p, ino, buf, buf_size);
    else
        return read_direct_data(p, ino, buf, buf_size);
}

/* ===== 目录遍历 ===== */

static bool ext4_dirent_valid(const uint8_t *d, int n, int off, bool has_ft,
                              char *name, int name_cap, uint32_t *ino_out, uint8_t *ft_out)
{
    (void)name_cap;
    int hdr = has_ft ? 8 : 7;
    if (off < 0 || off + hdr > n) return false;
    uint32_t ino;
    uint16_t rec_len;
    uint8_t nl;
    memcpy(&ino, d + off, 4);
    memcpy(&rec_len, d + off + 4, 2);
    memcpy(&nl, d + off + 6, 1);
    if (rec_len < (uint16_t)hdr || rec_len > 8192 || (rec_len & 3u) != 0) return false;
    if (off + (int)rec_len > n) return false;
    if (ino == 0 || nl == 0 || nl >= 255) return false;
    if (hdr + (int)nl > (int)rec_len) return false;
    memcpy(name, d + off + hdr, nl);
    name[nl] = '\0';
    for (int i = 0; i < nl; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32u || c == 127u) return false;
    }
    *ino_out = ino;
    *ft_out = has_ft ? d[off + 7] : 0;
    return true;
}

/* HTree：叶块内 dirent 仍为标准格式，顺序链在含 dx_root 的块上会错位 */
static void ext4_read_dir_aligned_scan(ext4_parser_t *p, const uint8_t *data, int n,
                                       ext4_dir_cb callback, void *ctx)
{
    bool has_ft = p->has_filetype;
    for (int off = 0; off + 8 <= n; off += 4) {
        char name[256];
        uint32_t ino;
        uint8_t ft;
        if (!ext4_dirent_valid(data, n, off, has_ft, name, sizeof(name), &ino, &ft))
            continue;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        callback(name, ino, (ext4_file_type_t)ft, ctx);
    }
}

bool ext4_read_dir(ext4_parser_t *p, uint32_t inode, ext4_dir_cb callback, void *ctx)
{
    if (!p || !p->valid || !callback) return false;

    ext4_inode_t ino;
    if (!read_inode(p, inode, &ino)) return false;

    int64_t dir_size = ino.i_size_lo | ((int64_t)ino.i_size_high << 32);
    if (dir_size <= 0 || dir_size > 4 * 1024 * 1024) return false;

    uint8_t *data = (uint8_t *)malloc((size_t)dir_size);
    if (!data) return false;

    int n = read_inode_data(p, &ino, data, (int)dir_size);
    if (n <= 0) { free(data); return false; }

    int off = 0;
    int seq_entries = 0;
    while (off + 8 <= n) {
        uint32_t de_inode;
        uint16_t rec_len;
        uint8_t name_len;
        memcpy(&de_inode, data + off, 4);
        memcpy(&rec_len, data + off + 4, 2);
        memcpy(&name_len, data + off + 6, 1);
        if (rec_len == 0 || rec_len > 8192 || off + rec_len > n) break;

        int hdr = p->has_filetype ? 8 : 7;
        uint8_t ftype = EXT4_FT_UNKNOWN;
        if (p->has_filetype) {
            if (off + 7 < n) ftype = data[off + 7];
        }
        if (de_inode != 0 && name_len > 0 && off + hdr + (int)name_len <= n) {
            char name[256];
            int nlen = name_len > 255 ? 255 : name_len;
            memcpy(name, data + off + hdr, nlen);
            name[nlen] = '\0';
            callback(name, de_inode, (ext4_file_type_t)ftype, ctx);
            seq_entries++;
        }
        off += rec_len;
    }

    bool has_hindex = (p->sb.s_feature_compat & EXT4_FC_DIR_INDEX) != 0;
    if (has_hindex || seq_entries < 3)
        ext4_read_dir_aligned_scan(p, data, n, callback, ctx);

    free(data);
    return true;
}

/* ===== 文件查找 ===== */

typedef struct {
    const char *target;
    uint32_t    found_ino;
} find_ctx_t;

static void find_cb(const char *name, uint32_t inode, ext4_file_type_t type, void *ctx)
{
    find_ctx_t *fc = (find_ctx_t *)ctx;
    (void)type;
    if (fc->found_ino != 0) return;
#ifdef _WIN32
    if (_stricmp(name, fc->target) == 0)
#else
    if (strcasecmp(name, fc->target) == 0)
#endif
        fc->found_ino = inode;
}

uint32_t ext4_find_file(ext4_parser_t *p, const char *path)
{
    if (!p || !path) return 0;

    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    uint32_t cur = EXT4_ROOT_INO;

    char *tok = buf;
    while (*tok == '/') tok++;

    char *next;
    while (*tok) {
        next = strchr(tok, '/');
        if (next) *next = '\0';

        find_ctx_t fc = { tok, 0 };
        ext4_read_dir(p, cur, find_cb, &fc);
        if (fc.found_ino == 0) return 0;
        cur = fc.found_ino;

        if (next) { tok = next + 1; while (*tok == '/') tok++; }
        else break;
    }
    return cur;
}

/* ===== 文件读取 ===== */

int ext4_read_file(ext4_parser_t *p, uint32_t inode, uint8_t *buf, int buf_size)
{
    if (!p || !p->valid || !buf || buf_size <= 0) return -1;
    ext4_inode_t ino;
    if (!read_inode(p, inode, &ino)) return -1;
    return read_inode_data(p, &ino, buf, buf_size);
}

int ext4_read_text(ext4_parser_t *p, const char *path, char *buf, int buf_size)
{
    uint32_t ino = ext4_find_file(p, path);
    if (ino == 0) return -1;
    int n = ext4_read_file(p, ino, (uint8_t *)buf, buf_size - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

bool ext4_get_prop(ext4_parser_t *p, const char *path, const char *key,
                    char *value, int value_size)
{
    if (!key || !value || value_size <= 0) return false;

    /*
     * system 分区镜像根即「挂载到 /system 后的内容」，常见为根目录直接有 build.prop，
     * 并无 system/ 子目录；故 /build.prop、/etc/build.prop 优先于 /system/build.prop。
     */
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
        n = ext4_read_text(p, path, text, sizeof(text));
    } else {
        for (int i = 0; search_paths[i]; i++) {
            n = ext4_read_text(p, search_paths[i], text, sizeof(text));
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
