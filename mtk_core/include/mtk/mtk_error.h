#ifndef MTK_ERROR_H
#define MTK_ERROR_H

#include "mtk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtk_error {
    MTK_OK = 0,
    MTK_E_INVALID_ARG = -1,
    MTK_E_NO_MEMORY = -2,
    MTK_E_NOT_IMPLEMENTED = -3,
    MTK_E_NOT_CONNECTED = -4,
    MTK_E_IO = -5,
    MTK_E_TIMEOUT = -6,
    MTK_E_PROTO = -7,
    MTK_E_SECURITY = -8,
    MTK_E_UNSUPPORTED = -9,
    MTK_E_NOT_FOUND = -10,
    MTK_E_STATE = -11,
} mtk_error_t;

MTK_API const char* mtk_error_string(mtk_error_t err);

#ifdef __cplusplus
}
#endif

#endif
