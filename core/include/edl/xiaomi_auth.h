#ifndef EDL_XIAOMI_AUTH_H
#define EDL_XIAOMI_AUTH_H

#include "edl_types.h"
#include "edl_error.h"
#include "serial_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 小米认证 (MiAuth) — 两阶段:
 *   1. 尝试预置签名库绕过 (edlclient 签名)
 *   2. 若失败，获取 Challenge Token (VQ 开头 Base64) 供在线授权
 *
 * 注意: 与 OPLUS VIP 完全不同! 小米使用 <sig> XML 命令。
 */

/* 构建 sig 命令 XML (准备接收签名) */
int edl_xiaomi_build_sig_request(char *buf, int buf_size);

/* 构建 sig 请求 Token XML (获取 Challenge) */
int edl_xiaomi_build_token_request(char *buf, int buf_size);

/* 获取内置签名数量 */
int edl_xiaomi_builtin_sign_count(void);

/* 获取第 index 个内置签名 (二进制数据)。返回长度，data 填充。 */
int edl_xiaomi_builtin_sign(int index, uint8_t *data, int max_len);

/* 判断认证响应是否成功 */
bool edl_xiaomi_is_auth_success(const char *response);

/* 从 Token 响应提取 Challenge 字符串，写入 out。返回是否成功。 */
bool edl_xiaomi_extract_token(const char *response, char *out, int out_size);

#ifdef __cplusplus
}
#endif

#endif /* EDL_XIAOMI_AUTH_H */
