#ifndef EDL_FS_PROP_PROBE_H
#define EDL_FS_PROP_PROBE_H

#include "edl_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*edl_prop_probe_log_fn)(const char *msg, void *user);
typedef int (*edl_prop_probe_read_text_fn)(void *parser, const char *path, char *buf, int buf_size);
typedef void (*edl_prop_probe_progress_fn)(int current, int total, void *user);

/**
 * 从分区镜像缓冲区探测 Android build.prop（EXT4 或 EROFS）。
 * @return true 若解析到至少一项常用属性
 */
bool edl_probe_android_props_from_buffer(const uint8_t *data, int len,
                                         edl_android_props_t *out,
                                         edl_prop_probe_log_fn log_fn, void *log_user);

/**
 * 同上，但可对 super 等容器分区做嵌入镜像扫描：
 * - 先试偏移 0
 * - scan_embedded 为 true 时按 4096 对齐扫描 [4096, len-min_slice) 寻找 EXT4/EROFS 超块
 * out_fs_offset 成功时写入子镜像起始偏移（首字节为 FS 时 0）
 */
bool edl_probe_android_props_from_buffer_scanned(const uint8_t *data, int len,
                                                 edl_android_props_t *out,
                                                 int64_t *out_fs_offset,
                                                 bool scan_embedded,
                                                 edl_prop_probe_log_fn log_fn,
                                                 void *log_user);

/**
 * 从已打开的 EXT4/EROFS 解析器中遍历常见 prop 文件并提取属性。
 * parser/read_text 由调用方提供，fs_name 仅用于日志展示。
 */
bool edl_probe_android_props_from_filesystem(void *parser,
                                             edl_prop_probe_read_text_fn read_text,
                                             const char *fs_name,
                                             edl_android_props_t *out,
                                             edl_prop_probe_log_fn log_fn,
                                             void *log_user);
bool edl_probe_android_props_from_filesystem_ex(void *parser,
                                                edl_prop_probe_read_text_fn read_text,
                                                const char *fs_name,
                                                edl_android_props_t *out,
                                                edl_prop_probe_log_fn log_fn,
                                                void *log_user,
                                                edl_prop_probe_progress_fn progress_fn,
                                                void *progress_user);

#ifdef __cplusplus
}
#endif

#endif /* EDL_FS_PROP_PROBE_H */
