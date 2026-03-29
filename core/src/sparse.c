#include "edl/sparse.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct edl_sparse_reader {
    FILE     *fp;
    edl_sparse_header_t hdr;

    uint32_t  current_chunk;
    edl_sparse_chunk_header_t chunk_hdr;
    uint32_t  blocks_left_in_chunk;
    uint32_t  fill_value;
    int64_t   current_block_index;
};

static uint32_t rd32_le(const uint8_t *p)
{
    return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
}

bool edl_sparse_is_sparse_data(const uint8_t *data, size_t len)
{
    if (!data || len < 4) return false;
    return rd32_le(data) == SPARSE_HEADER_MAGIC;
}

bool edl_sparse_is_sparse(const char *filepath)
{
    if (!filepath) return false;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return false;
    uint8_t hdr[4];
    bool result = (fread(hdr, 1, 4, fp) == 4) && edl_sparse_is_sparse_data(hdr, 4);
    fclose(fp);
    return result;
}

edl_sparse_reader_t *edl_sparse_open(const char *filepath)
{
    if (!filepath) return NULL;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return NULL;

    edl_sparse_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return NULL; }
    if (hdr.magic != SPARSE_HEADER_MAGIC) { fclose(fp); return NULL; }

    /* Seek past file header (may be larger than struct) */
    if (hdr.file_hdr_sz > sizeof(edl_sparse_header_t))
        fseek(fp, hdr.file_hdr_sz, SEEK_SET);

    edl_sparse_reader_t *r = (edl_sparse_reader_t *)calloc(1, sizeof(*r));
    if (!r) { fclose(fp); return NULL; }
    r->fp = fp;
    r->hdr = hdr;
    r->current_chunk = 0;
    r->blocks_left_in_chunk = 0;
    r->current_block_index = 0;
    return r;
}

void edl_sparse_close(edl_sparse_reader_t *reader)
{
    if (!reader) return;
    if (reader->fp) fclose(reader->fp);
    free(reader);
}

int64_t edl_sparse_total_size(const edl_sparse_reader_t *reader)
{
    if (!reader) return 0;
    return (int64_t)reader->hdr.total_blks * reader->hdr.blk_sz;
}

uint32_t edl_sparse_block_size(const edl_sparse_reader_t *reader)
{
    return reader ? reader->hdr.blk_sz : 4096;
}

int edl_sparse_read_block(edl_sparse_reader_t *reader, uint8_t *buf, int64_t *out_block_index)
{
    if (!reader || !buf || !reader->fp) return -1;

    while (1) {
        /* If we have blocks left in current chunk, service them */
        if (reader->blocks_left_in_chunk > 0) {
            uint32_t blk_sz = reader->hdr.blk_sz;

            switch (reader->chunk_hdr.chunk_type) {
            case SPARSE_CHUNK_RAW:
                if (fread(buf, blk_sz, 1, reader->fp) != 1)
                    return -1;
                break;

            case SPARSE_CHUNK_FILL:
                for (uint32_t i = 0; i < blk_sz; i += 4)
                    memcpy(buf + i, &reader->fill_value, 4);
                break;

            case SPARSE_CHUNK_DONT_CARE:
                memset(buf, 0, blk_sz);
                break;

            default:
                return -1;
            }

            if (out_block_index)
                *out_block_index = reader->current_block_index;

            reader->current_block_index++;
            reader->blocks_left_in_chunk--;
            return (int)blk_sz;
        }

        /* Load next chunk */
        if (reader->current_chunk >= reader->hdr.total_chunks)
            return 0; /* EOF */

        edl_sparse_chunk_header_t ch;
        if (fread(&ch, sizeof(ch), 1, reader->fp) != 1)
            return 0;

        /* Skip extra bytes in chunk header beyond our struct */
        if (reader->hdr.chunk_hdr_sz > sizeof(edl_sparse_chunk_header_t)) {
            int extra = reader->hdr.chunk_hdr_sz - sizeof(edl_sparse_chunk_header_t);
            fseek(reader->fp, extra, SEEK_CUR);
        }

        reader->chunk_hdr = ch;
        reader->blocks_left_in_chunk = ch.chunk_sz;
        reader->current_chunk++;

        if (ch.chunk_type == SPARSE_CHUNK_FILL) {
            if (fread(&reader->fill_value, 4, 1, reader->fp) != 1)
                return -1;
        } else if (ch.chunk_type == SPARSE_CHUNK_CRC32) {
            /* Skip CRC chunk data */
            uint32_t data_bytes = ch.total_sz - reader->hdr.chunk_hdr_sz;
            fseek(reader->fp, data_bytes, SEEK_CUR);
            reader->blocks_left_in_chunk = 0;
        } else if (ch.chunk_type == SPARSE_CHUNK_DONT_CARE) {
            /* No data to read from file */
        }
    }
}

edl_error_t edl_image_read_logical_prefix(const char *path, uint8_t **out_buf, int *out_len,
                                            int64_t max_bytes)
{
    if (!path || !out_buf || !out_len)
        return EDL_ERR_INVALID_PARAM;
    *out_buf = NULL;
    *out_len = 0;
    if (max_bytes <= 0)
        max_bytes = (int64_t)256 * 1024 * 1024;
    if (max_bytes > INT_MAX)
        max_bytes = INT_MAX;

    if (edl_sparse_is_sparse(path)) {
        edl_sparse_reader_t *r = edl_sparse_open(path);
        if (!r)
            return EDL_ERR_IO;
        int64_t total = edl_sparse_total_size(r);
        int64_t cap     = max_bytes < total ? max_bytes : total;
        if (cap <= 0) {
            edl_sparse_close(r);
            return EDL_ERR_IO;
        }
        uint32_t bs = edl_sparse_block_size(r);
        if (bs == 0 || bs > 32u * 1024u * 1024u) {
            edl_sparse_close(r);
            return EDL_ERR_INVALID_PARAM;
        }
        uint8_t *buf = (uint8_t *)malloc((size_t)cap);
        if (!buf) {
            edl_sparse_close(r);
            return EDL_ERR_NO_MEMORY;
        }
        uint8_t *blk = (uint8_t *)malloc((size_t)bs);
        if (!blk) {
            free(buf);
            edl_sparse_close(r);
            return EDL_ERR_NO_MEMORY;
        }
        int64_t filled = 0;
        while (filled < cap) {
            int n = edl_sparse_read_block(r, blk, NULL);
            if (n < 0) {
                free(blk);
                free(buf);
                edl_sparse_close(r);
                return EDL_ERR_IO;
            }
            if (n == 0)
                break;
            int64_t copy = cap - filled;
            if (copy > (int64_t)n)
                copy = (int64_t)n;
            memcpy(buf + filled, blk, (size_t)copy);
            filled += copy;
            if (copy < (int64_t)n)
                break;
        }
        free(blk);
        edl_sparse_close(r);
        *out_buf = buf;
        *out_len = (int)filled;
        return EDL_OK;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return EDL_ERR_IO;
    uint8_t *buf = (uint8_t *)malloc((size_t)max_bytes);
    if (!buf) {
        fclose(fp);
        return EDL_ERR_NO_MEMORY;
    }
    size_t n = fread(buf, 1, (size_t)max_bytes, fp);
    fclose(fp);
    *out_buf = buf;
    *out_len = (int)n;
    return EDL_OK;
}
