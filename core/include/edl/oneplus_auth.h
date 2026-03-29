#ifndef EDL_ONEPLUS_AUTH_H
#define EDL_ONEPLUS_AUTH_H

#include "edl_types.h"
#include "edl_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OnePlus Demacia/SetProjModel 认证。
 * 注意: 与 OPLUS VIP 伪装、Realme 云端签名 都不同!
 *
 * OnePlus 使用本地加密认证:
 *   V1/V2: demacia → setprojmodel (OP5~OP9 系列)
 *   V3:    setprocstart → setswprojmodel (N10/N100 系列)
 */

/* 构建 getprjversion XML (获取设备 ProjectID) */
int edl_oneplus_build_getprjversion(char *buf, int buf_size);

/* 构建 setprocstart XML (V3 获取设备时间戳) */
int edl_oneplus_build_setprocstart(char *buf, int buf_size);

/* 查找设备配置 (version, 是否有 cm 参数) */
typedef struct {
    int  version;       /* 1, 2, 3 */
    char cm[16];        /* model hash key, 可为空 */
    int  param_mode;    /* 0 或 1 */
    bool found;
} edl_oneplus_config_t;

edl_oneplus_config_t edl_oneplus_lookup_config(const char *proj_id);

/*
 * 生成 Demacia 令牌 (V1/V2 第一步)。
 * serial: 芯片序列号十进制字符串
 * pk: 16 字符随机密钥
 * 输出: demacia_token_hex (至少 513 字节), pk 原样返回
 */
bool edl_oneplus_generate_demacia(const char *serial, const char *pk,
                                   char *demacia_hex, int hex_size);

/*
 * 生成 SetProjModel 令牌 (V1/V2 第二步)。
 * model_id: 设备配置中的 cm 或 proj_id
 * serial: 十进制序列号
 * pk: 与 demacia 相同的随机密钥
 * proj_id: 用于选择 prodkey
 * 输出: token_hex (至少 513 字节)
 */
bool edl_oneplus_generate_setprojmodel(const char *model_id, const char *serial,
                                        const char *pk, const char *proj_id,
                                        char *token_hex, int hex_size);

/*
 * 生成 SetSwProjModel 令牌 (V3)。
 * device_ts: setprocstart 返回的设备时间戳字符串
 */
bool edl_oneplus_generate_setswprojmodel(const char *model_id, const char *serial,
                                          const char *pk, const char *proj_id,
                                          const char *device_ts,
                                          char *token_hex, int hex_size);

/* 从 getprjversion 响应提取 ProjectID */
bool edl_oneplus_extract_projid(const char *response, char *proj_id, int size);

/* 从 setprocstart 响应提取 device_timestamp */
bool edl_oneplus_extract_timestamp(const char *response, char *ts, int size);

/* 判断认证响应是否成功 */
bool edl_oneplus_is_auth_success(const char *response);

/* 生成16字符随机 PK */
void edl_oneplus_generate_pk(char *pk, int size);

#ifdef __cplusplus
}
#endif

#endif /* EDL_ONEPLUS_AUTH_H */
