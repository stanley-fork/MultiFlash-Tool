#ifndef EDL_FIREHOSE_H
#define EDL_FIREHOSE_H

#include <stdbool.h>

#include "edl_types.h"
#include "edl_error.h"
#include "serial_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edl_firehose edl_firehose_t;

edl_firehose_t *edl_firehose_create(edl_port_t *port, const edl_callbacks_t *cb);
void            edl_firehose_destroy(edl_firehose_t *ctx);

/* Configure Firehose session */
edl_error_t edl_firehose_configure(edl_firehose_t *ctx, const char *storage_type,
                                    int preferred_payload_size);

/* Ping / NOP */
edl_error_t edl_firehose_ping(edl_firehose_t *ctx);

/* Get storage info */
edl_error_t edl_firehose_get_storage_info(edl_firehose_t *ctx);

/* 采集 getstorageinfo 完整 XML 并格式化为「字库设备信息」报告（UTF-8） */
edl_error_t edl_firehose_get_storage_device_report(edl_firehose_t *ctx, char *report, size_t report_size);

/*
 * 发送 <getddrtype />（部分 OPLUS/Realme/OP Loader 支持）。
 * 成功时 out 为简短摘要；失败时 out 可为空。非致命，供上层在认证设备上调用。
 */
edl_error_t edl_firehose_try_get_ddr_type(edl_firehose_t *ctx, char *out, size_t out_size);

/* Read GPT partition table from a given LUN.
 * Caller allocates parts[] and sets *count to capacity. On return *count = actual. */
edl_error_t edl_firehose_read_gpt(edl_firehose_t *ctx, int lun,
                                   edl_partition_info_t *parts, int *count);

/* Read all partitions from all LUNs (UFS: 0-5) */
edl_error_t edl_firehose_read_all_gpt(edl_firehose_t *ctx,
                                       edl_partition_info_t *parts, int *count, int max_lun);

/* Read raw sectors to memory buffer */
edl_error_t edl_firehose_read_sectors(edl_firehose_t *ctx, int lun,
                                       int64_t start_sector, int num_sectors,
                                       uint8_t **out_data, int *out_len);

/* Read partition data to file */
edl_error_t edl_firehose_read_partition(edl_firehose_t *ctx,
                                         const edl_partition_info_t *part,
                                         const char *save_path);

/*
 * 写入选项（会话级，默认：补满 GPT=开，回读校验=开）。
 * pad_short_image_to_gpt：镜像短于 GPT 声明时是否用 0 补满整段分区（关=仅按镜像长度，与 SakuraEDL 一致）。
 */
void edl_firehose_set_write_options(edl_firehose_t *ctx,
                                    bool pad_short_image_to_gpt,
                                    bool program_read_back_verify);

/* Write partition from file */
edl_error_t edl_firehose_write_partition(edl_firehose_t *ctx,
                                          const edl_partition_info_t *part,
                                          const char *image_path);

/* Erase partition */
edl_error_t edl_firehose_erase_partition(edl_firehose_t *ctx,
                                          const edl_partition_info_t *part);

/* Write partition from memory buffer（含分区名用于 program label；GPT 分区按抓包补齐扇区） */
edl_error_t edl_firehose_write_partition_mem(edl_firehose_t *ctx,
                                              const edl_partition_info_t *part,
                                              const uint8_t *data, int data_len);

/* Read partition to memory buffer */
edl_error_t edl_firehose_read_partition_mem(edl_firehose_t *ctx,
                                             const edl_partition_info_t *part,
                                             uint8_t **out_data, int *out_len);

/* Apply a single patch */
edl_error_t edl_firehose_apply_patch(edl_firehose_t *ctx, int lun,
                                      int64_t start_sector, int byte_offset,
                                      int size_in_bytes, const char *value);

/* Repair GPT (Primary/Backup sync + CRC). lun < 0 => all LUNs (Firehose lun="all"). */
edl_error_t edl_firehose_fix_gpt(edl_firehose_t *ctx, int lun, bool grow_last_partition);

/* UFS 常见：将可启动存储设为指定 LUN（多为 0）。失败不致命，仅日志。 */
void edl_firehose_try_set_bootable_storage_drive(edl_firehose_t *ctx, int lun);

/* Power control */
edl_error_t edl_firehose_reboot(edl_firehose_t *ctx, const char *mode);

/* Send raw XML command and get response */
edl_error_t edl_firehose_send_xml(edl_firehose_t *ctx, const char *xml,
                                   char *response_buf, size_t response_buf_size);

/* Properties */
int         edl_firehose_sector_size(const edl_firehose_t *ctx);
int         edl_firehose_max_payload(const edl_firehose_t *ctx);
const char *edl_firehose_storage_type(const edl_firehose_t *ctx);
int         edl_firehose_max_lun_hint(const edl_firehose_t *ctx);

/* Configure/GetStorageInfo 等返回的 XML 中解析出的 MSM HWID（无 Sahara 时供 Realme 认证）；0 表示未知 */
uint32_t    edl_firehose_msm_hwid_hint(const edl_firehose_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EDL_FIREHOSE_H */
