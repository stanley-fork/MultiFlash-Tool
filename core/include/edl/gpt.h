#ifndef EDL_GPT_H
#define EDL_GPT_H

#include "edl_types.h"
#include "edl_error.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse GPT from raw sector data (starting from LBA 0).
 * Returns number of partitions found, or negative on error.
 * parts[] is filled up to max_parts entries.
 *
 * Layout:
 *  - Detects logical block size of the buffer (512 vs 4096) by GPT header at LBA 1.
 *  - Validates primary GPT header CRC32 over header_size bytes (UEFI).
 *  - Partition entry StartingLBA/EndingLBA are in logical sectors of that layout.
 *  - sector_size: Firehose Configure bytes-per-sector (device). When it differs from
 *    the buffer logical size, start_sector/num_sectors are converted via byte range
 *    so read/program/erase use device LBA consistently.
 *
 * flags: 0 = 校验主 GPT 头 CRC；EDL_GPT_PARSE_IGNORE_HEADER_CRC = 忽略头 CRC（兼容异常固件）。
 *
 * 分区条目：全零 Type GUID 且全零 Unique GUID 视为空槽并跳过。高通 UFS 部分 LUN 上存在
 * Type GUID 全零但 Unique GUID 非零的有效条目（与 SakuraEDL C# GptParser 一致），此类仍会解析。
 */
#define EDL_GPT_PARSE_IGNORE_HEADER_CRC  0x00000001u

int edl_gpt_parse(const uint8_t *data, int data_len, int lun, int sector_size,
                   edl_partition_info_t *parts, int max_parts, unsigned flags);

/** 缓冲区中是否存在可识别的 GPT 主头（EFI PART + 版本号），用于日志区分「无签名」与「CRC 失败」 */
int edl_gpt_buffer_has_primary_header(const uint8_t *data, int data_len);

/**
 * Firehose 读/写 GPT 区常用扇区数（与 Bus Hound 抓包及 edl_firehose_read_gpt 一致）：
 * - 512 B/扇（eMMC）：34
 * - 4096 B/扇（UFS 等）：6
 */
int edl_gpt_firehose_gpt_region_sectors(int sector_size);

/**
 * 备份 GPT 区常用扇区数（不含 LBA0 保护性 MBR）：
 * - 512 B/扇（eMMC）：33
 * - 4096 B/扇（UFS 等）：5
 */
int edl_gpt_backup_region_sectors(int sector_size);

/**
 * 写入前重算主 GPT：PartitionEntryArrayCRC32（头内 0x58）与 Header CRC32（0x10）。
 * 缓冲区须为从 LBA0 起的完整 Primary GPT 区（含保护性 MBR），长度至少覆盖分区条目表末尾。
 * 成功返回 0；非标准布局返回 -1（调用方可仅打日志，不中止写入）。
 */
int edl_gpt_update_primary_crcs(uint8_t *gpt_data, int data_len, int sector_size);

/**
 * 备份 GPT 区（通常为磁盘末尾连续 6/34 扇区）：头在区域最后一扇区，条目在头之前。
 * region_start_lba 为该区域起始 LBA（与 rawprogram 的 start_sector 一致）。
 */
int edl_gpt_update_backup_region_crcs(uint8_t *buf, int len, int sector_size,
                                      int64_t region_start_lba);

/**
 * 根据 GPT 头中的 PartitionEntryLBA / 条目数 / 条目大小，计算从 LBA0 起解析所需的最小字节数。
 * 用于在「标准 6/34 扇区不足」时扩大 read。
 */
int edl_gpt_bytes_needed_for_primary_parse(const uint8_t *data, int data_len, int *out_need_bytes);

/**
 * （可选）在已解析出的 GPT 分区列表中补充「PrimaryGPT / BackupGPT」元数据区行（与 rawprogram 镜像区一致）。
 * 注意：gpttool 等工具只展示 GPT 条目内的分区，不包含此类合成行；全 LUN 合并时若每 LUN 都追加
 * 会与「条目数」对不齐。Firehose 读表默认不再调用此函数；刷 GPT 元数据仍用 XML/专用任务。
 *
 * @param inout_count  输入/输出：解析出的条目数；成功时可能增加 1 或 2。
 * @return 0 已处理（含无需追加）；-1 非 GPT 或参数无效。
 */
int edl_gpt_prepend_protective_partitions(const uint8_t *gpt_data, int gpt_len, int lun, int sector_size,
                                          edl_partition_info_t *parts, int *inout_count, int max_parts);

/*
 * Format a 16-byte GUID as string "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX".
 * buf must be at least 37 bytes.
 */
void edl_guid_to_str(const uint8_t *guid, char *buf);

#ifdef __cplusplus
}
#endif

#endif /* EDL_GPT_H */
