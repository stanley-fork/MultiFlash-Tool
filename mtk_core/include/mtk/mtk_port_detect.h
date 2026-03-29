#ifndef MTK_PORT_DETECT_H
#define MTK_PORT_DETECT_H

#include "mtk_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtk_port_type {
    MTK_PORT_UNKNOWN = 0,
    MTK_PORT_BROM,
    MTK_PORT_PRELOADER,
    MTK_PORT_DA,
} mtk_port_type_t;

typedef struct mtk_detected_port {
    char port_name[32];
    char device_id[256];
    char description[128];
    mtk_port_type_t type;
} mtk_detected_port_t;

MTK_API int mtk_port_detect(mtk_detected_port_t* ports, int max_ports);
MTK_API mtk_error_t mtk_port_detect_first(char* port_name, size_t name_size);
MTK_API int mtk_port_detect_has_port(const char* port_name);

#ifdef __cplusplus
}
#endif

#endif
