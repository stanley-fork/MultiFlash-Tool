#include "edl/vip_spoof.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int edl_vip_build_strategies(int sector_size, int lun, int64_t start_sector,
                              const char *first_part_name,
                              edl_vip_strategy_t *out, int max_out)
{
    if (!out || max_out <= 0) return 0;
    memset(out, 0, sizeof(edl_vip_strategy_t) * (max_out < EDL_VIP_MAX_STRATEGIES ? max_out : EDL_VIP_MAX_STRATEGIES));

    /* OPLUS 分段边界 */
    int seg1_end = (sector_size == 4096) ? 5 : 33;     /* 段1 末尾扇区 */
    int seg2_sector = (sector_size == 4096) ? 6 : 34;   /* 段2 起始扇区 */

    int count = 0;
    char gpt_file[64];
    snprintf(gpt_file, sizeof(gpt_file), "gpt_main%d.bin", lun);

    if (start_sector <= seg1_end) {
        /* 段1: PrimaryGPT / gpt_main{lun}.bin */
        if (count < max_out) {
            snprintf(out[count].label, sizeof(out[count].label), "PrimaryGPT");
            snprintf(out[count].filename, sizeof(out[count].filename), "%s", gpt_file);
            out[count].priority = 100;
            count++;
        }
        if (count < max_out) {
            snprintf(out[count].label, sizeof(out[count].label), "PrimaryGPT");
            snprintf(out[count].filename, sizeof(out[count].filename), "gpt_main0.bin");
            out[count].priority = 90;
            count++;
        }
    } else if (start_sector == seg2_sector) {
        /* 段2: 使用该 LUN 第一个分区名 */
        if (first_part_name && first_part_name[0] && count < max_out) {
            snprintf(out[count].label, sizeof(out[count].label), "%s", first_part_name);
            snprintf(out[count].filename, sizeof(out[count].filename), "%s", first_part_name);
            out[count].priority = 100;
            count++;
        }
        if (count < max_out) {
            snprintf(out[count].label, sizeof(out[count].label), "PrimaryGPT");
            snprintf(out[count].filename, sizeof(out[count].filename), "%s", gpt_file);
            out[count].priority = 80;
            count++;
        }
    } else {
        /* 段3: PrimaryGPT / gpt_main0.bin */
        if (count < max_out) {
            snprintf(out[count].label, sizeof(out[count].label), "PrimaryGPT");
            snprintf(out[count].filename, sizeof(out[count].filename), "gpt_main0.bin");
            out[count].priority = 100;
            count++;
        }
        if (count < max_out) {
            snprintf(out[count].label, sizeof(out[count].label), "PrimaryGPT");
            snprintf(out[count].filename, sizeof(out[count].filename), "%s", gpt_file);
            out[count].priority = 90;
            count++;
        }
    }

    return count;
}

int edl_vip_build_program(char *buf, int buf_size,
                           int sector_size, int lun, int64_t start_sector,
                           int64_t num_sectors, const char *spoof_label,
                           const char *spoof_filename)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\"?><data><program SECTOR_SIZE_IN_BYTES=\"%d\" "
        "num_partition_sectors=\"%lld\" physical_partition_number=\"%d\" "
        "start_sector=\"%lld\" filename=\"%s\" label=\"%s\" "
        "read_back_verify=\"true\"/></data>",
        sector_size, (long long)num_sectors, lun,
        (long long)start_sector, spoof_filename, spoof_label);
}

int edl_vip_build_erase(char *buf, int buf_size,
                          int sector_size, int lun, int64_t start_sector,
                          int64_t num_sectors, const char *spoof_label,
                          const char *spoof_filename)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\"?><data><erase SECTOR_SIZE_IN_BYTES=\"%d\" "
        "num_partition_sectors=\"%lld\" physical_partition_number=\"%d\" "
        "start_sector=\"%lld\" filename=\"%s\" label=\"%s\"/></data>",
        sector_size, (long long)num_sectors, lun,
        (long long)start_sector, spoof_filename, spoof_label);
}

int edl_vip_build_read(char *buf, int buf_size,
                        int sector_size, int lun, int64_t start_sector,
                        int64_t num_sectors, const char *spoof_label,
                        const char *spoof_filename)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\"?><data><read SECTOR_SIZE_IN_BYTES=\"%d\" "
        "num_partition_sectors=\"%lld\" physical_partition_number=\"%d\" "
        "start_sector=\"%lld\" filename=\"%s\" label=\"%s\"/></data>",
        sector_size, (long long)num_sectors, lun,
        (long long)start_sector, spoof_filename, spoof_label);
}
