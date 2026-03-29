#ifndef EDL_SPARSE_H
#define EDL_SPARSE_H

#include "edl_types.h"
#include "edl_error.h"
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPARSE_HEADER_MAGIC  0xED26FF3A

typedef enum {
    SPARSE_CHUNK_RAW     = 0xCAC1,
    SPARSE_CHUNK_FILL    = 0xCAC2,
    SPARSE_CHUNK_DONT_CARE = 0xCAC3,
    SPARSE_CHUNK_CRC32   = 0xCAC4
} edl_sparse_chunk_type_t;

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t file_hdr_sz;
    uint16_t chunk_hdr_sz;
    uint32_t blk_sz;
    uint32_t total_blks;
    uint32_t total_chunks;
    uint32_t image_checksum;
} edl_sparse_header_t;

typedef struct {
    uint16_t chunk_type;
    uint16_t reserved;
    uint32_t chunk_sz;     /* in blocks */
    uint32_t total_sz;     /* in bytes (including chunk header) */
} edl_sparse_chunk_header_t;

#pragma pack(pop)

/* Check if file is sparse format */
bool edl_sparse_is_sparse(const char *filepath);
bool edl_sparse_is_sparse_data(const uint8_t *data, size_t len);

typedef struct edl_sparse_reader edl_sparse_reader_t;

/* Open a sparse image for streaming decompression */
edl_sparse_reader_t *edl_sparse_open(const char *filepath);
void                  edl_sparse_close(edl_sparse_reader_t *reader);

/* Get the total uncompressed size in bytes */
int64_t edl_sparse_total_size(const edl_sparse_reader_t *reader);

/* Get block size */
uint32_t edl_sparse_block_size(const edl_sparse_reader_t *reader);

/*
 * Read next sequential chunk of decompressed data.
 * buf must be at least block_size bytes.
 * Returns bytes read, 0 on EOF, -1 on error.
 * *out_block_index is set to the logical block index of this chunk.
 */
int edl_sparse_read_block(edl_sparse_reader_t *reader, uint8_t *buf, int64_t *out_block_index);

/**
 * 读取镜像逻辑前缀：若为 Android sparse（如 system.img）则展开后写入缓冲区；
 * 否则按原始文件顺序读取前 max_bytes 字节。
 * @param max_bytes <= 0 时按 256 MiB 处理
 * @return 成功时 *out_buf 由 malloc 分配，调用方 free；*out_len 为有效长度
 */
edl_error_t edl_image_read_logical_prefix(const char *path, uint8_t **out_buf, int *out_len,
                                          int64_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* EDL_SPARSE_H */
