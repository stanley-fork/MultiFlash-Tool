#include "edl/sahara.h"
#include "edl/serial_port.h"
#include "edl/chip_db.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#define edl_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define edl_sleep_ms(ms) usleep((ms) * 1000)
#endif

/* ===== Constants ===== */
#define SAHARA_READ_TIMEOUT_MS   30000
#define SAHARA_HELLO_TIMEOUT_MS  60000
/*
 * Sahara 探头：一次性读满 Hello 头 8 字节（与 QFIL / 旧版逻辑一致）。
 * 逐字节读在部分驱动上行为不一致；整包 read_exact 更可靠。
 */
#define SAHARA_PROBE_HDR_MS  25000
/* 已发 COMMAND HelloResponse 后等设备下一包；过短易误判（见 sahara_try_command_mode） */
#define SAHARA_POST_CMD_MODE_HDR_MS  20000
#define SAHARA_MAX_LOOP          1000

/* 由 core/CMakeLists.txt 的 EDL_SAHARA_COMMAND_MODE 注入；裸编译未定义时与默认一致：命令模式开启 */
#ifndef EDL_SAHARA_TRY_COMMAND_MODE
#define EDL_SAHARA_TRY_COMMAND_MODE 1
#endif

/* ===== Internal State ===== */
struct edl_sahara {
    edl_port_t      *port;
    edl_callbacks_t  cb;
    edl_chip_info_t  chip;

    uint32_t protocol_version;
    uint32_t device_mode;
    bool     connected;
    bool     chip_info_read;
    /* 已做过「命令模式失败 → RESET → 新 Hello」：下一轮再失败则放弃命令模式，避免死循环 */
    bool     cmd_mode_reset_recovery_done;
    bool     done_sent;
    int64_t  total_sent;
    size_t   loader_size_for_log;

    /* edl_sahara_probe_initial_hello 缓存的首个 Hello 全包，供 handshake 消费 */
    uint8_t *prefetch_pkt;
    size_t   prefetch_len;
};

/* ===== Helpers ===== */

static void sahara_log(edl_sahara_t *ctx, const char *fmt, ...)
{
    if (!ctx->cb.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->cb.log(buf, ctx->cb.user_data);
}

static void sahara_log_detail(edl_sahara_t *ctx, const char *fmt, ...)
{
    /* 仅当上层显式设置 log_detail 时输出；勿回退到 log */
    if (!ctx->cb.log_detail) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->cb.log_detail(buf, ctx->cb.user_data);
}

static void sahara_log_pk_hash_chunks(edl_sahara_t *ctx)
{
    const char *pk = ctx->chip.pk_hash;
    size_t len;
    int index = 0;

    if (!pk || !pk[0])
        return;

    len = strlen(pk);
    for (size_t off = 0; off < len; off += 32, index++) {
        size_t chunk = len - off;
        if (chunk > 32)
            chunk = 32;
        sahara_log(ctx, "PK_HASH[%d]        : 0x%.*s", index, (int)chunk, pk + off);
    }
}

static void sahara_log_chip_summary(edl_sahara_t *ctx)
{
    const char *chip_name = ctx->chip.chip_name[0] ? ctx->chip.chip_name : "Unknown";
    const char *codename = edl_chip_codename_precise(ctx->chip.msm_id);
    const char *oem_family = edl_vendor_by_oem(ctx->chip.oem_id);
    edl_brand_source_t brand_source = EDL_BRAND_SOURCE_NONE;
    const char *brand = edl_brand_by_ids_ex(ctx->chip.oem_id, ctx->chip.model_id,
                                            ctx->chip.pk_hash, &brand_source);
    bool show_precise_brand = false;

    if (brand)
        snprintf(ctx->chip.vendor, sizeof(ctx->chip.vendor), "%s", brand);
    else
        ctx->chip.vendor[0] = '\0';

    show_precise_brand = ctx->chip.vendor[0] &&
                         brand_source != EDL_BRAND_SOURCE_OEM_FAMILY &&
                         (!oem_family || strcmp(ctx->chip.vendor, oem_family) != 0);

    sahara_log(ctx, "当前模式        : Sahara");
    sahara_log(ctx, "CPU 识别        : %s", chip_name);
    if (codename)
        sahara_log(ctx, "芯片代号        : %s", codename);
    sahara_log(ctx, "Sahara 版本     : %u", ctx->chip.sahara_version);
    sahara_log(ctx, "硬件 ID         : %s", ctx->chip.hwid_hex[0] ? ctx->chip.hwid_hex : "(未知)");
    sahara_log(ctx, "MSM ID          : 0x%08x", ctx->chip.msm_id);
    if (oem_family)
        sahara_log(ctx, "OEM ID          : 0x%04x [%s]", ctx->chip.oem_id, oem_family);
    else
        sahara_log(ctx, "OEM ID          : 0x%04x", ctx->chip.oem_id);
    sahara_log(ctx, "机型 ID         : 0x%04x", ctx->chip.model_id);
    if (show_precise_brand)
        sahara_log(ctx, "品牌            : %s (%s)", ctx->chip.vendor, edl_brand_source_name(brand_source));
    sahara_log_pk_hash_chunks(ctx);
    sahara_log(ctx, "序列号          : %s", ctx->chip.serial_hex[0] ? ctx->chip.serial_hex : "(未知)");
}

static void write_u32_le(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static uint32_t read_u32_le(const uint8_t *buf)
{
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *buf)
{
    return (uint64_t)read_u32_le(buf) | ((uint64_t)read_u32_le(buf + 4) << 32);
}

static bool sahara_is_cancelled(edl_sahara_t *ctx)
{
    return ctx->cb.is_cancelled && ctx->cb.is_cancelled(ctx->cb.user_data);
}

static bool sahara_sleep_cancelable(edl_sahara_t *ctx, int total_ms)
{
    while (total_ms > 0) {
        if (sahara_is_cancelled(ctx))
            return true;
        int slice = total_ms > 20 ? 20 : total_ms;
        edl_sleep_ms(slice);
        total_ms -= slice;
    }
    return false;
}

static int sahara_read_bytes(edl_sahara_t *ctx, uint8_t *buf, int count, int timeout_ms)
{
    return edl_port_read_exact_ex(ctx->port, buf, count, timeout_ms,
                                  ctx->cb.is_cancelled, ctx->cb.user_data);
}

static edl_error_t sahara_discard_bytes(edl_sahara_t *ctx, int count, int timeout_ms)
{
    uint8_t tmp[256];

    while (count > 0) {
        int chunk = count > (int)sizeof(tmp) ? (int)sizeof(tmp) : count;
        int n = sahara_read_bytes(ctx, tmp, chunk, timeout_ms);
        if (n == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;
        if (n < chunk)
            return EDL_ERR_SAHARA_NO_RESPONSE;
        count -= n;
    }

    return EDL_OK;
}

static void sahara_free_prefetch(edl_sahara_t *ctx)
{
    if (!ctx) return;
    free(ctx->prefetch_pkt);
    ctx->prefetch_pkt = NULL;
    ctx->prefetch_len = 0;
}

static void sahara_send_reset_state(edl_sahara_t *ctx)
{
    uint8_t pkt[8];
    write_u32_le(pkt + 0, SAHARA_CMD_RESET_STATE);
    write_u32_le(pkt + 4, 8);
    edl_port_write(ctx->port, pkt, 8);
}

edl_error_t edl_sahara_probe_initial_hello(edl_sahara_t *ctx)
{
    if (!ctx || !ctx->port) return EDL_ERR_INVALID_PARAM;

    sahara_free_prefetch(ctx);
    /*
     * 禁止在此 edl_port_purge()：会 PURGE_RXCLEAR，丢掉设备在 USB 枚举/打开串口后已发出的 Hello。
     * edl_port_open 已只清 TX、保留 RX（与 SakuraEDL 一致）；此处再清 RX 会导致「永远收不到 Hello」
     * 只能走 Firehose 直连或重插。
     */
    if (sahara_sleep_cancelable(ctx, 150))
        return EDL_ERR_CANCELLED;

    /*
     * Bus Hound / QFIL 等：在首次读 Hello 之前先发 SAHARA_CMD_RESET_STATE(0x13)，
     * 与抓包「OUT 13 00 00 00 08 00 00 00 → IN Hello」一致；可同步 Sahara 状态机。
     * 若设备已在 Firehose（首字节为 XML），后续读仍走 FIREHOSE_XML 分支。
     */
    sahara_send_reset_state(ctx);
    if (sahara_sleep_cancelable(ctx, 50))
        return EDL_ERR_CANCELLED;

    uint8_t hdr[8];
    int got = sahara_read_bytes(ctx, hdr, 8, SAHARA_PROBE_HDR_MS);
    if (got == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (got < 0)
        got = 0;

    if (got == 0) {
        sahara_log_detail(ctx, "探头: %d ms 内未收到数据（非 Sahara 或链路异常）", SAHARA_PROBE_HDR_MS);
        return EDL_ERR_SAHARA_NO_HELLO;
    }

    if (got < 8) {
        if (edl_port_push_rx(ctx->port, hdr, got) != 0) {
            sahara_log_detail(ctx, "探头: 无法回灌部分 RX");
            return EDL_ERR_NO_MEMORY;
        }
        sahara_log_detail(ctx, "探头: 仅收到 %d 字节（非完整 8 字节头），按 Firehose/文本处理", got);
        return EDL_ERR_SAHARA_DEVICE_FIREHOSE_XML;
    }

    uint32_t cmd_id = read_u32_le(hdr);
    uint32_t pkt_len = read_u32_le(hdr + 4);

    if (cmd_id != (uint32_t)SAHARA_CMD_HELLO || pkt_len < 8 || pkt_len > 16384) {
        if (hdr[0] == '<' || hdr[0] == '\r' || hdr[0] == '\n' || hdr[0] == '\t' ||
            hdr[0] == ' ' || (unsigned char)hdr[0] == 0xEF) {
            if (edl_port_push_rx(ctx->port, hdr, 8) != 0) {
                edl_port_purge(ctx->port);
                return EDL_ERR_NO_MEMORY;
            }
            sahara_log_detail(ctx, "探头: 8 字节非 Hello (cmd=0x%08X)，回灌转 Firehose", cmd_id);
            return EDL_ERR_SAHARA_DEVICE_FIREHOSE_XML;
        }
        edl_port_purge(ctx->port);
        sahara_log_detail(ctx, "探头: 首包非 Sahara Hello (cmd=0x%08X len=%u)", cmd_id, (unsigned)pkt_len);
        return EDL_ERR_SAHARA_NO_HELLO;
    }

    ctx->prefetch_pkt = (uint8_t *)malloc(pkt_len);
    if (!ctx->prefetch_pkt) return EDL_ERR_NO_MEMORY;
    memcpy(ctx->prefetch_pkt, hdr, 8);
    if (pkt_len > 8) {
        int n = sahara_read_bytes(ctx, ctx->prefetch_pkt + 8, (int)(pkt_len - 8), 5000);
        if (n == EDL_ERR_CANCELLED) {
            sahara_free_prefetch(ctx);
            return EDL_ERR_CANCELLED;
        }
        if (n < (int)(pkt_len - 8)) {
            sahara_free_prefetch(ctx);
            return EDL_ERR_SAHARA_NO_RESPONSE;
        }
    }
    ctx->prefetch_len = pkt_len;
    sahara_log_detail(ctx, "【Sahara】探头：检测到首次连接（Hello）");
    return EDL_OK;
}

/* ===== Packet Sending ===== */

static void sahara_send_hello_response(edl_sahara_t *ctx, edl_sahara_mode_t mode)
{
    uint32_t version = ctx->protocol_version;
    if (version < 1 || version > 3) version = 2;

    uint8_t resp[48];
    memset(resp, 0, sizeof(resp));
    write_u32_le(resp + 0,  SAHARA_CMD_HELLO_RESP);
    write_u32_le(resp + 4,  48);
    write_u32_le(resp + 8,  version);
    write_u32_le(resp + 12, 1);  /* version_supported */
    write_u32_le(resp + 16, SAHARA_STATUS_SUCCESS);
    write_u32_le(resp + 20, (uint32_t)mode);
    edl_port_write(ctx->port, resp, 48);
}

static void sahara_send_done(edl_sahara_t *ctx)
{
    uint8_t pkt[8];
    write_u32_le(pkt + 0, SAHARA_CMD_DONE);
    write_u32_le(pkt + 4, 8);
    edl_port_write(ctx->port, pkt, 8);
}

static void sahara_send_switch_mode(edl_sahara_t *ctx, edl_sahara_mode_t mode)
{
    uint8_t pkt[12];
    write_u32_le(pkt + 0, SAHARA_CMD_SWITCH_MODE);
    write_u32_le(pkt + 4, 12);
    write_u32_le(pkt + 8, (uint32_t)mode);
    edl_port_write(ctx->port, pkt, 12);
}

static void sahara_send_execute(edl_sahara_t *ctx, edl_sahara_exec_cmd_t cmd)
{
    uint8_t pkt[12];
    write_u32_le(pkt + 0, SAHARA_CMD_EXECUTE);
    write_u32_le(pkt + 4, 12);
    write_u32_le(pkt + 8, (uint32_t)cmd);
    edl_port_write(ctx->port, pkt, 12);
}

static void sahara_send_execute_response(edl_sahara_t *ctx, edl_sahara_exec_cmd_t cmd)
{
    uint8_t pkt[12];
    write_u32_le(pkt + 0, SAHARA_CMD_EXECUTE_RESP);
    write_u32_le(pkt + 4, 12);
    write_u32_le(pkt + 8, (uint32_t)cmd);
    edl_port_write(ctx->port, pkt, 12);
}

/* ===== Chip Info Reading ===== */

static edl_error_t sahara_exec_command_safe(edl_sahara_t *ctx, edl_sahara_exec_cmd_t cmd,
                                            uint8_t **out_data, int *out_len)
{
    if (!out_data || !out_len)
        return EDL_ERR_INVALID_PARAM;

    *out_data = NULL;
    *out_len = 0;
    sahara_send_execute(ctx, cmd);

    uint8_t hdr[8];
    int n = sahara_read_bytes(ctx, hdr, 8, 1000);
    if (n == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (n < 8)
        return EDL_OK;

    uint32_t resp_cmd = read_u32_le(hdr);
    uint32_t resp_len = read_u32_le(hdr + 4);

    if (resp_cmd != SAHARA_CMD_EXECUTE_DATA) {
        if (resp_len > 8) {
            edl_error_t err = sahara_discard_bytes(ctx, (int)(resp_len - 8), 1000);
            if (err == EDL_ERR_CANCELLED)
                return err;
        }
        return EDL_OK;
    }

    if (resp_len <= 8)
        return EDL_OK;

    int body_len = (int)(resp_len - 8);
    uint8_t *body = (uint8_t *)malloc(body_len);
    if (!body)
        return EDL_ERR_NO_MEMORY;

    n = sahara_read_bytes(ctx, body, body_len, 1000);
    if (n == EDL_ERR_CANCELLED) {
        free(body);
        return EDL_ERR_CANCELLED;
    }
    if (n < body_len) {
        free(body);
        return EDL_OK;
    }
    if (body_len < 8) {
        free(body);
        return EDL_OK;
    }

    uint32_t data_cmd = read_u32_le(body);
    uint32_t data_len = read_u32_le(body + 4);
    free(body);

    if (data_cmd != (uint32_t)cmd || data_len == 0)
        return EDL_OK;

    /* Send confirm and read actual data */
    sahara_send_execute_response(ctx, cmd);

    uint8_t *data = (uint8_t *)malloc(data_len);
    if (!data)
        return EDL_ERR_NO_MEMORY;

    int timeout = data_len > 1000 ? 10000 : 1000;
    n = sahara_read_bytes(ctx, data, (int)data_len, timeout);
    if (n == EDL_ERR_CANCELLED) {
        free(data);
        return EDL_ERR_CANCELLED;
    }
    if (n < (int)data_len) {
        free(data);
        return EDL_OK;
    }

    *out_data = data;
    *out_len = (int)data_len;
    return EDL_OK;
}

static edl_error_t sahara_read_chip_info(edl_sahara_t *ctx)
{
    /* Serial number */
    int len = 0;
    uint8_t *data = NULL;
    edl_error_t err = sahara_exec_command_safe(ctx, SAHARA_EXEC_SERIAL_NUM, &data, &len);
    if (err != EDL_OK)
        return err;
    if (data && len >= 4) {
        uint32_t serial = read_u32_le(data);
        ctx->chip.serial_dec = serial;
        snprintf(ctx->chip.serial_hex, sizeof(ctx->chip.serial_hex), "0x%08x", serial);
    }
    free(data);

    /* PK Hash — devices may return multiple identical hash slots (e.g. 3×32 bytes);
     * deduplicate by using only the first unique block (SHA-256 = 32 bytes) */
    data = NULL;
    err = sahara_exec_command_safe(ctx, SAHARA_EXEC_OEM_PK_HASH, &data, &len);
    if (err != EDL_OK)
        return err;
    if (data && len > 0) {
        int hash_len = len < EDL_PK_HASH_MAX_LEN ? len : EDL_PK_HASH_MAX_LEN;
        int unique_len = hash_len;
        if (hash_len >= 64 && hash_len % 32 == 0) {
            int block = 32;
            bool all_same = true;
            for (int b = block; b < hash_len; b += block) {
                if (memcmp(data, data + b, block) != 0) { all_same = false; break; }
            }
            if (all_same) unique_len = block;
        } else if (hash_len >= 96 && hash_len % 48 == 0) {
            int block = 48;
            bool all_same = true;
            for (int b = block; b < hash_len; b += block) {
                if (memcmp(data, data + b, block) != 0) { all_same = false; break; }
            }
            if (all_same) unique_len = block;
        }
        char *p = ctx->chip.pk_hash;
        for (int i = 0; i < unique_len; i++)
            p += sprintf(p, "%02x", data[i]);
    }
    free(data);

    /* HWID - V1/V2 vs V3 */
    if (ctx->protocol_version < 3) {
        data = NULL;
        err = sahara_exec_command_safe(ctx, SAHARA_EXEC_MSM_HWID, &data, &len);
        if (err != EDL_OK)
            return err;
        if (data && len >= 8) {
            uint64_t hwid = read_u64_le(data);
            snprintf(ctx->chip.hwid_hex, sizeof(ctx->chip.hwid_hex), "0x%016llx", (unsigned long long)hwid);
            ctx->chip.msm_id = (uint32_t)(hwid >> 32);
            ctx->chip.oem_id = (uint16_t)((hwid >> 16) & 0xFFFF);
            ctx->chip.model_id = (uint16_t)(hwid & 0xFFFF);
            /* 使用芯片数据库查询名称 */
            const char *cname = edl_chip_name(ctx->chip.msm_id);
            snprintf(ctx->chip.chip_name, sizeof(ctx->chip.chip_name), "%s", cname);
            sahara_log_chip_summary(ctx);
        }
        free(data);
    } else {
        data = NULL;
        err = sahara_exec_command_safe(ctx, SAHARA_EXEC_CHIP_ID_V3, &data, &len);
        if (err != EDL_OK)
            return err;
        if (data && len >= 44) {
            uint32_t msm_id   = read_u32_le(data + 36);
            uint16_t oem_id   = (uint16_t)(data[40] | (data[41] << 8));
            uint16_t model_id = (uint16_t)(data[42] | (data[43] << 8));
            if (oem_id == 0 && len >= 46)
                oem_id = (uint16_t)(data[44] | (data[45] << 8));

            ctx->chip.msm_id = msm_id;
            ctx->chip.oem_id = oem_id;
            ctx->chip.model_id = model_id;
            snprintf(ctx->chip.hwid_hex, sizeof(ctx->chip.hwid_hex),
                     "0x00%06x%04x%04x", msm_id, oem_id, model_id);
            const char *cname3 = edl_chip_name(msm_id);
            snprintf(ctx->chip.chip_name, sizeof(ctx->chip.chip_name), "%s", cname3);
            sahara_log_chip_summary(ctx);
        }
        free(data);
    }

    return EDL_OK;
}

static edl_error_t sahara_drain_rx_available(edl_sahara_t *ctx, int max_rounds)
{
    uint8_t junk[512];
    for (int i = 0; i < max_rounds; i++) {
        int avail = edl_port_bytes_available(ctx->port);
        if (avail <= 0) {
            if (sahara_sleep_cancelable(ctx, 10))
                return EDL_ERR_CANCELLED;
            continue;
        }
        int chunk = avail > (int)sizeof(junk) ? (int)sizeof(junk) : avail;
        int n = edl_port_read(ctx->port, junk, chunk, 200);
        if (n <= 0)
            if (sahara_sleep_cancelable(ctx, 10))
                return EDL_ERR_CANCELLED;
    }

    return EDL_OK;
}

#if EDL_SAHARA_TRY_COMMAND_MODE

/* 命令模式尝试结果（与 Bus Hound / QFIL：先发 COMMAND HelloResponse，再视设备回应分支） */
enum {
    SAHARA_CMD_MODE_NEED_TX_HELLO = 0,
    SAHARA_CMD_MODE_DONE           = 1,
    SAHARA_CMD_MODE_SKIP_HELLO     = 2
};

static int sahara_try_command_mode(edl_sahara_t *ctx)
{
    sahara_log_detail(ctx, "进入命令模式 (v%u)...", ctx->protocol_version);
    sahara_send_hello_response(ctx, SAHARA_MODE_COMMAND);

    uint8_t hdr[8];
    int n = sahara_read_bytes(ctx, hdr, 8, SAHARA_POST_CMD_MODE_HDR_MS);
    if (n == EDL_ERR_CANCELLED)
        return EDL_ERR_CANCELLED;
    if (n < 8)
        return SAHARA_CMD_MODE_NEED_TX_HELLO;

    uint32_t cmd_id = read_u32_le(hdr);
    uint32_t pkt_len = read_u32_le(hdr + 4);

    if (cmd_id == SAHARA_CMD_COMMAND_READY) {
        if (pkt_len > 8) {
            edl_error_t err = sahara_discard_bytes(ctx, (int)(pkt_len - 8), 1000);
            if (err != EDL_OK)
                return err;
        }
        edl_error_t chip_err = sahara_read_chip_info(ctx);
        if (chip_err != EDL_OK)
            return chip_err;
        sahara_log_detail(ctx, "切换回传输模式...");
        sahara_send_switch_mode(ctx, SAHARA_MODE_IMAGE_TX_PENDING);
        if (sahara_sleep_cancelable(ctx, 50))
            return EDL_ERR_CANCELLED;
        return SAHARA_CMD_MODE_DONE;
    }

    /*
     * 设备跳过命令模式、直接发 ReadData/ReadData64（与 888888888.txt 中「仅传 Loader」路径一致）。
     * 若此处读完又丢弃，外层的 Hello 分支会再发一次 HelloResponse(传输模式)，协议错位 →
     * EndImageTransfer 状态 0x01 (INVALID_CMD)。必须把整包回灌，由主循环按 READ_DATA* 发送镜像。
     */
    if (cmd_id == SAHARA_CMD_READ_DATA || cmd_id == SAHARA_CMD_READ_DATA64) {
        int plen = (int)pkt_len;
        if (plen < 8 || plen > 16384)
            return SAHARA_CMD_MODE_NEED_TX_HELLO;
        uint8_t *pkt = (uint8_t *)malloc((size_t)plen);
        if (!pkt)
            return SAHARA_CMD_MODE_NEED_TX_HELLO;
        memcpy(pkt, hdr, 8);
        int rest = plen - 8;
        if (rest > 0) {
            n = sahara_read_bytes(ctx, pkt + 8, rest, 1000);
        } else {
            n = 0;
        }
        if (n == EDL_ERR_CANCELLED) {
            free(pkt);
            return EDL_ERR_CANCELLED;
        }
        if (rest > 0 && n < rest) {
            free(pkt);
            return SAHARA_CMD_MODE_NEED_TX_HELLO;
        }
        if (edl_port_push_rx(ctx->port, pkt, plen) != 0) {
            free(pkt);
            sahara_log_detail(ctx, "ReadData 回灌失败（缓冲区不足）");
            return SAHARA_CMD_MODE_NEED_TX_HELLO;
        }
        free(pkt);
        sahara_log_detail(ctx, "设备跳过命令模式 (ReadData/ReadData64)，已回灌 %d 字节，不重复发 HelloResponse", plen);
        return SAHARA_CMD_MODE_SKIP_HELLO;
    }

    if (cmd_id == SAHARA_CMD_END_IMAGE_TX) {
        sahara_log_detail(ctx, "命令模式异常响应 EndImageTransfer，放弃命令模式");
        if (pkt_len > 8) {
            edl_error_t err = sahara_discard_bytes(ctx, (int)(pkt_len - 8), 1000);
            if (err != EDL_OK)
                return err;
        }
        return SAHARA_CMD_MODE_NEED_TX_HELLO;
    }

    /* Device rejected command mode or unknown */
    sahara_log_detail(ctx, "命令模式: 非预期响应 cmd=0x%08X len=%u", cmd_id, (unsigned)pkt_len);
    if (pkt_len > 8) {
        edl_error_t err = sahara_discard_bytes(ctx, (int)(pkt_len - 8), 1000);
        if (err != EDL_OK)
            return err;
    }
    return SAHARA_CMD_MODE_NEED_TX_HELLO;
}

#endif /* EDL_SAHARA_TRY_COMMAND_MODE */

/* ===== Core Handshake Loop ===== */

static edl_error_t sahara_handshake_internal(edl_sahara_t *ctx,
                                              const uint8_t *loader, size_t loader_size)
{
    bool done = false;
    int loop_guard = 0;
    int end_image_count = 0;
    int timeout_count = 0;
    ctx->done_sent = false;
    ctx->total_sent = 0;
    ctx->loader_size_for_log = loader_size;
    ctx->cmd_mode_reset_recovery_done = false;

    sahara_log_detail(ctx, "【Sahara】等待设备 Hello（最长约 %u 秒）…",
                      (unsigned)(SAHARA_HELLO_TIMEOUT_MS / 1000));

    while (!done && loop_guard++ < SAHARA_MAX_LOOP) {
        uint8_t hdr[8];
        int got;
        if (ctx->prefetch_pkt && ctx->prefetch_len >= 8) {
            memcpy(hdr, ctx->prefetch_pkt, 8);
            got = 8;
        } else {
            int timeout_ms = (loop_guard == 1) ? SAHARA_HELLO_TIMEOUT_MS : SAHARA_READ_TIMEOUT_MS;
            got = sahara_read_bytes(ctx, hdr, 8, timeout_ms);
        }

        if (got == EDL_ERR_CANCELLED)
            return EDL_ERR_CANCELLED;
        if (got < 8) {
            timeout_count++;
            /* 与 C# SerialPortManager 一致：超时时若 RX 里已有字节，先读掉以免半包/杂散数据挡死同步 */
            if (edl_port_bytes_available(ctx->port) > 0) {
                edl_error_t err = sahara_drain_rx_available(ctx, 40);
                if (err != EDL_OK)
                    return err;
            }
            if (timeout_count >= 5) {
                sahara_log(ctx, "设备无响应");
                return EDL_ERR_SAHARA_NO_RESPONSE;
            }
            if (sahara_sleep_cancelable(ctx, 500))
                return EDL_ERR_CANCELLED;
            continue;
        }

        timeout_count = 0;
        uint32_t cmd_id = read_u32_le(hdr);
        uint32_t pkt_len = read_u32_le(hdr + 4);

        if (pkt_len < 8 || pkt_len > 16384) {
            sahara_free_prefetch(ctx);
            edl_port_purge(ctx->port);
            if (sahara_sleep_cancelable(ctx, 50))
                return EDL_ERR_CANCELLED;
            continue;
        }

        switch ((edl_sahara_cmd_t)cmd_id) {

        case SAHARA_CMD_HELLO: {
            uint8_t body[40];
            int have_body = 0;
            if (ctx->prefetch_pkt && ctx->prefetch_len >= pkt_len) {
                if (pkt_len > 8) {
                    size_t bl = pkt_len - 8;
                    size_t ncpy = bl < sizeof(body) ? bl : sizeof(body);
                    memcpy(body, ctx->prefetch_pkt + 8, ncpy);
                    have_body = 1;
                } else {
                    have_body = 1;
                }
                sahara_free_prefetch(ctx);
            } else if (pkt_len > 8) {
                int body_read = sahara_read_bytes(ctx, body, (int)(pkt_len - 8), 5000);
                if (body_read == EDL_ERR_CANCELLED)
                    return EDL_ERR_CANCELLED;
                if (body_read >= (int)(pkt_len - 8))
                    have_body = 1;
            }
            if (have_body && pkt_len > 8) {
                ctx->protocol_version = read_u32_le(body);
                ctx->device_mode = (pkt_len >= 20) ? read_u32_le(body + 12) : 0;
                ctx->chip.sahara_version = ctx->protocol_version;
                sahara_log_detail(ctx, "HELLO (version=%u, mode=%u)", ctx->protocol_version, ctx->device_mode);
            }

#if EDL_SAHARA_TRY_COMMAND_MODE
            if (!ctx->chip_info_read && ctx->device_mode == SAHARA_MODE_IMAGE_TX_PENDING) {
                sahara_log_detail(ctx, "【Sahara】收到 Hello → 命令模式读取设备标识（序列号/芯片等）…");
                int cm = sahara_try_command_mode(ctx);
                if (cm < 0)
                    return (edl_error_t)cm;
                if (cm == SAHARA_CMD_MODE_DONE) {
                    ctx->chip_info_read = true;
                    break;
                }
                if (cm == SAHARA_CMD_MODE_SKIP_HELLO) {
                    sahara_log_detail(ctx, "【Sahara】设备已请求镜像数据，跳过重复 HelloResponse，直接传 Loader");
                    ctx->chip_info_read = true;
                    break;
                }
                /*
                 * 已发 HelloResponse(COMMAND)，若未收到 COMMAND_READY/ReadData 回灌，不能在同一 Hello 内
                 * 再发 HelloResponse(传输)（易 0x01）。首次失败：RESET + 新 Hello，**再试命令模式**以填 HWID
                 *（Realme 等认证需要）；若 RESET 后仍失败则仅传 Loader。
                 */
                if (!ctx->cmd_mode_reset_recovery_done) {
                    sahara_log_detail(ctx, "【Sahara】命令模式未同步，RESET_STATE 后重新收 Hello 再试命令模式…");
                    edl_error_t drain_err = sahara_drain_rx_available(ctx, 40);
                    if (drain_err != EDL_OK)
                        return drain_err;
                    sahara_send_reset_state(ctx);
                    if (sahara_sleep_cancelable(ctx, 800))
                        return EDL_ERR_CANCELLED;
                    uint8_t nhdr[8];
                    int new_hdr = sahara_read_bytes(ctx, nhdr, 8, SAHARA_PROBE_HDR_MS);
                    if (new_hdr == EDL_ERR_CANCELLED)
                        return EDL_ERR_CANCELLED;
                    if (new_hdr < 8) {
                        sahara_log(ctx, "【Sahara】RESET 后未收到 Hello");
                        return EDL_ERR_SAHARA_NO_RESPONSE;
                    }
                    uint32_t ncmd = read_u32_le(nhdr);
                    uint32_t nlen = read_u32_le(nhdr + 4);
                    if (ncmd != (uint32_t)SAHARA_CMD_HELLO || nlen < 8 || nlen > 16384) {
                        sahara_log_detail(ctx, "RESET 后期望 Hello，收到 cmd=0x%08X len=%u", ncmd, (unsigned)nlen);
                        return EDL_ERR_SAHARA_NO_HELLO;
                    }
                    ctx->prefetch_pkt = (uint8_t *)malloc(nlen);
                    if (!ctx->prefetch_pkt) return EDL_ERR_NO_MEMORY;
                    memcpy(ctx->prefetch_pkt, nhdr, 8);
                    if (nlen > 8) {
                        int body_n = sahara_read_bytes(ctx, ctx->prefetch_pkt + 8, (int)(nlen - 8), 5000);
                        if (body_n == EDL_ERR_CANCELLED) {
                            sahara_free_prefetch(ctx);
                            return EDL_ERR_CANCELLED;
                        }
                        if (body_n < (int)(nlen - 8)) {
                            sahara_log(ctx, "【Sahara】RESET 后 Hello 包体不完整（len=%u）", (unsigned)nlen);
                            sahara_free_prefetch(ctx);
                            return EDL_ERR_SAHARA_NO_RESPONSE;
                        }
                    }
                    ctx->prefetch_len = nlen;
                    ctx->chip_info_read = false;
                    ctx->cmd_mode_reset_recovery_done = true;
                    break;
                }
                sahara_log_detail(ctx, "【Sahara】命令模式在 RESET 后仍失败，跳过芯片信息，仅传 Loader…");
                edl_error_t drain_err = sahara_drain_rx_available(ctx, 40);
                if (drain_err != EDL_OK)
                    return drain_err;
                ctx->chip_info_read = true;
                /* 继续本 case 末尾：HelloResponse(传输) */
            }
#else
            if (!ctx->chip_info_read && ctx->device_mode == SAHARA_MODE_IMAGE_TX_PENDING) {
                ctx->chip_info_read = true;
                sahara_log_detail(ctx, "【Sahara】收到 Hello → 传输模式（已关闭 Sahara 命令模式；正常请保持 CMake 默认 EDL_SAHARA_COMMAND_MODE=ON）");
            }
#endif

            sahara_log(ctx, "【Sahara】向设备传输 Firehose 加载器 (%u KB)…",
                       (unsigned)(ctx->loader_size_for_log / 1024));
            sahara_log_detail(ctx, "发送 HelloResponse (传输模式)");
            sahara_send_hello_response(ctx, SAHARA_MODE_IMAGE_TX_PENDING);
            break;
        }

        /* 32 位 READ_DATA：offset/length 各 4 字节；64 位 READ_DATA64：各 8 字节（大镜像/新引导） */
        case SAHARA_CMD_READ_DATA: {
            uint8_t body[12];
            int body_n = sahara_read_bytes(ctx, body, 12, 5000);
            if (body_n == EDL_ERR_CANCELLED)
                return EDL_ERR_CANCELLED;
            if (body_n < 12) break;
            uint32_t offset = read_u32_le(body + 4);
            uint32_t length = read_u32_le(body + 8);
            if (offset + length <= (uint32_t)loader_size)
                edl_port_write(ctx->port, loader + offset, (int)length);
            ctx->total_sent += length;
            if (ctx->cb.progress)
                ctx->cb.progress(ctx->total_sent, (int64_t)loader_size, ctx->cb.user_data);
            break;
        }

        case SAHARA_CMD_READ_DATA64: {
            uint8_t body[24];
            int body_n = sahara_read_bytes(ctx, body, 24, 5000);
            if (body_n == EDL_ERR_CANCELLED)
                return EDL_ERR_CANCELLED;
            if (body_n < 24) break;
            uint64_t offset = read_u64_le(body + 8);
            uint64_t length = read_u64_le(body + 16);
            if (offset + length <= loader_size)
                edl_port_write(ctx->port, loader + offset, (int)length);
            ctx->total_sent += (int64_t)length;
            if (ctx->cb.progress)
                ctx->cb.progress(ctx->total_sent, (int64_t)loader_size, ctx->cb.user_data);
            break;
        }

        case SAHARA_CMD_END_IMAGE_TX: {
            end_image_count++;
            if (end_image_count > 10) {
                sahara_log(ctx, "过多的 EndImageTransfer，设备状态异常");
                return EDL_ERR_SAHARA_TRANSFER;
            }
            uint32_t status = 0;
            if (pkt_len >= 16) {
                uint8_t body[8];
                int body_n = sahara_read_bytes(ctx, body, 8, 5000);
                if (body_n == EDL_ERR_CANCELLED)
                    return EDL_ERR_CANCELLED;
                if (body_n >= 8)
                    status = read_u32_le(body + 4);
            }
            if (status != 0) {
                sahara_log(ctx, "传输失败: 设备错误码 0x%02X", status);
                if (status == SAHARA_STATUS_INVALID_CMD)
                    sahara_log_detail(ctx, "（0x01=INVALID_CMD：多为 Sahara 状态错位；可重插 USB 或换 Loader）");
                if (status == SAHARA_STATUS_HASH_AUTH_FAIL)
                    return EDL_ERR_SAHARA_AUTH_FAIL;
                if (status == SAHARA_STATUS_HASH_VERIFY_FAIL || status == SAHARA_STATUS_HASH_TABLE_NOT_FOUND)
                    return EDL_ERR_SAHARA_HASH_FAIL;
                return EDL_ERR_SAHARA_TRANSFER;
            }
            if (!ctx->done_sent) {
                sahara_log_detail(ctx, "镜像传输完成，发送 Done");
                sahara_send_done(ctx);
                ctx->done_sent = true;
            }
            break;
        }

        case SAHARA_CMD_DONE_RESP: {
            if (pkt_len > 8) {
                edl_error_t err = sahara_discard_bytes(ctx, (int)(pkt_len - 8), 1000);
                if (err != EDL_OK)
                    return err;
            }
            sahara_log(ctx, "引导加载成功");
            done = true;
            ctx->connected = true;
            break;
        }

        case SAHARA_CMD_COMMAND_READY: {
            if (pkt_len > 8) {
                edl_error_t err = sahara_discard_bytes(ctx, (int)(pkt_len - 8), 1000);
                if (err != EDL_OK)
                    return err;
            }
            sahara_log_detail(ctx, "设备接受命令模式，切换回传输模式");
            sahara_send_switch_mode(ctx, SAHARA_MODE_IMAGE_TX_PENDING);
            break;
        }

        default:
            sahara_log_detail(ctx, "未知命令: 0x%02X", cmd_id);
            if (pkt_len > 8) {
                edl_error_t err = sahara_discard_bytes(ctx, (int)(pkt_len - 8), 1000);
                if (err != EDL_OK)
                    return err;
            }
            break;
        }
    }

    return done ? EDL_OK : EDL_ERR_SAHARA_TRANSFER;
}

/* ===== Public API ===== */

edl_sahara_t *edl_sahara_create(edl_port_t *port, const edl_callbacks_t *cb)
{
    if (!port) return NULL;
    edl_sahara_t *ctx = (edl_sahara_t *)calloc(1, sizeof(edl_sahara_t));
    if (!ctx) return NULL;
    ctx->port = port;
    if (cb) ctx->cb = *cb;
    ctx->protocol_version = 2;
    return ctx;
}

void edl_sahara_destroy(edl_sahara_t *ctx)
{
    if (!ctx) return;
    sahara_free_prefetch(ctx);
    free(ctx);
}

edl_error_t edl_sahara_handshake(edl_sahara_t *ctx,
                                  const uint8_t *loader_data, size_t loader_size,
                                  int max_retries)
{
    if (!ctx || !loader_data || loader_size == 0) return EDL_ERR_INVALID_PARAM;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            sahara_log(ctx, "重试 %d/%d - 重置状态机...", attempt, max_retries);
            sahara_free_prefetch(ctx);
            edl_port_purge(ctx->port);
            sahara_send_reset_state(ctx);
            if (sahara_sleep_cancelable(ctx, 900))
                return EDL_ERR_CANCELLED;

            /* Wait for new Hello */
            uint8_t probe[48];
            int got = sahara_read_bytes(ctx, probe, 48, 8000);
            if (got == EDL_ERR_CANCELLED)
                return EDL_ERR_CANCELLED;
            if (got >= 8 && read_u32_le(probe) == SAHARA_CMD_HELLO)
                sahara_log_detail(ctx, "重置后收到新 Hello 包");
            else
                sahara_log_detail(ctx, "重置后未收到 Hello，继续尝试");

            ctx->chip_info_read = false;
            ctx->done_sent = false;
            ctx->total_sent = 0;
            ctx->connected = false;
            ctx->loader_size_for_log = loader_size;
        }

        edl_error_t err = sahara_handshake_internal(ctx, loader_data, loader_size);
        if (err == EDL_OK) {
            if (attempt > 0)
                sahara_log(ctx, "重试成功 (第 %d 次)", attempt + 1);
            return EDL_OK;
        }

        if (err == EDL_ERR_CANCELLED)
            return err;

        /* Fatal errors - don't retry */
        if (err == EDL_ERR_SAHARA_AUTH_FAIL || err == EDL_ERR_SAHARA_HASH_FAIL)
            return err;
    }

    sahara_log(ctx, "所有尝试均失败，设备可能需要重新上电");
    return EDL_ERR_SAHARA_TRANSFER;
}

const edl_chip_info_t *edl_sahara_chip_info(const edl_sahara_t *ctx)
{
    return ctx ? &ctx->chip : NULL;
}

uint32_t edl_sahara_version(const edl_sahara_t *ctx)
{
    return ctx ? ctx->protocol_version : 0;
}

edl_error_t edl_sahara_reset(edl_sahara_t *ctx)
{
    if (!ctx) return EDL_ERR_INVALID_PARAM;
    sahara_send_reset_state(ctx);
    return EDL_OK;
}
