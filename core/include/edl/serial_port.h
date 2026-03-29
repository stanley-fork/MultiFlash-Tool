#ifndef EDL_SERIAL_PORT_H
#define EDL_SERIAL_PORT_H

#include "edl_types.h"
#include "edl_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edl_port edl_port_t;

edl_port_t *edl_port_create(void);
void        edl_port_destroy(edl_port_t *port);

edl_error_t edl_port_open(edl_port_t *port, const char *name,
                           int baud_rate, int read_timeout_ms, int write_timeout_ms);
void        edl_port_close(edl_port_t *port);
bool        edl_port_is_open(const edl_port_t *port);

int         edl_port_write(edl_port_t *port, const uint8_t *data, int len);
int         edl_port_read(edl_port_t *port, uint8_t *buf, int len, int timeout_ms);
int         edl_port_read_exact(edl_port_t *port, uint8_t *buf, int count, int timeout_ms);
int         edl_port_read_exact_ex(edl_port_t *port, uint8_t *buf, int count, int timeout_ms,
                                   edl_cancel_cb cancel_cb, void *user_data);
int         edl_port_bytes_available(edl_port_t *port);
void        edl_port_set_transfer_window(edl_port_t *port, int payload_bytes);

void        edl_port_purge(edl_port_t *port);
/* 仅丢弃接收缓冲（与 SakuraEDL DiscardInBuffer 一致，发送 program 前调用） */
void        edl_port_discard_rx(edl_port_t *port);
/* 将已读出的字节插回队首，供后续 read 再消费（Sahara 探头识别 Firehose XML 首字节用） */
int         edl_port_push_rx(edl_port_t *port, const uint8_t *data, int len);
void        edl_port_flush(edl_port_t *port);

const char *edl_port_name(const edl_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* EDL_SERIAL_PORT_H */
