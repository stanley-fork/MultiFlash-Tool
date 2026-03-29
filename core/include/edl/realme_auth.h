#ifndef EDL_REALME_AUTH_H
#define EDL_REALME_AUTH_H

#include "edl_types.h"
#include "edl_error.h"
#include "serial_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 【Realme 云端】与【OPLUS VIP 本地文件】是两套独立流程，切勿混为一谈：
 *   - edl_realme_authenticate：Realme 官方路径，getsigndata 材料由应用层云端签名（如 RCSMAUTH），
 *     verify 侧为 EnableVip=0（与 VIP 文件认证无关）。
 *   - edl_realme_vip_authenticate：OPLUS VIP，使用用户提供的 Digest + Signature 文件，
 *     Verify(EnableVip=1) 等 6 步串口流程。
 *
 * Realme 认证协议变体 (三种固件签名验证流程):
 *
 *   Modern (新型旗舰平台):
 *     … → getsigndata(ProjectID=…) → 云端签名 → verify(EnableVip=0) + 签名数据
 *
 *   LegacyPreXmlDigest (旧型含 initdigest):
 *     … → initdigest(digest 数据) → getsigndata → 云端签名 → verify + 签名数据
 *
 *   LegacySimplified (简化旧型):
 *     … → getsigndata → 云端签名 → verify + 签名数据
 *
 * 注意: OnePlus Demacia 等不走此 Realme 云端流程。
 *
 * VIP 认证 (OPLUS：Digest/Signature 文件，6 步):
 *     Digest → TransferCfg → Verify(EnableVip=1) → Signature → SHA256Init → …
 */

typedef enum {
    REALME_PROTO_UNKNOWN       = 0,
    REALME_PROTO_MODERN        = 1,
    REALME_PROTO_LEGACY_DIGEST = 2,
    REALME_PROTO_LEGACY_SIMPLE = 3
} edl_realme_protocol_t;

/* getsigndata 返回的设备签名材料 */
typedef struct {
    char chip_sn[64];
    char rand[128];
    char digest_write[512];
    char digest_read[512];
    char project_write[64];
    char project_read[64];
    char requested_project[64];
    char version[64];
    char platform1[64];
    char platform2[64];
    char mode[32];
    char secure_boot[32];
    char nv_data[256];
    char nv_code[128];
    bool nv_check;
    bool is_valid;
} edl_realme_sign_material_t;

/* 云端签名回调: UI 层实现网络请求，传入材料，返回签名数据 */
typedef bool (*edl_realme_sign_cb)(const edl_realme_sign_material_t *material,
                                    uint8_t *signature_out, int *signature_len,
                                    void *user_data);

/* ===== XML 构建器 ===== */

int edl_realme_build_configure(char *buf, int buf_size,
                                const char *storage_type, int payload_size,
                                bool enable_vip);

int edl_realme_build_getsigndata(char *buf, int buf_size, const char *project_id);
int edl_realme_build_getsigndata_legacy(char *buf, int buf_size);
int edl_realme_build_initdigest(char *buf, int buf_size, int digest_data_len);

/* Modern verify: EnableVip=0; Legacy verify: 无 EnableVip */
int edl_realme_build_verify_modern(char *buf, int buf_size);
int edl_realme_build_verify_legacy(char *buf, int buf_size);

/* VIP 认证流程 XML */
int edl_realme_build_transfercfg(char *buf, int buf_size);
int edl_realme_build_verify_vip(char *buf, int buf_size);
int edl_realme_build_sha256init(char *buf, int buf_size);
int edl_realme_build_sha256final(char *buf, int buf_size);
int edl_realme_build_getstorageinfo(char *buf, int buf_size, int lun);
int edl_realme_build_nop(char *buf, int buf_size);

/* ===== 签名材料解析 ===== */

bool edl_realme_parse_sign_material(const char *response,
                                     const char *requested_project,
                                     edl_realme_sign_material_t *out);

/* ===== 签名数据处理 ===== */

/*
 * 标准化签名: 截取/填充到 256 字节 (RSA-2048)
 * 返回实际写入长度
 */
int edl_realme_normalize_signature(const uint8_t *sig_in, int sig_len,
                                    uint8_t *sig_out);

/*
 * rawmode 下填充签名到 4096 字节 (扇区对齐)
 * sig_256: 256 字节的标准化签名
 * padded_out: 至少 4096 字节的输出缓冲区
 */
void edl_realme_pad_signature_4096(const uint8_t *sig_256, uint8_t *padded_out);

/* ===== 响应判断 ===== */

edl_realme_protocol_t edl_realme_detect_protocol(const char *response);

/* 根据 Sahara 读出的 MSM_ID + 是否提供 Digest 文件选择认证变体 */
edl_realme_protocol_t edl_realme_pick_auth_protocol(uint32_t msm_id, bool have_digest_file);
bool edl_realme_is_rawmode(const char *response);
bool edl_realme_is_verify_passed(const char *response);
bool edl_realme_is_ack(const char *response);
bool edl_realme_is_nak(const char *response);

/* ===== 完整认证编排 (需要串口和回调) ===== */

/*
 * Realme 完整云端签名认证。
 * 步骤:
 *   1. 根据 protocol 发送 getsigndata → 获取签名材料
 *   2. 调用 sign_cb 让 UI 层做云端签名
 *   3. 发送 verify + 签名数据 → 验证结果
 *
 * port:        串口
 * protocol:    已检测的协议变体
 * project_id:  ProjectID (Modern 必需, Legacy 可 NULL)
 * digest_data: Digest 数据 (Legacy initdigest 需要, Modern 可 NULL)
 * digest_len:  Digest 数据长度
 * sign_cb:     云端签名回调
 * cb_data:     回调用户数据
 * cb:          日志回调
 */
edl_error_t edl_realme_authenticate(edl_port_t *port,
                                     edl_realme_protocol_t protocol,
                                     const char *project_id,
                                     const uint8_t *digest_data, int digest_len,
                                     edl_realme_sign_cb sign_cb, void *cb_data,
                                     const edl_callbacks_t *cb);

/*
 * VIP 认证 (Digest + Signature 文件)。
 * 6步: Digest → TransferCfg → Verify(VIP=1) → Signature → SHA256Init → 完成
 */
edl_error_t edl_realme_vip_authenticate(edl_port_t *port,
                                         const uint8_t *digest_data, int digest_len,
                                         const uint8_t *signature_data, int sig_len,
                                         const edl_callbacks_t *cb);

#ifdef __cplusplus
}
#endif

#endif /* EDL_REALME_AUTH_H */
