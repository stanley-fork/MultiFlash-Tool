#include "mtk/mtk_error.h"

const char* mtk_error_string(mtk_error_t err) {
    switch (err) {
    case MTK_OK: return "ok";
    case MTK_E_INVALID_ARG: return "invalid argument";
    case MTK_E_NO_MEMORY: return "out of memory";
    case MTK_E_NOT_IMPLEMENTED: return "not implemented";
    case MTK_E_NOT_CONNECTED: return "not connected";
    case MTK_E_IO: return "i/o error";
    case MTK_E_TIMEOUT: return "timeout";
    case MTK_E_PROTO: return "protocol error";
    case MTK_E_SECURITY: return "security restriction";
    case MTK_E_UNSUPPORTED: return "unsupported";
    case MTK_E_NOT_FOUND: return "not found";
    case MTK_E_STATE: return "invalid state";
    default: return "unknown error";
    }
}
