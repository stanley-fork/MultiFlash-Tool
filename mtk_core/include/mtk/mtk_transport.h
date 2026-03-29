#ifndef MTK_TRANSPORT_H
#define MTK_TRANSPORT_H

#include "mtk_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtk_transport mtk_transport_t;

MTK_API mtk_transport_t* mtk_transport_create(const mtk_callbacks_t* cb);
MTK_API void mtk_transport_destroy(mtk_transport_t* transport);
MTK_API mtk_error_t mtk_transport_open(mtk_transport_t* transport, const char* port_name);
MTK_API void mtk_transport_close(mtk_transport_t* transport);
MTK_API int mtk_transport_is_open(const mtk_transport_t* transport);
MTK_API const char* mtk_transport_port_name(const mtk_transport_t* transport);
MTK_API int mtk_transport_write(mtk_transport_t* transport, const uint8_t* data, int len);
MTK_API int mtk_transport_read(mtk_transport_t* transport, uint8_t* buf, int len, int timeout_ms);
MTK_API int mtk_transport_read_exact(mtk_transport_t* transport, uint8_t* buf, int count, int timeout_ms);
MTK_API int mtk_transport_bytes_available(mtk_transport_t* transport);
MTK_API void mtk_transport_purge(mtk_transport_t* transport);

#ifdef __cplusplus
}
#endif

#endif
