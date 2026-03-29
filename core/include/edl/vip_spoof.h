#ifndef EDL_VIP_SPOOF_H
#define EDL_VIP_SPOOF_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OPLUS VIP 伪装策略 — OPPO/Realme 设备的 Digest 地址检查限制。
 *
 * 规则 (来自官方抓包):
 *   UFS 每 LUN 分三段:
 *     段1 (sector 0-5):  label=PrimaryGPT, filename=gpt_main{lun}.bin
 *     段2 (sector 6):    label=该 LUN 第一个分区名, filename=同上
 *     段3 (sector 7+):   label=PrimaryGPT, filename=gpt_main0.bin
 *   eMMC 同理但边界为 sector 0-33 / 34 / 35+
 *
 * 写入/擦除时: program/erase 命令需带伪装后的 label + filename
 * 读取时:   read 命令同理
 */

#define EDL_VIP_MAX_STRATEGIES  4

typedef struct {
    char label[72];
    char filename[260];
    int  priority;
} edl_vip_strategy_t;

/*
 * 为给定分区操作生成 VIP 伪装策略列表。
 * sector_size: 4096(UFS) 或 512(eMMC)
 * lun: LUN 编号
 * start_sector: 操作起始扇区
 * first_part_name: 该 LUN 第一个分区名 (可 NULL)
 *
 * 返回策略数量 (1-4)，优先级递减。
 */
int edl_vip_build_strategies(int sector_size, int lun, int64_t start_sector,
                              const char *first_part_name,
                              edl_vip_strategy_t *out, int max_out);

/*
 * 构建 VIP 模式 program XML。
 * 与普通 program 不同: 加了 label, filename, read_back_verify 属性。
 */
int edl_vip_build_program(char *buf, int buf_size,
                           int sector_size, int lun, int64_t start_sector,
                           int64_t num_sectors, const char *spoof_label,
                           const char *spoof_filename);

/*
 * 构建 VIP 模式 erase XML。
 */
int edl_vip_build_erase(char *buf, int buf_size,
                          int sector_size, int lun, int64_t start_sector,
                          int64_t num_sectors, const char *spoof_label,
                          const char *spoof_filename);

/*
 * 构建 VIP 模式 read XML。
 */
int edl_vip_build_read(char *buf, int buf_size,
                        int sector_size, int lun, int64_t start_sector,
                        int64_t num_sectors, const char *spoof_label,
                        const char *spoof_filename);

#ifdef __cplusplus
}
#endif

#endif /* EDL_VIP_SPOOF_H */
