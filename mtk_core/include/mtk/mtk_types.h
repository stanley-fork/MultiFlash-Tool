#ifndef MTK_TYPES_H
#define MTK_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MTK_API
#define MTK_API
#endif

#define MTK_HWCODE_UNKNOWN 0u
#define MTK_MAX_PARTITIONS 512
#define MTK_NAME_MAX 64
#define MTK_PATH_MAX 260

typedef enum mtk_boot_stage {
    MTK_BOOT_STAGE_UNKNOWN = 0,
    MTK_BOOT_STAGE_BROM,
    MTK_BOOT_STAGE_PRELOADER,
    MTK_BOOT_STAGE_DA,
} mtk_boot_stage_t;

typedef enum mtk_da_mode {
    MTK_DA_MODE_UNKNOWN = 0,
    MTK_DA_MODE_LEGACY,
    MTK_DA_MODE_XFLASH,
    MTK_DA_MODE_XMLFLASH,
} mtk_da_mode_t;

typedef enum mtk_storage_type {
    MTK_STORAGE_UNKNOWN = 0,
    MTK_STORAGE_EMMC,
    MTK_STORAGE_UFS,
    MTK_STORAGE_NAND,
    MTK_STORAGE_NOR,
} mtk_storage_type_t;

typedef enum mtk_log_level {
    MTK_LOG_TRACE = 0,
    MTK_LOG_DEBUG,
    MTK_LOG_INFO,
    MTK_LOG_WARN,
    MTK_LOG_ERROR,
} mtk_log_level_t;

typedef struct mtk_device_info {
    char port_name[64];
    uint32_t hwcode;
    uint32_t hwsubcode;
    uint32_t hwver;
    uint32_t swver;
    mtk_boot_stage_t boot_stage;
    mtk_storage_type_t storage_type;
    uint32_t block_size;
    uint64_t total_size;
} mtk_device_info_t;

typedef struct mtk_security_state {
    int secure_boot;
    int sla_required;
    int daa_required;
    int auth_required;
    int can_upload_da;
    int can_read_flash;
    int can_write_flash;
    int can_erase_flash;
    int brom_sync_ok;
    int preloader_mode;
} mtk_security_state_t;

typedef struct mtk_capabilities {
    int can_connect;
    int can_detect_stage;
    int can_upload_da;
    int can_read_flash;
    int can_write_flash;
    int can_erase_flash;
    int can_reboot;
    int can_read_partitions;
    int can_write_partitions;
    int can_erase_partitions;
} mtk_capabilities_t;

typedef struct mtk_partition_info {
    char name[MTK_NAME_MAX];
    uint32_t region;
    uint64_t start_lba;
    uint64_t lba_count;
    uint32_t block_size;
    uint64_t byte_size;
} mtk_partition_info_t;

typedef struct mtk_connect_options {
    const char* port_name;
    const char* auth_path;
    const char* cert_path;
    const char* da_path;
    mtk_da_mode_t preferred_da_mode;
    int auto_detect_port;
} mtk_connect_options_t;

typedef void (*mtk_log_fn)(void* user, mtk_log_level_t level, const char* message);
typedef void (*mtk_progress_fn)(void* user, uint64_t current, uint64_t total);

typedef struct mtk_callbacks {
    void* user;
    mtk_log_fn log;
    mtk_progress_fn progress;
} mtk_callbacks_t;

#ifdef __cplusplus
}
#endif

#endif
