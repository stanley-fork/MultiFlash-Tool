#ifndef EDL_EXT4_PARSER_H
#define EDL_EXT4_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EXT2/3/4 文件系统只读解析器
 * 支持: SuperBlock 解析, Extent/直接块/内联数据, 目录遍历, 文件读取
 * 用途: 从 system/vendor 分区镜像中读取 build.prop 等配置文件
 */

#define EXT4_SUPER_MAGIC      0xEF53
#define EXT4_SUPERBLOCK_OFF   1024
#define EXT4_SUPERBLOCK_SIZE  1024
/** 超块内 s_magic 相对超块起始偏移（与 ext4_superblock_t 一致）；脚本 analyze_system_img.py 可校验 */
#define EXT4_MAGIC_OFF_IN_SB  56
/** 分区/逻辑镜像内绝对字节偏移（sparse 展开后逻辑字节 0 = 分区首字节时） */
#define EXT4_MAGIC_FILE_OFFSET (EXT4_SUPERBLOCK_OFF + EXT4_MAGIC_OFF_IN_SB)
#define EXT4_ROOT_INO         2

/* Inode 特性标志 */
#define EXT4_EXTENTS_FL       0x00080000
#define EXT4_INLINE_DATA_FL   0x10000000
#define EXT4_EXT_MAGIC        0xF30A

/* Feature flags (incompat) */
#define EXT4_FI_FILETYPE      0x0002
#define EXT4_FI_EXTENTS       0x0040
#define EXT4_FI_64BIT         0x0080
#define EXT4_FI_INLINE_DATA   0x8000
#define EXT4_FI_ENCRYPT       0x10000

/* Feature flags (compat) */
#define EXT4_FC_JOURNAL       0x0004
#define EXT4_FC_DIR_INDEX     0x0020 /* HTree 哈希目录；仅靠顺序 rec_len 链易漏项 */

#pragma pack(push, 1)

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    int16_t  s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    uint8_t  s_volume_name[16];
    uint8_t  s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
} ext4_superblock_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint8_t  i_block[60];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
} ext4_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} ext4_dir_entry_t;

#pragma pack(pop)

typedef enum {
    EXT4_FT_UNKNOWN   = 0,
    EXT4_FT_REG_FILE  = 1,
    EXT4_FT_DIR       = 2,
    EXT4_FT_CHRDEV    = 3,
    EXT4_FT_BLKDEV    = 4,
    EXT4_FT_FIFO      = 5,
    EXT4_FT_SOCK      = 6,
    EXT4_FT_SYMLINK   = 7
} ext4_file_type_t;

/* 目录遍历回调: name, inode, file_type */
typedef void (*ext4_dir_cb)(const char *name, uint32_t inode, ext4_file_type_t type, void *ctx);

typedef void (*ext4_log_fn)(const char *msg, void *ctx);

/* 解析器上下文 (不透明) */
typedef struct ext4_parser ext4_parser_t;

/*
 * 数据源回调: 从镜像/流中读取任意位置的数据
 * offset: 文件中的绝对偏移
 * buf:    输出缓冲区
 * len:    要读取的字节数
 * ctx:    用户数据
 * 返回:   实际读取的字节数，<0 表示错误
 */
typedef int (*ext4_read_fn)(int64_t offset, uint8_t *buf, int len, void *ctx);

/*
 * 打开 EXT4 解析器
 * read_fn: 数据读取回调
 * read_ctx: 读取回调上下文
 * log_fn:  日志回调 (可 NULL)
 * log_ctx: 日志上下文
 * 返回:    解析器句柄，失败返回 NULL
 */
ext4_parser_t *ext4_open(ext4_read_fn read_fn, void *read_ctx,
                          ext4_log_fn log_fn, void *log_ctx);

/* 关闭并释放 */
void ext4_close(ext4_parser_t *p);

/* 检查是否有效 */
bool ext4_is_valid(const ext4_parser_t *p);

/* 获取卷标 (NUL 结尾, 最多 16 字符) */
const char *ext4_volume_name(const ext4_parser_t *p);

/* 获取块大小 */
int ext4_block_size(const ext4_parser_t *p);

/* 静态检测: 读取 offset 1024+56 的 2 字节魔数 */
bool ext4_detect(ext4_read_fn read_fn, void *read_ctx);

/*
 * 查找文件，返回 inode 号，未找到返回 0
 * path: 类似 "/system/build.prop" 的路径
 */
uint32_t ext4_find_file(ext4_parser_t *p, const char *path);

/* 遍历目录 */
bool ext4_read_dir(ext4_parser_t *p, uint32_t inode, ext4_dir_cb callback, void *ctx);

/*
 * 读取文件内容
 * inode:    文件 inode 号
 * buf:      输出缓冲区
 * buf_size: 缓冲区大小
 * 返回:     实际读取字节数，<0 表示错误
 */
int ext4_read_file(ext4_parser_t *p, uint32_t inode, uint8_t *buf, int buf_size);

/*
 * 便捷: 从镜像读取文本文件
 * path: 文件路径
 * buf:  输出缓冲区 (NUL 结尾)
 * buf_size: 缓冲区大小
 * 返回: 文本长度，<0 失败
 */
int ext4_read_text(ext4_parser_t *p, const char *path, char *buf, int buf_size);

/*
 * 便捷: 从镜像读取 build.prop 的某个属性
 * path:  build.prop 路径 (NULL 则自动搜索常见位置)
 * key:   属性名 (如 "ro.build.display.id")
 * value: 输出缓冲区
 * value_size: 缓冲区大小
 * 返回:  true 找到
 */
bool ext4_get_prop(ext4_parser_t *p, const char *path, const char *key,
                    char *value, int value_size);

#ifdef __cplusplus
}
#endif

#endif /* EDL_EXT4_PARSER_H */
