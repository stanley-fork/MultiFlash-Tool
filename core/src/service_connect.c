#include "service_internal.h"

#include "edl/port_detect.h"
#include "edl/chip_db.h"
#include "vendor_plugin.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t svc_read_u16_le(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t svc_read_u32_le(const uint8_t *buf)
{
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static uint64_t svc_read_u64_le(const uint8_t *buf)
{
    return (uint64_t)svc_read_u32_le(buf) |
           ((uint64_t)svc_read_u32_le(buf + 4) << 32);
}

static const char *svc_loader_class_name(uint8_t elf_class)
{
    switch (elf_class) {
    case 1: return "ELF32";
    case 2: return "ELF64";
    default: return "ELF";
    }
}

static const char *svc_loader_machine_name(uint16_t machine)
{
    switch (machine) {
    case 40:  return "ARM";
    case 183: return "ARM64";
    default:  return "未知架构";
    }
}

static const char *svc_loader_endian_name(uint8_t elf_data)
{
    switch (elf_data) {
    case 1: return "小端";
    case 2: return "大端";
    default: return "未知端序";
    }
}

static bool svc_extract_ascii_tag(const uint8_t *data, size_t size, const char *needle,
                                  char *out, size_t out_size)
{
    size_t needle_len;

    if (!data || !needle || !out || out_size == 0)
        return false;

    out[0] = '\0';
    needle_len = strlen(needle);
    if (needle_len == 0 || size <= needle_len)
        return false;

    for (size_t i = 0; i + needle_len < size; i++) {
        if (memcmp(data + i, needle, needle_len) != 0)
            continue;

        size_t j = 0;
        const uint8_t *p = data + i + needle_len;
        while (i + needle_len + j < size && j < out_size - 1) {
            unsigned char ch = p[j];
            if (ch == '\0' || ch == '\r' || ch == '\n')
                break;
            if (!isprint(ch))
                break;
            out[j++] = (char)ch;
        }
        out[j] = '\0';
        return j > 0;
    }

    return false;
}

static void svc_parse_loader_info(const uint8_t *data, size_t size, edl_loader_info_t *info)
{
    if (!info)
        return;

    memset(info, 0, sizeof(*info));
    info->valid = true;
    if (!data || size < 4)
        return;

    if (size >= 20 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        info->is_elf = true;
        info->elf_class = data[4];
        info->elf_data = data[5];
        info->elf_version = data[6];

        if (info->elf_data == 1) {
            info->machine = svc_read_u16_le(data + 18);
            if (info->elf_class == 1 && size >= 28)
                info->entry = svc_read_u32_le(data + 24);
            else if (info->elf_class == 2 && size >= 32)
                info->entry = svc_read_u64_le(data + 24);
        }
    }

    (void)svc_extract_ascii_tag(data, size, "QC_IMAGE_VERSION_STRING=",
                                info->qc_version, sizeof(info->qc_version));
    (void)svc_extract_ascii_tag(data, size, "OEM_IMAGE_VERSION_STRING=",
                                info->oem_version, sizeof(info->oem_version));
    (void)svc_extract_ascii_tag(data, size, "IMAGE_VARIANT_STRING=",
                                info->variant, sizeof(info->variant));
}

static void svc_log_loader_info(edl_service_t *svc, const char *loader_path,
                                const edl_loader_info_t *info, size_t loader_size)
{
    if (!svc || !info)
        return;

    if (info->is_elf) {
        const unsigned bits = info->elf_class == 2 ? 64u :
                              (info->elf_class == 1 ? 32u : 0u);
        if (bits > 0) {
            svc_log(svc, "【Loader】已解析: %s | %s | %u 位 | %s | 入口 0x%llx | 大小 %u KB",
                    svc_loader_class_name(info->elf_class),
                    svc_loader_machine_name(info->machine),
                    bits,
                    svc_loader_endian_name(info->elf_data),
                    (unsigned long long)info->entry,
                    (unsigned)(loader_size / 1024));
        } else {
            svc_log(svc, "【Loader】已解析: %s | %s | %s | 入口 0x%llx | 大小 %u KB",
                    svc_loader_class_name(info->elf_class),
                    svc_loader_machine_name(info->machine),
                    svc_loader_endian_name(info->elf_data),
                    (unsigned long long)info->entry,
                    (unsigned)(loader_size / 1024));
        }
        svc_log_detail(svc,
                       "【Loader】传输策略: 仅预解析位宽/版本；实际仍按设备发起的 Sahara ReadData / ReadData64 请求自动匹配");
        if (info->machine != 40 && info->machine != 183)
            svc_log_detail(svc, "【Loader】机器码: 0x%04x", info->machine);
    } else {
        svc_log(svc, "【Loader】已解析: 原始/未知格式 | 大小 %u KB",
                (unsigned)(loader_size / 1024));
    }

    if (info->qc_version[0])
        svc_log_detail(svc, "【Loader】QC 版本: %s", info->qc_version);
    if (info->oem_version[0])
        svc_log_detail(svc, "【Loader】OEM 版本: %s", info->oem_version);
    if (info->variant[0])
        svc_log_detail(svc, "【Loader】变体: %s", info->variant);
    if (loader_path && loader_path[0])
        svc_log_detail(svc, "【Loader】路径: %s", loader_path);
}

static uint8_t *load_file(const char *path, size_t *out_size)
{
    if (!path || !out_size)
        return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return NULL;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(fp);
        return NULL;
    }

    if ((long)fread(data, 1, (size_t)size, fp) != size) {
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *out_size = (size_t)size;
    return data;
}

static void svc_teardown_connect_fail(edl_service_t *svc)
{
    if (svc->firehose) {
        edl_firehose_destroy(svc->firehose);
        svc->firehose = NULL;
    }
    if (svc->sahara) {
        edl_sahara_destroy(svc->sahara);
        svc->sahara = NULL;
    }
    if (svc->port) {
        edl_port_destroy(svc->port);
        svc->port = NULL;
    }
}

static edl_error_t svc_connect_internal(edl_service_t *svc, const char *port_name,
                                        const char *loader_path, const char *storage_type,
                                        const edl_svc_auth_options_t *auth)
{
    svc->auth_mode = auth ? auth->mode : EDL_SVC_AUTH_NONE;

    if (storage_type)
        snprintf(svc->storage_type, sizeof(svc->storage_type), "%s", storage_type);

    size_t loader_size = 0;
    uint8_t *loader_data = load_file(loader_path, &loader_size);
    if (!loader_data) {
        svc_log(svc, "加载引导文件失败: %s", loader_path);
        return EDL_ERR_FILE_NOT_FOUND;
    }

    edl_loader_info_t loader_info;
    svc_parse_loader_info(loader_data, loader_size, &loader_info);
    svc_log_loader_info(svc, loader_path, &loader_info, loader_size);
    svc->loader_info = loader_info;

    svc->port = edl_port_create();
    if (!svc->port) {
        free(loader_data);
        return EDL_ERR_NO_MEMORY;
    }

    edl_error_t err = edl_port_open(svc->port, port_name, 921600, 5000, 5000);
    if (err != EDL_OK) {
        svc_log(svc, "无法打开端口 %s", port_name);
        edl_port_destroy(svc->port);
        svc->port = NULL;
        free(loader_data);
        return err;
    }

    snprintf(svc->port_name, sizeof(svc->port_name), "%s", port_name);
    svc_log(svc, "已连接: %s", port_name);

    svc->sahara = edl_sahara_create(svc->port, &svc->core_cb);
    if (!svc->sahara) {
        edl_port_destroy(svc->port);
        svc->port = NULL;
        free(loader_data);
        return EDL_ERR_NO_MEMORY;
    }

    edl_error_t probe = EDL_ERR_SAHARA_NO_HELLO;
    const int sah_max_tries = 3;
    for (int sah_try = 0; sah_try < sah_max_tries; sah_try++) {
        if (sah_try > 0) {
            svc_log_detail(svc, "Sahara 探头重试 (%d/%d)...", sah_try + 1, sah_max_tries);
            if (svc_sleep_cancelable(svc, sah_try == 1 ? 400 : 700)) {
                free(loader_data);
                svc_teardown_connect_fail(svc);
                return EDL_ERR_CANCELLED;
            }
        }
        probe = edl_sahara_probe_initial_hello(svc->sahara);
        if (probe == EDL_ERR_CANCELLED) {
            free(loader_data);
            svc_teardown_connect_fail(svc);
            return probe;
        }
        if (probe == EDL_OK || probe == EDL_ERR_SAHARA_DEVICE_FIREHOSE_XML)
            break;
    }

    if (probe == EDL_OK) {
        err = edl_sahara_handshake(svc->sahara, loader_data, loader_size, 3);
        free(loader_data);
        loader_data = NULL;
        if (err != EDL_OK) {
            svc_log(svc, "Sahara 握手失败: %s", edl_error_str(err));
            edl_sahara_destroy(svc->sahara);
            svc->sahara = NULL;
            edl_port_destroy(svc->port);
            svc->port = NULL;
            return err;
        }
        if (svc_sleep_cancelable(svc, 800)) {
            svc_teardown_connect_fail(svc);
            return EDL_ERR_CANCELLED;
        }
    } else if (probe == EDL_ERR_SAHARA_DEVICE_FIREHOSE_XML) {
        svc_log_detail(svc, "链路: 首包非 Sahara Hello（已在 Firehose，跳过 Loader）");
        edl_sahara_destroy(svc->sahara);
        svc->sahara = NULL;

        svc->firehose = edl_firehose_create(svc->port, &svc->core_cb);
        if (!svc->firehose) {
            free(loader_data);
            edl_port_destroy(svc->port);
            svc->port = NULL;
            return EDL_ERR_NO_MEMORY;
        }
        err = edl_firehose_ping(svc->firehose);
        free(loader_data);
        loader_data = NULL;
        if (err != EDL_OK) {
            svc_log(svc, "Firehose NOP 失败: %s（设备可能未处于 EDL 或端口错误）",
                    edl_error_str(err));
            edl_firehose_destroy(svc->firehose);
            svc->firehose = NULL;
            edl_port_destroy(svc->port);
            svc->port = NULL;
            return EDL_ERR_EDL_NO_PROTOCOL;
        }
        svc_log_detail(svc, "链路: Firehose 已就绪（跳过 Sahara Loader）");
    } else {
        svc_log_detail(svc, "探头: 非 Sahara Hello（%s），尝试 Firehose 直连",
                       edl_error_str(probe));
        edl_sahara_destroy(svc->sahara);
        svc->sahara = NULL;
        edl_port_purge(svc->port);

        svc->firehose = edl_firehose_create(svc->port, &svc->core_cb);
        if (!svc->firehose) {
            free(loader_data);
            edl_port_destroy(svc->port);
            svc->port = NULL;
            return EDL_ERR_NO_MEMORY;
        }
        err = edl_firehose_ping(svc->firehose);
        free(loader_data);
        loader_data = NULL;
        if (err != EDL_OK) {
            svc_log(svc, "Firehose NOP 失败: %s（设备可能未处于 EDL 或端口错误）",
                    edl_error_str(err));
            edl_firehose_destroy(svc->firehose);
            svc->firehose = NULL;
            edl_port_destroy(svc->port);
            svc->port = NULL;
            return EDL_ERR_EDL_NO_PROTOCOL;
        }
        svc_log_detail(svc, "链路: Firehose 已就绪（跳过 Sahara Loader）");
    }

    const edl_chip_info_t *chip_info = svc->sahara ? edl_sahara_chip_info(svc->sahara) : NULL;
    edl_platform_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    if (chip_info) {
        (void)edl_query_platform_profile(chip_info->msm_id,
                                         chip_info->oem_id,
                                         chip_info->model_id,
                                         chip_info->pk_hash,
                                         &profile);
    }

    if (auth && auth->mode != EDL_SVC_AUTH_NONE) {
        edl_vendor_plugin_ctx_t plugin_ctx = {
            .phase = EDL_VENDOR_PLUGIN_PRE_CONFIGURE,
            .port = svc->port,
            .sahara = svc->sahara,
            .firehose = svc->firehose,
            .cb = &svc->core_cb,
            .auth = auth,
            .chip_info = chip_info,
            .profile = chip_info ? &profile : NULL,
        };
        err = edl_vendor_plugin_run(auth->mode, &plugin_ctx);
        if (err != EDL_OK) {
            svc_teardown_connect_fail(svc);
            return err;
        }
    }

    if (!svc->firehose) {
        svc->firehose = edl_firehose_create(svc->port, &svc->core_cb);
        if (!svc->firehose) {
            edl_sahara_destroy(svc->sahara);
            svc->sahara = NULL;
            edl_port_destroy(svc->port);
            svc->port = NULL;
            return EDL_ERR_NO_MEMORY;
        }
    }

    err = edl_firehose_configure(svc->firehose, svc->storage_type, 0);
    if (err != EDL_OK) {
        svc_log(svc, "Firehose 配置失败: %s", edl_error_str(err));
        svc_teardown_connect_fail(svc);
        return err;
    }

    if (auth && auth->mode != EDL_SVC_AUTH_NONE) {
        edl_vendor_plugin_ctx_t plugin_ctx = {
            .phase = EDL_VENDOR_PLUGIN_POST_CONFIGURE,
            .port = svc->port,
            .sahara = svc->sahara,
            .firehose = svc->firehose,
            .cb = &svc->core_cb,
            .auth = auth,
            .chip_info = chip_info,
            .profile = chip_info ? &profile : NULL,
        };
        err = edl_vendor_plugin_run(auth->mode, &plugin_ctx);
        if (err != EDL_OK) {
            svc_teardown_connect_fail(svc);
            return err;
        }
    }

    svc->connected = true;
    svc->gpt_scan_failed = false;
    svc->gpt_last_scan_err = EDL_OK;
    svc_log(svc, "设备就绪 - %s | 扇区:%dB | 载荷:%dKB",
            svc->storage_type,
            edl_firehose_sector_size(svc->firehose),
            edl_firehose_max_payload(svc->firehose) / 1024);
    return EDL_OK;
}

edl_error_t edl_service_connect(edl_service_t *svc, const char *loader_path,
                                const char *storage_type)
{
    return edl_service_connect_ex(svc, loader_path, storage_type, NULL);
}

edl_error_t edl_service_connect_ex(edl_service_t *svc, const char *loader_path,
                                   const char *storage_type,
                                   const edl_svc_auth_options_t *auth)
{
    if (!svc || !loader_path)
        return EDL_ERR_INVALID_PARAM;

    svc_log_detail(svc, "正在检测高通 EDL 设备...");

    char port_name[32];
    edl_error_t err = edl_port_detect_first_edl(port_name, sizeof(port_name));
    if (err != EDL_OK) {
        svc_log(svc, "未检测到 EDL 设备");
        return err;
    }

    svc_log_detail(svc, "检测到 EDL 设备: %s", port_name);
    return svc_connect_internal(svc, port_name, loader_path, storage_type, auth);
}

edl_error_t edl_service_connect_port(edl_service_t *svc, const char *port_name,
                                     const char *loader_path, const char *storage_type)
{
    return edl_service_connect_port_ex(svc, port_name, loader_path, storage_type, NULL);
}

edl_error_t edl_service_connect_port_ex(edl_service_t *svc, const char *port_name,
                                        const char *loader_path, const char *storage_type,
                                        const edl_svc_auth_options_t *auth)
{
    if (!svc || !port_name || !loader_path)
        return EDL_ERR_INVALID_PARAM;

    return svc_connect_internal(svc, port_name, loader_path, storage_type, auth);
}
