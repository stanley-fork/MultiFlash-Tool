#ifndef EDL_EROFS_PARSER_H
#define EDL_EROFS_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EROFS (Enhanced Read-Only File System) 只读解析器
 * 支持: FlatPlain / FlatInline / ChunkBased 数据布局
 * 注意: 压缩数据 (LZ4/LZMA) 仅做尽力提取，不保证完整解压
 * 用途: 从 super/system/vendor 分区 EROFS 镜像中读取 build.prop 等配置
 */

#define EROFS_SUPER_MAGIC     0xE0F5E1E2
#define EROFS_SUPERBLOCK_OFF  1024
/** 超块首 4 字节为 magic；与 scripts/analyze_system_img.py 中 EROFS 检测一致 */
#define EROFS_MAGIC_FILE_OFFSET EROFS_SUPERBLOCK_OFF

/* Feature incompat flags */
#define EROFS_FI_CHUNKED_FILE  0x0004
#define EROFS_FI_DEVICE_TABLE  0x0008
#define EROFS_FI_COMPR_HEAD2   0x0010
#define EROFS_FI_ZTAILPACKING  0x0020
#define EROFS_FI_FRAGMENTS     0x0040
#define EROFS_FI_DEDUPE        0x0080

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint32_t checksum;
    uint32_t feature_compat;
    uint8_t  blkszbits;
    uint8_t  sb_extslots;
    uint16_t root_nid;
    uint64_t inos;
    uint64_t build_time;
    uint32_t build_time_nsec;
    uint32_t blocks;
    uint32_t meta_blkaddr;
    uint32_t xattr_blkaddr;
    uint8_t  uuid[16];
    uint8_t  volume_name[16];
    uint32_t feature_incompat;
    uint16_t available_compr_algs;
    uint16_t extra_devices;
    uint16_t devt_slotoff;
    uint8_t  dirblkbits;
    uint8_t  xattr_prefix_count;
    uint32_t xattr_prefix_start;
    uint64_t packed_nid;
    uint8_t  xattr_filter_reserved;
    uint8_t  reserved2[23];
} erofs_superblock_t;

typedef struct {
    uint16_t i_format;
    uint16_t i_xattr_icount;
    uint16_t i_mode;
    uint16_t i_nlink;
    uint32_t i_size;
    uint32_t i_reserved;
    uint32_t i_u;
    uint32_t i_ino;
    uint16_t i_uid;
    uint16_t i_gid;
    uint32_t i_reserved2;
} erofs_inode_compact_t;

typedef struct {
    uint16_t i_format;
    uint16_t i_xattr_icount;
    uint16_t i_mode;
    uint16_t i_reserved;
    uint64_t i_size;
    uint32_t i_u;
    uint32_t i_ino;
    uint32_t i_uid;
    uint32_t i_gid;
    uint64_t i_mtime;
    uint32_t i_mtime_nsec;
    uint32_t i_nlink;
    uint8_t  i_reserved2[16];
} erofs_inode_extended_t;

/* 与 Linux erofs_dirent 一致：12 字节头 + 名称在目录块 nameoff 处 */
typedef struct {
    uint64_t nid;
    uint16_t nameoff;
    uint8_t  file_type;
    uint8_t  name_len;
} erofs_dir_entry_t;

#pragma pack(pop)

typedef enum {
    EROFS_FT_UNKNOWN   = 0,
    EROFS_FT_REG_FILE  = 1,
    EROFS_FT_DIR       = 2,
    EROFS_FT_CHRDEV    = 3,
    EROFS_FT_BLKDEV    = 4,
    EROFS_FT_FIFO      = 5,
    EROFS_FT_SOCK      = 6,
    EROFS_FT_SYMLINK   = 7
} erofs_file_type_t;

typedef enum {
    EROFS_DL_FLAT_PLAIN      = 0,
    EROFS_DL_COMPRESSED_FULL = 1,
    EROFS_DL_FLAT_INLINE     = 2,
    EROFS_DL_COMPRESSED_COMPACT = 3,
    EROFS_DL_CHUNK_BASED     = 4
} erofs_data_layout_t;

typedef void (*erofs_dir_cb)(const char *name, uint64_t nid, erofs_file_type_t type, void *ctx);
typedef void (*erofs_log_fn)(const char *msg, void *ctx);

typedef struct erofs_parser erofs_parser_t;

typedef int (*erofs_read_fn)(int64_t offset, uint8_t *buf, int len, void *ctx);

erofs_parser_t *erofs_open(erofs_read_fn read_fn, void *read_ctx,
                            erofs_log_fn log_fn, void *log_ctx);
void erofs_close(erofs_parser_t *p);
bool erofs_is_valid(const erofs_parser_t *p);
const char *erofs_volume_name(const erofs_parser_t *p);
int erofs_block_size(const erofs_parser_t *p);
bool erofs_detect(erofs_read_fn read_fn, void *read_ctx);

uint64_t erofs_find_file(erofs_parser_t *p, const char *path);
bool erofs_read_dir(erofs_parser_t *p, uint64_t nid, erofs_dir_cb callback, void *ctx);
int erofs_read_file(erofs_parser_t *p, uint64_t nid, uint8_t *buf, int buf_size);
int erofs_read_text(erofs_parser_t *p, const char *path, char *buf, int buf_size);
bool erofs_get_prop(erofs_parser_t *p, const char *path, const char *key,
                     char *value, int value_size);

#ifdef __cplusplus
}
#endif

#endif /* EDL_EROFS_PARSER_H */
