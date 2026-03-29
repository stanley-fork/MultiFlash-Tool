#ifndef EDL_SAHARA_H
#define EDL_SAHARA_H

#include "edl_types.h"
#include "edl_error.h"
#include "serial_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edl_sahara edl_sahara_t;

edl_sahara_t *edl_sahara_create(edl_port_t *port, const edl_callbacks_t *cb);
void          edl_sahara_destroy(edl_sahara_t *ctx);

/*
 * 连接后探测：是否收到 Sahara Hello（首次 EDL）。成功则内部缓存首包供 handshake 消费。
 * 另可返回 EDL_ERR_SAHARA_DEVICE_FIREHOSE_XML：首字节非 Hello（常见为 XML），已 edl_port_push_rx 回灌，应按 Firehose 处理。
 */
edl_error_t edl_sahara_probe_initial_hello(edl_sahara_t *ctx);

/*
 * Full handshake: wait for Hello, optionally read chip info, upload loader.
 * Retries on failure with state machine reset.
 */
edl_error_t edl_sahara_handshake(edl_sahara_t *ctx,
                                  const uint8_t *loader_data, size_t loader_size,
                                  int max_retries);

/* Get chip info (populated after successful handshake) */
const edl_chip_info_t *edl_sahara_chip_info(const edl_sahara_t *ctx);

/* Protocol version reported by device */
uint32_t edl_sahara_version(const edl_sahara_t *ctx);

/* Send reset command */
edl_error_t edl_sahara_reset(edl_sahara_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EDL_SAHARA_H */
