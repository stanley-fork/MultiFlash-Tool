#ifndef EDL_CHIP_DB_H
#define EDL_CHIP_DB_H

#include "edl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 查询芯片名称 (带 Snapdragon 代号)，返回静态字符串，未找到返回 "Unknown" */
const char *edl_chip_name(uint32_t msm_id);

/* 查询芯片代号 (不含括号)，未找到返回 NULL */
const char *edl_chip_codename(uint32_t msm_id);
const char *edl_chip_codename_precise(uint32_t msm_id);

/* 根据 OEM ID 查询厂商名，未找到返回 NULL */
const char *edl_vendor_by_oem(uint16_t oem_id);

/* 根据 PK Hash 前8字符查询厂商，未找到返回 NULL */
const char *edl_vendor_by_pk_hash(const char *pk_hash);

typedef enum {
    EDL_BRAND_SOURCE_NONE = 0,
    EDL_BRAND_SOURCE_PK_HASH,
    EDL_BRAND_SOURCE_OEM_MODEL,
    EDL_BRAND_SOURCE_OEM_FAMILY
} edl_brand_source_t;

/* 综合 PK Hash / OEM / Model 解析更精确的品牌名，未找到返回 OEM family 或 NULL */
const char *edl_brand_by_ids(uint16_t oem_id, uint16_t model_id, const char *pk_hash);
const char *edl_brand_by_ids_ex(uint16_t oem_id, uint16_t model_id, const char *pk_hash,
                                edl_brand_source_t *source);
const char *edl_brand_source_name(edl_brand_source_t source);

/* 判断是否需要 VIP (OPLUS) 伪装 */
bool edl_requires_vip(const char *pk_hash);

/* 判断是否为 OnePlus 设备 */
bool edl_is_oneplus(const char *pk_hash);

/* 判断是否为小米设备 */
bool edl_is_xiaomi(const char *pk_hash);

/* Realme 云端认证：SM8350(888) 及更新旗舰平台用新协议(Modern)，此前用旧协议 */
bool edl_realme_is_modern_platform(uint32_t msm_id);

typedef enum {
    EDL_MEM_UFS,
    EDL_MEM_EMMC,
    EDL_MEM_NAND,
    EDL_MEM_UNKNOWN
} edl_memory_type_t;

edl_memory_type_t edl_guess_memory_type(uint32_t msm_id);
const char *edl_memory_type_name(edl_memory_type_t type);

typedef enum {
    EDL_LOADER_ARCH_HINT_UNKNOWN = 0,
    EDL_LOADER_ARCH_HINT_32,
    EDL_LOADER_ARCH_HINT_64
} edl_loader_arch_hint_t;

const char *edl_loader_arch_hint_name(edl_loader_arch_hint_t hint);

typedef enum {
    EDL_AUTH_HINT_UNKNOWN = 0,
    EDL_AUTH_HINT_NONE,
    EDL_AUTH_HINT_OPLUS_VIP,
    EDL_AUTH_HINT_REALME_LEGACY,
    EDL_AUTH_HINT_REALME_MODERN,
    EDL_AUTH_HINT_XIAOMI_BUILTIN,
    EDL_AUTH_HINT_ONEPLUS
} edl_auth_hint_t;

const char *edl_auth_hint_name(edl_auth_hint_t hint);

typedef struct {
    uint32_t msm_id;
    char     soc_code[64];
    char     marketing_name[96];
    char     chip_name[EDL_CHIP_NAME_MAX_LEN];
    char     codename[64];
    char     precise_codename[64];
    edl_memory_type_t      memory_type;
    edl_loader_arch_hint_t loader_arch_hint;
    edl_auth_hint_t        auth_hint;
} edl_platform_profile_t;

bool edl_query_platform_profile(uint32_t msm_id, uint16_t oem_id, uint16_t model_id,
                                const char *pk_hash, edl_platform_profile_t *out);
const char *edl_device_marketing_name_by_model(const char *brand, const char *model);

/**
 * 根据 MSM ID 推断 DRAM 代际（LPDDR4/5 等公开市场常见搭配）。
 * 注意：不是 SPD/颗粒丝印；单板可能混用不同规格，仅供参考。
 */
const char *edl_guess_ddr_generation(uint32_t msm_id);

/* PK Hash 安全信息字符串，buf 至少 64 字节 */
void edl_pk_hash_info(const char *pk_hash, char *buf, int buf_size);

#ifdef __cplusplus
}
#endif

#endif /* EDL_CHIP_DB_H */
