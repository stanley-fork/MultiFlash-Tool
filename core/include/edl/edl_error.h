#ifndef EDL_ERROR_H
#define EDL_ERROR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EDL_OK                       =  0,

    /* General */
    EDL_ERR_INVALID_PARAM        = -1,
    EDL_ERR_NO_MEMORY            = -2,
    EDL_ERR_TIMEOUT              = -3,
    EDL_ERR_CANCELLED            = -4,
    EDL_ERR_IO                   = -5,
    EDL_ERR_FILE_NOT_FOUND       = -6,
    EDL_ERR_FILE_IO              = -7,

    /* Serial Port */
    EDL_ERR_PORT_OPEN            = -100,
    EDL_ERR_PORT_CONFIG          = -101,
    EDL_ERR_PORT_WRITE           = -102,
    EDL_ERR_PORT_READ            = -103,
    EDL_ERR_PORT_CLOSED          = -104,
    EDL_ERR_PORT_NOT_FOUND       = -105,
    EDL_ERR_EDL_NO_PROTOCOL      = -106,  /* 非 Sahara 且 Firehose NOP 失败 */

    /* Sahara */
    EDL_ERR_SAHARA_NO_HELLO      = -200,
    EDL_ERR_SAHARA_VERSION       = -201,
    EDL_ERR_SAHARA_AUTH_FAIL     = -202,
    EDL_ERR_SAHARA_HASH_FAIL     = -203,
    EDL_ERR_SAHARA_TRANSFER      = -204,
    EDL_ERR_SAHARA_CMD_FAIL      = -205,
    EDL_ERR_SAHARA_NO_RESPONSE   = -206,
    EDL_ERR_SAHARA_RESET_FAIL    = -207,
    EDL_ERR_SAHARA_DEVICE_FIREHOSE_XML = -208, /* 首字节非 Hello，已回灌 RX，按 Firehose 处理 */

    /* Firehose */
    EDL_ERR_FH_CONFIGURE         = -300,
    EDL_ERR_FH_NAK               = -301,
    EDL_ERR_FH_XML_PARSE         = -302,
    EDL_ERR_FH_READ              = -303,
    EDL_ERR_FH_WRITE             = -304,
    EDL_ERR_FH_ERASE             = -305,
    EDL_ERR_FH_PARTITION_NOT_FOUND = -306,
    EDL_ERR_FH_WRITE_PROTECT     = -307,
    EDL_ERR_FH_AUTH_REQUIRED     = -308,
    EDL_ERR_FH_LUN_ABSENT        = -309,  /* UFS 无此 LUN 槽位（如 LUN6+ 不存在） */
    EDL_ERR_FH_FIXGPT_UNSUPPORTED = -310, /* 设备固件不支持 fixgpt 或需认证（分区已写仍可启动） */

    /* GPT */
    EDL_ERR_GPT_NO_SIGNATURE     = -400,
    EDL_ERR_GPT_INVALID_HEADER   = -401,
    EDL_ERR_GPT_CRC_MISMATCH    = -402,
    EDL_ERR_GPT_TOO_SMALL        = -403,
    EDL_ERR_GPT_SCAN_EMPTY       = -404, /* 已扫 LUN 但未解析出任何分区（或各 LUN 读/解析均失败） */

    /* Rawprogram */
    EDL_ERR_RP_PARSE             = -500,
    EDL_ERR_RP_MISSING_FILE      = -501
} edl_error_t;

const char *edl_error_str(edl_error_t err);

/* 失败分级: FAIL = 设备侧拒绝/业务失败; ERROR = 通讯/解析/参数等异常 */
bool edl_error_is_fail(edl_error_t err);

/* 批量操作中遇 ERROR 是否终止后续项（会话/链路级异常 true；单机分区错误 false 可继续） */
bool edl_error_error_stops_batch(edl_error_t err);

#ifdef __cplusplus
}
#endif

#endif /* EDL_ERROR_H */
