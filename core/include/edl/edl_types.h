#ifndef EDL_TYPES_H
#define EDL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Sahara Protocol Enums ===== */

typedef enum {
    SAHARA_CMD_HELLO             = 0x01,
    SAHARA_CMD_HELLO_RESP        = 0x02,
    SAHARA_CMD_READ_DATA         = 0x03,
    SAHARA_CMD_END_IMAGE_TX      = 0x04,
    SAHARA_CMD_DONE              = 0x05,
    SAHARA_CMD_DONE_RESP         = 0x06,
    SAHARA_CMD_RESET             = 0x07,
    SAHARA_CMD_RESET_RESP        = 0x08,
    SAHARA_CMD_MEMORY_DEBUG      = 0x09,
    SAHARA_CMD_MEMORY_READ       = 0x0A,
    SAHARA_CMD_COMMAND_READY     = 0x0B,
    SAHARA_CMD_SWITCH_MODE       = 0x0C,
    SAHARA_CMD_EXECUTE           = 0x0D,
    SAHARA_CMD_EXECUTE_DATA      = 0x0E,
    SAHARA_CMD_EXECUTE_RESP      = 0x0F,
    SAHARA_CMD_MEMORY_DEBUG64    = 0x10,
    SAHARA_CMD_MEMORY_READ64     = 0x11,
    SAHARA_CMD_READ_DATA64       = 0x12,
    SAHARA_CMD_RESET_STATE       = 0x13
} edl_sahara_cmd_t;

typedef enum {
    SAHARA_MODE_IMAGE_TX_PENDING  = 0x0,
    SAHARA_MODE_IMAGE_TX_COMPLETE = 0x1,
    SAHARA_MODE_MEMORY_DEBUG      = 0x2,
    SAHARA_MODE_COMMAND           = 0x3
} edl_sahara_mode_t;

typedef enum {
    SAHARA_EXEC_SERIAL_NUM       = 0x01,
    SAHARA_EXEC_MSM_HWID         = 0x02,
    SAHARA_EXEC_OEM_PK_HASH      = 0x03,
    SAHARA_EXEC_SBL_INFO         = 0x06,
    SAHARA_EXEC_SBL_SW_VER       = 0x07,
    SAHARA_EXEC_PBL_SW_VER       = 0x08,
    SAHARA_EXEC_CHIP_ID_V3       = 0x0A,
    SAHARA_EXEC_SERIAL_NUM64     = 0x14
} edl_sahara_exec_cmd_t;

typedef enum {
    SAHARA_STATUS_SUCCESS                = 0x00,
    SAHARA_STATUS_INVALID_CMD            = 0x01,
    SAHARA_STATUS_PROTOCOL_MISMATCH      = 0x02,
    SAHARA_STATUS_INVALID_TARGET_PROTO   = 0x03,
    SAHARA_STATUS_INVALID_HOST_PROTO     = 0x04,
    SAHARA_STATUS_INVALID_PACKET_SIZE    = 0x05,
    SAHARA_STATUS_UNEXPECTED_IMAGE_ID    = 0x06,
    SAHARA_STATUS_INVALID_HEADER_SIZE    = 0x07,
    SAHARA_STATUS_INVALID_DATA_SIZE      = 0x08,
    SAHARA_STATUS_INVALID_IMAGE_TYPE     = 0x09,
    SAHARA_STATUS_INVALID_TX_LEN         = 0x0A,
    SAHARA_STATUS_INVALID_RX_LEN         = 0x0B,
    SAHARA_STATUS_GENERAL_TX_RX_ERROR    = 0x0C,
    SAHARA_STATUS_READ_DATA_ERROR        = 0x0D,
    SAHARA_STATUS_RECEIVE_TIMEOUT        = 0x16,
    SAHARA_STATUS_TRANSMIT_TIMEOUT       = 0x17,
    SAHARA_STATUS_HASH_AUTH_FAIL         = 0x21,
    SAHARA_STATUS_HASH_VERIFY_FAIL       = 0x22,
    SAHARA_STATUS_HASH_TABLE_NOT_FOUND   = 0x23,
    SAHARA_STATUS_CMD_EXEC_FAIL          = 0x1D,
    SAHARA_STATUS_ACCESS_DENIED          = 0x1F
} edl_sahara_status_t;

/* ===== Sahara Packet Structures (little-endian, packed) ===== */

#pragma pack(push, 1)

typedef struct {
    uint32_t cmd;
    uint32_t len;
} edl_sahara_pkt_hdr_t;

typedef struct {
    uint32_t cmd;        /* SAHARA_CMD_HELLO */
    uint32_t len;        /* 48 */
    uint32_t version;
    uint32_t version_supported;
    uint32_t status;
    uint32_t mode;
    uint32_t reserved[6];
} edl_sahara_hello_t;

typedef struct {
    uint32_t cmd;        /* SAHARA_CMD_HELLO_RESP */
    uint32_t len;        /* 48 */
    uint32_t version;
    uint32_t version_supported;
    uint32_t status;
    uint32_t mode;
    uint32_t reserved[6];
} edl_sahara_hello_resp_t;

typedef struct {
    uint32_t cmd;        /* SAHARA_CMD_READ_DATA */
    uint32_t len;        /* 20 */
    uint32_t image_id;
    uint32_t offset;
    uint32_t length;
} edl_sahara_read_data_t;

typedef struct {
    uint32_t cmd;        /* SAHARA_CMD_READ_DATA64 */
    uint32_t len;        /* 32 */
    uint64_t image_id;
    uint64_t offset;
    uint64_t length;
} edl_sahara_read_data64_t;

typedef struct {
    uint32_t cmd;        /* SAHARA_CMD_END_IMAGE_TX */
    uint32_t len;
    uint32_t image_id;
    uint32_t status;
} edl_sahara_end_image_t;

typedef struct {
    uint32_t cmd;        /* SAHARA_CMD_EXECUTE */
    uint32_t len;        /* 12 */
    uint32_t exec_cmd;
} edl_sahara_execute_t;

typedef struct {
    uint32_t cmd;        /* SAHARA_CMD_EXECUTE_DATA */
    uint32_t len;
    uint32_t exec_cmd;
    uint32_t data_len;
} edl_sahara_exec_data_t;

#pragma pack(pop)

/* ===== Chip Info ===== */

#define EDL_PK_HASH_MAX_LEN   144
#define EDL_CHIP_NAME_MAX_LEN  64
#define EDL_VENDOR_MAX_LEN     64
#define EDL_SERIAL_MAX_LEN     20

typedef struct {
    char     serial_hex[EDL_SERIAL_MAX_LEN];
    uint32_t serial_dec;
    char     hwid_hex[20];
    uint32_t msm_id;
    uint16_t oem_id;
    uint16_t model_id;
    char     pk_hash[EDL_PK_HASH_MAX_LEN * 2 + 1];
    char     chip_name[EDL_CHIP_NAME_MAX_LEN];
    char     vendor[EDL_VENDOR_MAX_LEN];
    uint32_t sahara_version;
} edl_chip_info_t;

/* 从 system/vendor 等分区（EXT4/EROFS）解析的 Android 属性（build.prop） */
typedef struct {
    char fs_type[32];
    char volume_label[32];
    char brand[128];
    char manufacturer[128];
    char market_name[128];
    char locale[32];
    char region_mark[64];
    char region_type[64];
    char model[128];
    char device[128];
    char product[128];
    char android_release[64];
    char fingerprint[256];
    char security_patch[64];   /* ro.build.version.security_patch */
    char build_id[96];         /* ro.build.id */
    char incremental[96];      /* ro.build.version.incremental */
    char display_id[128];      /* ro.build.display.id */
    char build_date[128];      /* ro.build.date */
    char build_date_utc[32];   /* ro.build.date.utc */
    char build_type[32];       /* ro.build.type */
    char build_tags[64];       /* ro.build.tags */
    char miui_version[64];     /* ro.miui.ui.version.name */
    char sdk[16];              /* ro.build.version.sdk */
    char ota_version[128];     /* ro.build.version.ota */
    char display_ota[128];     /* ro.build.display.ota */
    char display_full_id[160]; /* ro.build.display.full_id */
    char common_ota[96];       /* ro.commonsoft.ota */
    char project_number[64];   /* ro.separate.soft / ro.product.supported_versions */
    char auth_project[96];     /* ro.product.authentication */
    char hardware_code[64];    /* ro.product.hw */
    char nv_id[64];            /* ro.build.oplus_nv_id */
    char pipeline_key[64];     /* ro.oplus.pipeline_key */
    char base_version[128];    /* ro.oplus.version.base */
    char source_partition[96];
    /** 文件系统在 GPT 分区/super 镜像内的起始偏移；0 表示从分区首字节即 FS */
    int64_t fs_embed_offset;
} edl_android_props_t;

/* ===== Partition Info ===== */

/* GPT 分区名为 UTF-16LE，转 UTF-8 后可能达约 4×36 字节；留余量 */
#define EDL_PART_NAME_MAX  192
#define EDL_GUID_STR_LEN   37

typedef struct {
    int      lun;
    char     name[EDL_PART_NAME_MAX];
    int64_t  start_sector;
    char     start_sector_expr[64];
    int64_t  num_sectors;
    int      sector_size;
    /* rawprogram.xml: skip this many sectors from the start of the image file before programming */
    int64_t  file_sector_offset;
    char     type_guid[EDL_GUID_STR_LEN];
    char     unique_guid[EDL_GUID_STR_LEN];
    uint64_t attributes;
    int      entry_index;
} edl_partition_info_t;

/* ===== GPT Structures ===== */

typedef struct {
    char     signature[9];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    char     disk_guid[EDL_GUID_STR_LEN];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_entry_crc32;
    bool     is_valid;
    bool     crc_valid;
    int      sector_size;
} edl_gpt_header_t;

typedef struct {
    edl_gpt_header_t     header;
    edl_partition_info_t *partitions;
    int                   partition_count;
    int                   lun;
    bool                  success;
    char                  error[256];
} edl_gpt_result_t;

/* ===== Flash Task (rawprogram.xml) ===== */

typedef enum {
    EDL_TASK_PROGRAM,
    EDL_TASK_PATCH,
    EDL_TASK_ERASE,
    EDL_TASK_ZEROOUT
} edl_task_type_t;

typedef struct {
    char            label[EDL_PART_NAME_MAX];
    char            filename[260];
    char            filepath[260];
    int             lun;
    int64_t         start_sector;
    char            start_sector_expr[64];
    int64_t         num_sectors;
    int             sector_size;
    int64_t         file_offset;
    int64_t         file_sector_offset;
    bool            is_sparse;
    edl_task_type_t type;
} edl_flash_task_t;

typedef struct {
    int     lun;
    int64_t start_sector;
    int     byte_offset;
    int     size_in_bytes;
    char    value[64];
    char    what[128];
    char    filename[260];
} edl_patch_entry_t;

/* ===== Callbacks ===== */

typedef void (*edl_log_cb)(const char *msg, void *user_data);
typedef void (*edl_progress_cb)(int64_t current, int64_t total, void *user_data);
typedef bool (*edl_cancel_cb)(void *user_data);

typedef struct {
    edl_log_cb      log;
    /** 可选；为 NULL 时不输出调试级信息（且不会回退到 log） */
    edl_log_cb      log_detail;
    edl_progress_cb progress;
    /** 可选；返回 true 表示用户已请求取消，核心层会在分块循环间检查并提前返回 EDL_ERR_CANCELLED */
    edl_cancel_cb   is_cancelled;
    void           *user_data;
} edl_callbacks_t;

/* ===== Detected Port ===== */

typedef enum {
    EDL_PORT_UNKNOWN,
    EDL_PORT_EDL_9008,
    EDL_PORT_DLOAD_9006,
    EDL_PORT_DIAG_9091,
    EDL_PORT_OTHER
} edl_port_type_t;

typedef struct {
    char            port_name[16];
    char            description[128];
    char            device_id[256];
    edl_port_type_t type;
} edl_detected_port_t;

#ifdef __cplusplus
}
#endif

#endif /* EDL_TYPES_H */
