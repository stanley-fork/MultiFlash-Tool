#ifndef EDL_STORAGE_REPORT_H
#define EDL_STORAGE_REPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 将 Firehose getstorageinfo 返回的整段 XML（含多行 <log value="..."/>）格式化为「字库设备信息」报告。
 * report 为 UTF-8 文本，以换行符分隔；若无法识别字段会保留「原始日志」段落。
 */
int edl_storage_build_device_report(const char *firehose_rx_xml, char *report, size_t report_size);

/*
 * Reuse the storage-report parser to extract LUN hints from getstorageinfo XML.
 * Returns 1 when at least one hint was found, otherwise 0.
 */
int edl_storage_extract_lun_hints(const char *firehose_rx_xml,
                                  int *lun_count,
                                  uint32_t *lun_enable_mask);

#ifdef __cplusplus
}
#endif

#endif
