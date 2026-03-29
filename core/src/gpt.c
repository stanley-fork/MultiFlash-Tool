#include "edl/gpt.h"
#include "edl/crc32.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#ifdef _WIN32
#define EDL_STRICMP _stricmp
#else
#include <strings.h>
#define EDL_STRICMP strcasecmp
#endif

/* GPT constants */
#define GPT_SIGNATURE     "EFI PART"
#define GPT_SIG_LEN       8
#define GPT_HDR_SIZE      92
#define GPT_ENTRY_SIZE    128

/* Read little-endian values from buffer */
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4)<<32); }

/* GPT 分区名：UTF-16LE → UTF-8（避免中文等显示为 ?） */
static bool gpt_append_utf8(uint32_t cp, char *out, int out_cap, int *io)
{
    int o = *io;
    if (cp <= 0x7Fu) {
        if (o + 1 >= out_cap) return false;
        out[o++] = (char)cp;
    } else if (cp <= 0x7FFu) {
        if (o + 2 >= out_cap) return false;
        out[o++] = (char)(0xC0 | (cp >> 6));
        out[o++] = (char)(0x80 | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        if (o + 3 >= out_cap) return false;
        out[o++] = (char)(0xE0 | (cp >> 12));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3Fu));
        out[o++] = (char)(0x80 | (cp & 0x3Fu));
    } else if (cp <= 0x10FFFFu) {
        if (o + 4 >= out_cap) return false;
        out[o++] = (char)(0xF0 | (cp >> 18));
        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3Fu));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3Fu));
        out[o++] = (char)(0x80 | (cp & 0x3Fu));
    } else {
        return false;
    }
    *io = o;
    return true;
}

static void gpt_utf16le_name_to_utf8(const uint8_t *name_raw, int name_bytes_in_entry,
                                     char *out, int out_cap)
{
    if (!out || out_cap <= 0)
        return;
    out[0] = '\0';
    if (!name_raw || name_bytes_in_entry <= 0)
        return;
    int units = name_bytes_in_entry / 2;
    if (units > 36)
        units = 36; /* UEFI 分区名最多 36 个 UTF-16 码元 */
    int o = 0;
    for (int j = 0; j < units && o < out_cap - 1; j++) {
        uint16_t ch = (uint16_t)(name_raw[j * 2] | (name_raw[j * 2 + 1] << 8));
        if (ch == 0)
            break;
        if (ch >= 0xD800u && ch <= 0xDBFFu && j + 1 < units) {
            uint16_t ch2 = (uint16_t)(name_raw[(j + 1) * 2] | (name_raw[(j + 1) * 2 + 1] << 8));
            if (ch2 >= 0xDC00u && ch2 <= 0xDFFFu) {
                uint32_t cp = 0x10000u
                    + ((((uint32_t)(ch & 0x3FFu)) << 10) | (uint32_t)(ch2 & 0x3FFu));
                j++;
                if (!gpt_append_utf8(cp, out, out_cap, &o))
                    break;
                continue;
            }
        }
        if (ch >= 0xD800u && ch <= 0xDFFFu)
            continue; /* 未配对代理项 */
        if (!gpt_append_utf8((uint32_t)ch, out, out_cap, &o))
            break;
    }
    out[o] = '\0';
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

int edl_gpt_firehose_gpt_region_sectors(int sector_size)
{
    if (sector_size <= 0)
        return 6;
    return (sector_size == 512) ? 34 : 6;
}

int edl_gpt_backup_region_sectors(int sector_size)
{
    if (sector_size <= 0)
        return 5;
    return (sector_size == 512) ? 33 : 5;
}

void edl_guid_to_str(const uint8_t *g, char *buf)
{
    /*
     * Microsoft mixed-endian GUID format:
     * Data1 (4 bytes LE) - Data2 (2 bytes LE) - Data3 (2 bytes LE) - Data4 (8 bytes BE)
     */
    sprintf(buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            g[3], g[2], g[1], g[0],   /* Data1 LE */
            g[5], g[4],               /* Data2 LE */
            g[7], g[6],               /* Data3 LE */
            g[8], g[9],               /* Data4 BE */
            g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int detect_sector_size(const uint8_t *data, int data_len)
{
    /* Sector size candidates */
    int sizes[] = { 4096, 512 };
    for (int i = 0; i < 2; i++) {
        int off = sizes[i]; /* GPT header at LBA 1 */
        if (off + GPT_HDR_SIZE <= data_len) {
            if (memcmp(data + off, GPT_SIGNATURE, GPT_SIG_LEN) == 0)
                return sizes[i];
        }
    }
    return 0;
}

/* UEFI GPT header revision at offset 8 */
#define GPT_REVISION_1_0  0x00010000u

/*
 * Firehose 读回的数据前可能带少量残留字节（非从 LBA0 对齐），固定查 512/4096 会漏掉 EFI PART。
 * 在缓冲区前 64KiB 内扫描主 GPT 头（LBA1），返回 LBA0 在缓冲区中的起始偏移。
 */
static int find_lba0_prefix_length(const uint8_t *data, int data_len)
{
    int scan_max = data_len - GPT_HDR_SIZE;
    if (scan_max > 65536)
        scan_max = 65536;
    for (int j = 0; j <= scan_max; j++) {
        if (memcmp(data + j, GPT_SIGNATURE, GPT_SIG_LEN) != 0)
            continue;
        if (rd32(data + j + 8) != GPT_REVISION_1_0)
            continue;
        const int ss[] = { 4096, 512 };
        for (int k = 0; k < 2; k++) {
            int p = j - ss[k];
            if (p >= 0)
                return p;
        }
    }
    return -1;
}

int edl_gpt_buffer_has_primary_header(const uint8_t *data, int data_len)
{
    if (!data || data_len < GPT_HDR_SIZE + 8)
        return 0;
    if (detect_sector_size(data, data_len) != 0)
        return 1;
    return find_lba0_prefix_length(data, data_len) >= 0 ? 1 : 0;
}

int edl_gpt_bytes_needed_for_primary_parse(const uint8_t *data, int data_len, int *out_need_bytes)
{
    if (!data || !out_need_bytes || data_len < GPT_HDR_SIZE + 8)
        return -1;

    const uint8_t *d = data;
    int len = data_len;

    int actual_sector = detect_sector_size(d, len);
    if (actual_sector == 0) {
        int prefix = find_lba0_prefix_length(d, len);
        if (prefix < 0)
            return -1;
        d += prefix;
        len -= prefix;
        actual_sector = detect_sector_size(d, len);
        if (actual_sector == 0)
            return -1;
    }

    const uint8_t *hdr = d + actual_sector;
    if (memcmp(hdr, GPT_SIGNATURE, GPT_SIG_LEN) != 0)
        return -1;

    uint32_t hdr_size = rd32(hdr + 12);
    if (hdr_size < GPT_HDR_SIZE || hdr_size > 16384u)
        return -1;
    if ((int64_t)actual_sector + (int64_t)hdr_size > (int64_t)len)
        return -1;

    uint64_t entry_lba = rd64(hdr + 72);
    uint32_t num_entries = rd32(hdr + 80);
    uint32_t entry_size = rd32(hdr + 84);
    if (entry_size < GPT_ENTRY_SIZE)
        entry_size = GPT_ENTRY_SIZE;

    uint64_t end = entry_lba * (uint64_t)actual_sector + (uint64_t)num_entries * (uint64_t)entry_size;
    if (end > (uint64_t)INT_MAX)
        return -1;

    *out_need_bytes = (int)end;
    return 0;
}

int edl_gpt_update_primary_crcs(uint8_t *gpt_data, int data_len, int sector_size)
{
    (void)sector_size;
    if (!gpt_data || data_len < GPT_HDR_SIZE + 8)
        return -1;

    uint8_t *d = gpt_data;
    int len = data_len;

    int actual_sector = detect_sector_size((const uint8_t *)d, len);
    if (actual_sector == 0) {
        int prefix = find_lba0_prefix_length((const uint8_t *)d, len);
        if (prefix < 0)
            return -1;
        d += prefix;
        len -= prefix;
        actual_sector = detect_sector_size((const uint8_t *)d, len);
        if (actual_sector == 0)
            return -1;
    }

    uint8_t *hdr = d + actual_sector;
    if (memcmp(hdr, GPT_SIGNATURE, GPT_SIG_LEN) != 0)
        return -1;

    uint32_t hdr_size = rd32(hdr + 12);
    if (hdr_size < GPT_HDR_SIZE || hdr_size > 16384u)
        return -1;
    if ((int64_t)actual_sector + (int64_t)hdr_size > (int64_t)len)
        return -1;

    uint64_t entry_lba = rd64(hdr + 72);
    uint32_t num_entries = rd32(hdr + 80);
    uint32_t entry_size = rd32(hdr + 84);
    if (entry_size < GPT_ENTRY_SIZE)
        entry_size = GPT_ENTRY_SIZE;

    uint64_t arr_off = entry_lba * (uint64_t)actual_sector;
    uint64_t arr_len = (uint64_t)num_entries * (uint64_t)entry_size;
    if (arr_off > (uint64_t)len || arr_off + arr_len > (uint64_t)len)
        return -1;

    uint32_t parray_crc = edl_crc32(d + arr_off, (size_t)arr_len);
    wr32(hdr + 88, parray_crc);

    uint8_t *hdr_copy = (uint8_t *)malloc((size_t)hdr_size);
    if (!hdr_copy)
        return -1;
    memcpy(hdr_copy, hdr, (size_t)hdr_size);
    hdr_copy[16] = hdr_copy[17] = hdr_copy[18] = hdr_copy[19] = 0;
    uint32_t hcrc = edl_crc32(hdr_copy, (size_t)hdr_size);
    free(hdr_copy);
    wr32(hdr + 16, hcrc);

    return 0;
}

int edl_gpt_update_backup_region_crcs(uint8_t *buf, int len, int sector_size,
                                       int64_t region_start_lba)
{
    if (!buf || len < sector_size || sector_size <= 0 || region_start_lba < 0)
        return -1;
    if (len % sector_size != 0)
        return -1;

    /* 备份 GPT 头位于该区域的最后一个逻辑扇区 */
    uint8_t *hdr = buf + len - sector_size;

    if (memcmp(hdr, GPT_SIGNATURE, GPT_SIG_LEN) != 0)
        return -1;

    uint32_t hdr_size = rd32(hdr + 12);
    if (hdr_size < GPT_HDR_SIZE || hdr_size > 16384u)
        return -1;
    if ((int64_t)hdr_size > (int64_t)sector_size)
        return -1;

    uint64_t entry_lba = rd64(hdr + 72);
    uint32_t num_entries = rd32(hdr + 80);
    uint32_t entry_size = rd32(hdr + 84);
    if (entry_size < GPT_ENTRY_SIZE)
        entry_size = GPT_ENTRY_SIZE;

    if (entry_lba < (uint64_t)region_start_lba)
        return -1;
    uint64_t rel = entry_lba - (uint64_t)region_start_lba;
    uint64_t arr_off = rel * (uint64_t)sector_size;
    uint64_t arr_len = (uint64_t)num_entries * (uint64_t)entry_size;
    if (arr_off + arr_len > (uint64_t)len)
        return -1;

    uint32_t parray_crc = edl_crc32(buf + arr_off, (size_t)arr_len);
    wr32(hdr + 88, parray_crc);

    uint8_t *hdr_copy = (uint8_t *)malloc((size_t)hdr_size);
    if (!hdr_copy)
        return -1;
    memcpy(hdr_copy, hdr, (size_t)hdr_size);
    hdr_copy[16] = hdr_copy[17] = hdr_copy[18] = hdr_copy[19] = 0;
    uint32_t hcrc = edl_crc32(hdr_copy, (size_t)hdr_size);
    free(hdr_copy);
    wr32(hdr + 16, hcrc);

    return 0;
}

int edl_gpt_parse(const uint8_t *data, int data_len, int lun, int sector_size,
                   edl_partition_info_t *parts, int max_parts, unsigned flags)
{
    if (!data || !parts || max_parts <= 0) return -1;

    const uint8_t *d = data;
    int len = data_len;

    int actual_sector = detect_sector_size(d, len);
    if (actual_sector == 0) {
        int prefix = find_lba0_prefix_length(d, len);
        if (prefix < 0)
            return -1;
        d += prefix;
        len -= prefix;
        actual_sector = detect_sector_size(d, len);
        if (actual_sector == 0)
            return -1;
    }

    if (sector_size <= 0) sector_size = actual_sector;

    const uint8_t *hdr = d + actual_sector;

    /* Validate signature */
    if (memcmp(hdr, GPT_SIGNATURE, GPT_SIG_LEN) != 0) return -1;

    uint32_t hdr_size = rd32(hdr + 12);
    /* UEFI: Header CRC32 covers bytes [0, header_size); field at 16-19 is zeroed for computation */
    if (hdr_size < GPT_HDR_SIZE || hdr_size > 16384u) return -1;
    if ((int64_t)actual_sector + (int64_t)hdr_size > (int64_t)len) return -1;

    if (!(flags & EDL_GPT_PARSE_IGNORE_HEADER_CRC)) {
        uint32_t stored_crc = rd32(hdr + 16);
        uint8_t *hdr_copy = (uint8_t *)malloc((size_t)hdr_size);
        if (!hdr_copy) return -1;
        memcpy(hdr_copy, hdr, (size_t)hdr_size);
        hdr_copy[16] = hdr_copy[17] = hdr_copy[18] = hdr_copy[19] = 0;
        uint32_t computed_crc = edl_crc32(hdr_copy, (size_t)hdr_size);
        free(hdr_copy);
        if (computed_crc != stored_crc) return -1;
    }

    uint64_t entry_lba = rd64(hdr + 72);
    uint32_t num_entries = rd32(hdr + 80);
    uint32_t entry_size = rd32(hdr + 84);

    if (entry_size < GPT_ENTRY_SIZE) entry_size = GPT_ENTRY_SIZE;

    /* Partition entries start at byte offset entry_lba * logical_block (actual_sector) */
    uint64_t entries_byte_off = entry_lba * (uint64_t)actual_sector;
    if (entries_byte_off > (uint64_t)len) return -1;
    if (num_entries > 0 && entry_size > UINT64_MAX / (uint64_t)num_entries) return -1;
    /* Truncated read (e.g. small gpt_sectors): parse entries that fit in buffer */
    int count = 0;

    for (uint32_t i = 0; i < num_entries && count < max_parts; i++) {
        uint64_t eoff64 = entries_byte_off + (uint64_t)i * (uint64_t)entry_size;
        if (eoff64 > (uint64_t)INT_MAX) break;
        if (eoff64 + (uint64_t)entry_size > (uint64_t)len) break;
        int eoff = (int)eoff64;

        const uint8_t *entry = d + eoff;

        /* 空槽：Type GUID 与 Unique GUID 均为全 0 */
        bool type_zero = true;
        for (int j = 0; j < 16; j++) {
            if (entry[j] != 0) { type_zero = false; break; }
        }
        bool unique_zero = true;
        for (int j = 16; j < 32; j++) {
            if (entry[j] != 0) { unique_zero = false; break; }
        }
        if (type_zero && unique_zero)
            continue;

        /*
         * Qualcomm UFS 部分 LUN（如 LUN2）分区条目 Type GUID 为全 0，但分区仍存在
         * （Unique GUID 非 0）。与 SakuraEDL C# GptParser.HasValidPartitionEntry 一致。
         */

        edl_partition_info_t *p = &parts[count];
        memset(p, 0, sizeof(*p));
        p->lun = lun;
        p->sector_size = sector_size;
        p->entry_index = (int)i;

        edl_guid_to_str(entry, p->type_guid);
        edl_guid_to_str(entry + 16, p->unique_guid);

        /* Partition entry first/last LBA are in sectors of the GPT logical layout
         * (actual_sector: 512 for eMMC-style buffer, 4096 for UFS-style buffer).
         * Firehose read/program use sector_size from Configure (device bytes per sector).
         * When actual_sector != sector_size, convert [first_byte, last_byte] to device LBA. */
        uint64_t start_lba = rd64(entry + 32);
        uint64_t end_lba = rd64(entry + 40);
        p->attributes = rd64(entry + 48);

        /* 未使用条目常见 first=last=0 */
        if (start_lba == 0 && end_lba == 0)
            continue;
        if (start_lba > end_lba) continue;

        if (sector_size > 0 && actual_sector > 0 && sector_size != actual_sector) {
            uint64_t first_byte = start_lba * (uint64_t)actual_sector;
            uint64_t last_byte = end_lba * (uint64_t)actual_sector + (uint64_t)actual_sector - 1;
            p->start_sector = (int64_t)(first_byte / (uint64_t)sector_size);
            uint64_t last_dev_sec = last_byte / (uint64_t)sector_size;
            p->num_sectors = (int64_t)(last_dev_sec - (uint64_t)p->start_sector + 1);
        } else {
            p->start_sector = (int64_t)start_lba;
            int64_t end_sector = (int64_t)end_lba;
            p->num_sectors = end_sector - p->start_sector + 1;
        }

        /* UTF-16LE name at offset 56（UEFI 36 码元）→ UTF-8 */
        const uint8_t *name_raw = entry + 56;
        int name_field_bytes = (int)entry_size - 56;
        if (name_field_bytes < 0)
            name_field_bytes = 0;
        gpt_utf16le_name_to_utf8(name_raw, name_field_bytes, p->name, EDL_PART_NAME_MAX);

        /* 丢弃主机侧无法安全表示的区间（避免后续 read/program 溢出） */
        if (p->start_sector < 0 || p->num_sectors <= 0)
            continue;
        if (p->num_sectors > INT64_MAX - p->start_sector)
            continue;

        count++;
    }

    return count;
}

static int gpt_has_partition_name(const edl_partition_info_t *parts, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (EDL_STRICMP(parts[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static void gpt_fill_protective_row(edl_partition_info_t *p, int lun, int sector_size,
                                    const char *name, int64_t start, int64_t num_sec)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->lun = lun;
    p->sector_size = sector_size;
    p->start_sector = start;
    p->num_sectors = num_sec;
    p->entry_index = -1;
}

int edl_gpt_prepend_protective_partitions(const uint8_t *gpt_data, int gpt_len, int lun, int sector_size,
                                          edl_partition_info_t *parts, int *inout_count, int max_parts)
{
    if (!gpt_data || !parts || !inout_count || max_parts <= 0)
        return -1;
    int n = *inout_count;
    if (n < 0 || n > max_parts)
        return -1;

    const uint8_t *d = gpt_data;
    int len = gpt_len;

    int actual_sector = detect_sector_size(d, len);
    if (actual_sector == 0) {
        int prefix = find_lba0_prefix_length(d, len);
        if (prefix < 0)
            return -1;
        d += prefix;
        len -= prefix;
        actual_sector = detect_sector_size(d, len);
        if (actual_sector == 0)
            return -1;
    }
    if (sector_size <= 0)
        sector_size = actual_sector;

    const uint8_t *hdr = d + actual_sector;
    if (memcmp(hdr, GPT_SIGNATURE, GPT_SIG_LEN) != 0)
        return -1;

    uint64_t alternate_lba = rd64(hdr + 32);
    int64_t disk_sectors = (int64_t)(alternate_lba + 1);
    if (disk_sectors < 2)
        return 0;

    int pr_sec = edl_gpt_firehose_gpt_region_sectors(sector_size);
    int bk_sec = edl_gpt_backup_region_sectors(sector_size);

    if (disk_sectors < (int64_t)(pr_sec + bk_sec))
        return 0;

    int64_t backup_start = disk_sectors - (int64_t)bk_sec;

    int have_pri = gpt_has_partition_name(parts, n, "PrimaryGPT");
    int have_bak = gpt_has_partition_name(parts, n, "BackupGPT");

    if (!have_pri && !have_bak) {
        if (n + 2 > max_parts)
            return 0;
        memmove(parts + 2, parts, (size_t)n * sizeof(edl_partition_info_t));
        gpt_fill_protective_row(&parts[0], lun, sector_size, "PrimaryGPT", 0, pr_sec);
        gpt_fill_protective_row(&parts[1], lun, sector_size, "BackupGPT", backup_start, bk_sec);
        *inout_count = n + 2;
        return 0;
    }

    if (have_pri && !have_bak && n + 1 <= max_parts) {
        gpt_fill_protective_row(&parts[n], lun, sector_size, "BackupGPT", backup_start, bk_sec);
        *inout_count = n + 1;
        return 0;
    }

    if (!have_pri && have_bak && n + 1 <= max_parts) {
        memmove(parts + 1, parts, (size_t)n * sizeof(edl_partition_info_t));
        gpt_fill_protective_row(&parts[0], lun, sector_size, "PrimaryGPT", 0, pr_sec);
        *inout_count = n + 1;
        return 0;
    }

    return 0;
}

