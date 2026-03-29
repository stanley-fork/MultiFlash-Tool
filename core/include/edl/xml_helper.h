#ifndef EDL_XML_HELPER_H
#define EDL_XML_HELPER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lightweight XML builder/parser for Firehose protocol.
 * No external dependencies - handles the subset of XML used by Qualcomm.
 */

/* ===== XML Response Parsing ===== */

#define EDL_XML_MAX_ATTRS 16
#define EDL_XML_ATTR_LEN  256

typedef struct {
    char name[64];
    char value[EDL_XML_ATTR_LEN];
} edl_xml_attr_t;

typedef struct {
    char            tag[64];         /* e.g. "response", "log", "configure" */
    edl_xml_attr_t  attrs[EDL_XML_MAX_ATTRS];
    int             attr_count;
    bool            is_ack;          /* value="ACK" */
    bool            is_nak;          /* value="NAK" */
    char            raw_value[EDL_XML_ATTR_LEN]; /* the "value" attribute */
} edl_xml_response_t;

/*
 * Parse a single XML element like <response value="ACK" MaxPayloadSizeToTargetInBytes="1048576"/>
 * Returns 0 on success, -1 on failure.
 */
int edl_xml_parse_response(const char *xml, edl_xml_response_t *resp);

/*
 * Find next <response .../> or <log .../> element in buffer.
 * Returns pointer to '<' of found element, or NULL.
 * Sets *end to char after '/>' of that element.
 */
const char *edl_xml_find_element(const char *buf, const char *tag, const char **end);

/*
 * Get attribute value from a parsed response. Returns NULL if not found.
 */
const char *edl_xml_get_attr(const edl_xml_response_t *resp, const char *name);

/*
 * Extract integer attribute. Returns default_val if not found.
 */
int edl_xml_get_attr_int(const edl_xml_response_t *resp, const char *name, int default_val);

/* ===== XML Building (Firehose commands) ===== */

int edl_xml_build_configure(char *buf, size_t buf_size,
                             const char *storage_type, int max_payload_size);

int edl_xml_build_read(char *buf, size_t buf_size,
                        int sector_size, int lun, int64_t start_sector,
                        int64_t num_sectors, const char *filename,
                        const char *label);

int edl_xml_build_program(char *buf, size_t buf_size,
                           int sector_size, int lun, int64_t start_sector,
                           int64_t num_sectors, const char *filename,
                           int64_t file_sector_offset, bool is_sparse);

/*
 * SakuraEDL 风格：filename + label（同名）+ 可选 read_back_verify。
 * 旧版仅 label 时 read_back_verify=false 且不含 filename（仍可用，不推荐）。
 */
int edl_xml_build_program_label(char *buf, size_t buf_size,
                                 int sector_size, int lun, int64_t start_sector,
                                 int64_t num_sectors, const char *label,
                                 bool read_back_verify, bool include_filename);

/*
 * 与 Qualcomm 工具 / Bus Hound 抓包一致：无 label、无 filename、无 Sparse。
 * read_back_verify：与 SakuraEDL 一致时可置 true（部分 loader 兼容）。
 */
int edl_xml_build_program_raw(char *buf, size_t buf_size,
                               int sector_size, int lun, int64_t start_sector,
                               int64_t num_sectors, bool read_back_verify);

/* 与抓包一致，但允许原样传递 start_sector 表达式（如 "NUM_DISK_SECTORS-5."） */
int edl_xml_build_program_raw_expr(char *buf, size_t buf_size,
                                   int sector_size, int lun, const char *start_sector_expr,
                                   int64_t num_sectors, bool read_back_verify);

int edl_xml_build_erase(char *buf, size_t buf_size,
                         int sector_size, int lun, int64_t start_sector,
                         int64_t num_sectors);

/* Qualcomm rawprogram: <zeroout .../> — not the same as <erase .../> */
int edl_xml_build_zeroout(char *buf, size_t buf_size,
                           int sector_size, int lun, int64_t start_sector,
                           int64_t num_sectors);

int edl_xml_build_patch(char *buf, size_t buf_size,
                         int sector_size, int lun, int64_t start_sector,
                         int byte_offset, int size_in_bytes, const char *value);

int edl_xml_build_nop(char *buf, size_t buf_size);

int edl_xml_build_power(char *buf, size_t buf_size, const char *mode);

int edl_xml_build_getstorageinfo(char *buf, size_t buf_size);

/* OPLUS VIP / Realme / OnePlus 等 Loader 扩展：查询 DRAM 类型（部分机型支持） */
int edl_xml_build_getddrtype(char *buf, size_t buf_size);

int edl_xml_build_setbootablestoragedrive(char *buf, size_t buf_size, int lun);

#ifdef __cplusplus
}
#endif

#endif /* EDL_XML_HELPER_H */
