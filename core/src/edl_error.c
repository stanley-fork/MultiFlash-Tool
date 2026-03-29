#include "edl/edl_error.h"

const char *edl_error_str(edl_error_t err)
{
    switch (err) {
    case EDL_OK:                        return "成功";
    case EDL_ERR_INVALID_PARAM:         return "无效参数";
    case EDL_ERR_NO_MEMORY:             return "内存不足";
    case EDL_ERR_TIMEOUT:               return "操作超时";
    case EDL_ERR_CANCELLED:             return "操作已取消";
    case EDL_ERR_IO:                    return "I/O 错误";
    case EDL_ERR_FILE_NOT_FOUND:        return "文件不存在";
    case EDL_ERR_FILE_IO:               return "文件读写错误";

    case EDL_ERR_PORT_OPEN:             return "无法打开端口";
    case EDL_ERR_PORT_CONFIG:           return "端口配置失败";
    case EDL_ERR_PORT_WRITE:            return "端口写入失败";
    case EDL_ERR_PORT_READ:             return "端口读取失败";
    case EDL_ERR_PORT_CLOSED:           return "端口已关闭";
    case EDL_ERR_PORT_NOT_FOUND:        return "未找到 EDL 端口";
    case EDL_ERR_EDL_NO_PROTOCOL:       return "EDL: 非 Sahara 且 Firehose 无响应";

    case EDL_ERR_SAHARA_NO_HELLO:       return "Sahara: 未收到 Hello 包";
    case EDL_ERR_SAHARA_VERSION:        return "Sahara: 协议版本不匹配";
    case EDL_ERR_SAHARA_AUTH_FAIL:      return "Sahara: 认证失败";
    case EDL_ERR_SAHARA_HASH_FAIL:      return "Sahara: Hash 校验失败";
    case EDL_ERR_SAHARA_TRANSFER:       return "Sahara: 传输失败";
    case EDL_ERR_SAHARA_CMD_FAIL:       return "Sahara: 命令执行失败";
    case EDL_ERR_SAHARA_NO_RESPONSE:    return "Sahara: 设备无响应";
    case EDL_ERR_SAHARA_RESET_FAIL:     return "Sahara: 重置失败";
    case EDL_ERR_SAHARA_DEVICE_FIREHOSE_XML: return "Sahara: 首包为 Firehose/XML（非 Hello）";

    case EDL_ERR_FH_CONFIGURE:          return "Firehose: 配置失败";
    case EDL_ERR_FH_NAK:                return "Firehose: 设备拒绝 (NAK)";
    case EDL_ERR_FH_XML_PARSE:          return "Firehose: XML 解析错误";
    case EDL_ERR_FH_READ:               return "Firehose: 读取失败";
    case EDL_ERR_FH_WRITE:              return "Firehose: 写入失败";
    case EDL_ERR_FH_ERASE:              return "Firehose: 擦除失败";
    case EDL_ERR_FH_PARTITION_NOT_FOUND: return "Firehose: 分区未找到";
    case EDL_ERR_FH_WRITE_PROTECT:      return "Firehose: 分区写保护";
    case EDL_ERR_FH_AUTH_REQUIRED:      return "Firehose: 需要认证";
    case EDL_ERR_FH_LUN_ABSENT:         return "无此 UFS LUN 槽位（常见仅 LUN0–5，LUN6/7 不存在为正常）";
    case EDL_ERR_FH_FIXGPT_UNSUPPORTED: return "Firehose: 本机不支持 fixgpt（镜像已写入，可忽略；若需 CRC 修复请换支持该命令的 Loader 或冷启动 Sahara 后认证）";

    case EDL_ERR_GPT_NO_SIGNATURE:      return "GPT: 未找到 EFI PART 签名";
    case EDL_ERR_GPT_INVALID_HEADER:    return "GPT: 无效头部";
    case EDL_ERR_GPT_CRC_MISMATCH:      return "GPT: CRC 校验不匹配";
    case EDL_ERR_GPT_TOO_SMALL:         return "GPT: 数据太短";
    case EDL_ERR_GPT_SCAN_EMPTY:        return "GPT: 全 LUN 扫描后未解析到分区（请试「读取 GPT」或换 Loader；部分机型分区在更高 LUN）";

    case EDL_ERR_RP_PARSE:              return "Rawprogram: 解析错误";
    case EDL_ERR_RP_MISSING_FILE:       return "Rawprogram: 缺少镜像文件";
    default:                            return "未知错误";
    }
}

bool edl_error_is_fail(edl_error_t err)
{
    if (err == EDL_OK)
        return false;
    switch (err) {
    case EDL_ERR_FH_NAK:
    case EDL_ERR_FH_PARTITION_NOT_FOUND:
    case EDL_ERR_FH_WRITE_PROTECT:
    case EDL_ERR_FH_AUTH_REQUIRED:
    case EDL_ERR_FH_ERASE:
    case EDL_ERR_SAHARA_AUTH_FAIL:
    case EDL_ERR_SAHARA_HASH_FAIL:
        return true;
    default:
        return false;
    }
}

bool edl_error_error_stops_batch(edl_error_t err)
{
    if (err == EDL_OK)
        return false;
    if (edl_error_is_fail(err))
        return true;
    switch (err) {
    case EDL_ERR_CANCELLED:
    case EDL_ERR_TIMEOUT:
    case EDL_ERR_IO:
    case EDL_ERR_NO_MEMORY:
    case EDL_ERR_INVALID_PARAM:
    case EDL_ERR_FILE_NOT_FOUND:
    case EDL_ERR_FILE_IO:
    case EDL_ERR_PORT_OPEN:
    case EDL_ERR_PORT_CONFIG:
    case EDL_ERR_PORT_WRITE:
    case EDL_ERR_PORT_READ:
    case EDL_ERR_PORT_CLOSED:
    case EDL_ERR_PORT_NOT_FOUND:
    case EDL_ERR_SAHARA_NO_HELLO:
    case EDL_ERR_SAHARA_VERSION:
    case EDL_ERR_SAHARA_TRANSFER:
    case EDL_ERR_SAHARA_CMD_FAIL:
    case EDL_ERR_SAHARA_NO_RESPONSE:
    case EDL_ERR_SAHARA_RESET_FAIL:
    case EDL_ERR_EDL_NO_PROTOCOL:
    case EDL_ERR_FH_CONFIGURE:
        return true;
    default:
        return false;
    }
}
