#ifndef EDL_SERVICE_H
#define EDL_SERVICE_H

#include "edl_types.h"
#include "edl_error.h"
#include "edl/realme_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edl_service edl_service_t;

typedef struct {
    bool     valid;
    bool     is_elf;
    uint8_t  elf_class;
    uint8_t  elf_data;
    uint8_t  elf_version;
    uint16_t machine;
    uint64_t entry;
    char     qc_version[96];
    char     oem_version[96];
    char     variant[96];
} edl_loader_info_t;

typedef struct {
    bool     read_storage_report;
    bool     ensure_gpt_cache;
    bool     read_android_props;
    int      storage_report_timeout_ms;
    int      storage_report_retries;
    int      gpt_timeout_ms;
    int      gpt_retries;
    int      android_props_timeout_ms;
    int      android_props_retries;
    int      retry_delay_ms;
    int      gpt_max_lun;
    unsigned gpt_flags;
} edl_device_query_options_t;

typedef struct {
    char                 *storage_report;
    size_t                storage_report_size;
    edl_partition_info_t *gpt_parts;
    int                  *gpt_count;
    edl_android_props_t  *android_props;
} edl_device_query_result_t;

/* 与主界面「认证类型」对应；在连接流程中自动执行 */
typedef enum {
    EDL_SVC_AUTH_NONE = 0,
    /* OPLUS VIP：本地 Digest+Signature 串口流程 (EnableVip=1)，与 Realme 云端 RCSMAUTH 无关 */
    EDL_SVC_AUTH_OPLUS_VIP,
    /* Realme：Sahara 首次连接时走 getsigndata→云端签名→verify；已在 Firehose 时连接流程跳过串口认证 */
    EDL_SVC_AUTH_REALME,
    EDL_SVC_AUTH_ONEPLUS,
    EDL_SVC_AUTH_XIAOMI
} edl_svc_auth_mode_t;

typedef struct {
    edl_svc_auth_mode_t mode;
    char digest_path[512];
    char signature_path[512];
    char project_id[64];
    edl_realme_sign_cb realme_sign_cb;
    void              *realme_sign_user;
} edl_svc_auth_options_t;

/* ===== Lifecycle ===== */
edl_service_t *edl_service_create(const edl_callbacks_t *cb);
void            edl_service_destroy(edl_service_t *svc);

/* ===== Device Connection ===== */

/* Detect and open EDL port, run Sahara handshake, upload Firehose loader.
 * loader_path: path to the Firehose programmer (.elf/.mbn) */
edl_error_t edl_service_connect(edl_service_t *svc, const char *loader_path,
                                 const char *storage_type);

/* 带认证选项（auth 可为 NULL，等价于无认证） */
edl_error_t edl_service_connect_ex(edl_service_t *svc, const char *loader_path,
                                    const char *storage_type,
                                    const edl_svc_auth_options_t *auth);

/* Connect to a specific port (skip auto-detection) */
edl_error_t edl_service_connect_port(edl_service_t *svc, const char *port_name,
                                      const char *loader_path, const char *storage_type);

edl_error_t edl_service_connect_port_ex(edl_service_t *svc, const char *port_name,
                                         const char *loader_path, const char *storage_type,
                                         const edl_svc_auth_options_t *auth);

/* Disconnect and clean up */
void edl_service_disconnect(edl_service_t *svc);

/* Check if connected */
bool edl_service_is_connected(const edl_service_t *svc);

/* ===== Device Info ===== */

const edl_chip_info_t *edl_service_chip_info(const edl_service_t *svc);
const edl_loader_info_t *edl_service_loader_info(const edl_service_t *svc);
const char            *edl_service_port_name(const edl_service_t *svc);
const char            *edl_service_storage_type(const edl_service_t *svc);
const char            *edl_service_session_mode(const edl_service_t *svc);
int                    edl_service_sector_size(const edl_service_t *svc);
int                    edl_service_max_payload_bytes(const edl_service_t *svc);

/* Firehose: 查询设备存储信息（设备日志通过回调输出） */
edl_error_t edl_service_get_storage_info(edl_service_t *svc);

/* Firehose getstorageinfo：解析为「字库设备信息」多行报告（UTF-8） */
edl_error_t edl_service_get_storage_device_report(edl_service_t *svc, char *report, size_t report_size);
void edl_device_query_options_init(edl_device_query_options_t *options);
edl_error_t edl_service_collect_device_query(edl_service_t *svc,
                                             const edl_device_query_options_t *options,
                                             const edl_device_query_result_t *result);

/**
 * 从 GPT 中 system/vendor 等分区读取镜像前缀，用 EXT4/EROFS 解析 build.prop（需 Firehose 已连接）。
 * 成功时填充 out（含 source_partition、fs_type）；失败时 out 清零。
 */
edl_error_t edl_service_probe_android_build_props(edl_service_t *svc, edl_android_props_t *out);

/* Firehose NOP/ping，用于操作前或批量过程中检测链路 */
edl_error_t edl_service_ping(edl_service_t *svc);

/* ===== Partition Operations ===== */

/* Read GPT from all LUNs. parts[] must be pre-allocated. */
/* GPT read flags. Default read/cache APIs are safe and do not enable these flags. */
/* Deprecated: kept only for compatibility. The service no longer auto-sends
 * setbootablestoragedrive during GPT reads; boot activation is manual or patch-only. */
#define EDL_SERVICE_GPT_READ_ALLOW_SETBOOTABLE_FALLBACK 0x01u

edl_error_t edl_service_read_gpt(edl_service_t *svc,
                                  edl_partition_info_t *parts, int *count, int max_lun);
edl_error_t edl_service_read_gpt_ex(edl_service_t *svc,
                                    edl_partition_info_t *parts, int *count, int max_lun,
                                    unsigned flags);

/* 将 GPT 读入服务内部缓存：已有缓存则直接返回；否则走与「读取分区表」按钮相同的 edl_service_read_gpt 路径 */
edl_error_t edl_service_ensure_gpt_cache(edl_service_t *svc);
edl_error_t edl_service_ensure_gpt_cache_ex(edl_service_t *svc, unsigned flags);

/* 复制内部 GPT 缓存到调用方缓冲区（需已成功加载缓存） */
edl_error_t edl_service_copy_cached_gpt(edl_service_t *svc,
                                        edl_partition_info_t *parts, int *count);

/* Find a partition by name (case-insensitive). Returns partition or NULL. */
const edl_partition_info_t *edl_service_find_partition(edl_service_t *svc,
                                                        const char *name);

/* 是否已成功从设备解析 GPT 并缓存（与分区表 UI/XML 是否一致无关） */
bool edl_service_is_gpt_cache_loaded(const edl_service_t *svc);

/* Read a partition to file */
edl_error_t edl_service_read_partition(edl_service_t *svc,
                                        const edl_partition_info_t *part,
                                        const char *save_path);

/* Read partition to memory buffer */
edl_error_t edl_service_read_partition_mem(edl_service_t *svc,
                                            const edl_partition_info_t *part,
                                            uint8_t **out_data, int *out_len);

/* 与 edl_firehose_set_write_options 一致（连接后、写入前调用） */
void edl_service_set_write_options(edl_service_t *svc,
                                   bool pad_short_image_to_gpt,
                                   bool program_read_back_verify);

/* Write a partition from file */
edl_error_t edl_service_write_partition(edl_service_t *svc,
                                         const edl_partition_info_t *part,
                                         const char *image_path);

/* Write partition from memory buffer */
edl_error_t edl_service_write_partition_mem(edl_service_t *svc,
                                             const edl_partition_info_t *part,
                                             const uint8_t *data, int data_len);

/* Erase a partition */
edl_error_t edl_service_erase_partition(edl_service_t *svc,
                                         const edl_partition_info_t *part);

/* Erase a partition by name (reads GPT if needed) */
edl_error_t edl_service_erase_partition_by_name(edl_service_t *svc, const char *name);

/* Erase FRP (Factory Reset Protection) */
edl_error_t edl_service_erase_frp(edl_service_t *svc);

/* Erase userdata */
edl_error_t edl_service_erase_userdata(edl_service_t *svc);

/* Repair GPT after writes/patches (Firehose fixgpt，与 SakuraEDL FixGptAsync 一致) */
edl_error_t edl_service_fix_gpt(edl_service_t *svc);

/* 发送 setbootablestoragedrive（手动按钮路径使用）；失败不致命，仅日志。 */
void edl_service_try_set_bootable_storage_drive(edl_service_t *svc, int lun);

/* 与 SakuraEDL 一致：根据回读 GPT + 本次写入 _a/_b 统计选择 LUN 并激活启动分区。 */
void edl_service_activate_boot_lun_sakura(edl_service_t *svc,
                                          const edl_partition_info_t *parts, int count,
                                          int wrote_a_count, int wrote_b_count);

/* 仅应用 patch*.xml（批量写入后、fix_gpt 前调用；与 flash_xml 内 patch 逻辑一致） */
edl_error_t edl_service_apply_patch_file(edl_service_t *svc, const char *patch_path);

/* ===== Flash from XML ===== */

/* edl_service_flash_xml_ex flags */
#define EDL_FLASH_XML_RUN_FIXGPT   0x01u
/* End-of-flash boot activation: only honored after patch XML was actually applied.
 * Partition-table read/write/erase paths do not use this flag. */
#define EDL_FLASH_XML_SET_BOOTABLE_AT_END  0x02u

/* Execute rawprogram + optional patch.
 * EDL_FLASH_XML_RUN_FIXGPT runs fixgpt at the end.
 * EDL_FLASH_XML_SET_BOOTABLE_AT_END activates boot storage only after patch XML succeeds. */
edl_error_t edl_service_flash_xml_ex(edl_service_t *svc,
                                      const char *rawprogram_path,
                                      const char *patch_path,
                                      const char *base_dir,
                                      unsigned flags);

/* 等价于 edl_service_flash_xml_ex(..., EDL_FLASH_XML_RUN_FIXGPT) */
edl_error_t edl_service_flash_xml(edl_service_t *svc,
                                   const char *rawprogram_path,
                                   const char *patch_path,
                                   const char *base_dir);

/* 仅从 rawprogram XML 中筛选 PrimaryGPT / BackupGPT 的 PROGRAM 项并写入（需 filename 指向镜像；base_dir 与刷机一致） */
edl_error_t edl_service_write_gpt_from_rawprogram_xml(edl_service_t *svc,
                                                       const char *rawprogram_path,
                                                       const char *base_dir,
                                                       unsigned flags);

/* ===== Power Control ===== */

/**
 * 将镜像完整写入 MISC（或指定 GPT 分区名）后发送 power reset。
 * 用于「重启到 Fastboot / Recovery」：先写入厂商要求的 misc 内容再正常重启。
 * @param part_name 可为 NULL，则依次尝试 misc / MISC
 */
edl_error_t edl_service_misc_write_image_and_reset(edl_service_t *svc,
                                                   const char *image_path,
                                                   const char *part_name);

edl_error_t edl_service_reboot(edl_service_t *svc, const char *mode);

/* ===== Detected Port Listing ===== */

int edl_service_detect_ports(edl_detected_port_t *ports, int max_ports);

#ifdef __cplusplus
}
#endif

#endif /* EDL_SERVICE_H */
