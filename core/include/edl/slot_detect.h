#ifndef EDL_SLOT_DETECT_H
#define EDL_SLOT_DETECT_H

#include "edl_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 与 SakuraEDL QualcommUIController.WritePartitionsBatchAsync / FirehoseClient.MergeSlotInfo 对齐：
 * 根据 GPT 分区属性（Android A/B 标志位）与可选的「本次写入」分区名统计，选择 setbootablestoragedrive 的目标 LUN。
 *
 * @param storage_type edl_service_storage_type()，如 "ufs" / "emmc"
 * @param parts 回读 GPT 后的分区列表（可跨 LUN）
 * @param count 分区数量
 * @param wrote_a_count 本次写入任务中分区名以 _a 结尾的数量（可为 0）
 * @param wrote_b_count 本次写入任务中分区名以 _b 结尾的数量
 * @param detail_buf 可选，写入简短说明（如 "slot_a -> LUN1"）
 * @param detail_len detail_buf 大小
 * @return 要传给 setbootablestoragedrive 的 LUN；若应跳过（无 A/B 等）返回 -1
 */
int edl_boot_lun_pick_sakura(const char *storage_type,
                             const edl_partition_info_t *parts, int count,
                             int wrote_a_count, int wrote_b_count,
                             char *detail_buf, size_t detail_len);

#ifdef __cplusplus
}
#endif

#endif /* EDL_SLOT_DETECT_H */
