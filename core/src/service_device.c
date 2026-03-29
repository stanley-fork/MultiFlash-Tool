#include "service_internal.h"

#include "edl/port_detect.h"

#include <string.h>

#ifndef _WIN32
#include <strings.h>
#endif

static bool svc_part_is_gpt_label(const char *name)
{
    if (!name || !name[0])
        return false;
#ifdef _WIN32
    return _stricmp(name, "PrimaryGPT") == 0 || _stricmp(name, "BackupGPT") == 0;
#else
    return strcasecmp(name, "PrimaryGPT") == 0 || strcasecmp(name, "BackupGPT") == 0;
#endif
}

const edl_chip_info_t *edl_service_chip_info(const edl_service_t *svc)
{
    return svc && svc->sahara ? edl_sahara_chip_info(svc->sahara) : NULL;
}

const edl_loader_info_t *edl_service_loader_info(const edl_service_t *svc)
{
    return svc ? &svc->loader_info : NULL;
}

const char *edl_service_port_name(const edl_service_t *svc)
{
    return svc ? svc->port_name : "";
}

const char *edl_service_storage_type(const edl_service_t *svc)
{
    return svc ? svc->storage_type : "";
}

const char *edl_service_session_mode(const edl_service_t *svc)
{
    if (!svc || !svc->connected)
        return "未连接";
    if (svc->firehose)
        return "Firehose";
    if (svc->sahara)
        return "Sahara";
    return "未知";
}

int edl_service_sector_size(const edl_service_t *svc)
{
    return svc && svc->firehose ? edl_firehose_sector_size(svc->firehose) : 4096;
}

int edl_service_max_payload_bytes(const edl_service_t *svc)
{
    return svc && svc->firehose ? edl_firehose_max_payload(svc->firehose) : 0;
}

edl_error_t edl_service_get_storage_info(edl_service_t *svc)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_get_storage_info(svc->firehose);
    svc_log_elapsed(svc, "获取存储信息(getstorageinfo)", err, start_ms);
    return err;
}

edl_error_t edl_service_get_storage_device_report(edl_service_t *svc,
                                                  char *report, size_t report_size)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_get_storage_device_report(svc->firehose, report, report_size);
    svc_log_elapsed(svc, "读取存储设备信息报告", err, start_ms);
    return err;
}

edl_error_t edl_service_ping(edl_service_t *svc)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;
    return edl_firehose_ping(svc->firehose);
}

edl_error_t edl_service_read_partition(edl_service_t *svc,
                                       const edl_partition_info_t *part,
                                       const char *save_path)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_read_partition(svc->firehose, part, save_path);
    svc_log_elapsed(svc, part && part->name[0] ? part->name : "读取分区", err, start_ms);
    return err;
}

edl_error_t edl_service_read_partition_mem(edl_service_t *svc,
                                           const edl_partition_info_t *part,
                                           uint8_t **out_data, int *out_len)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_read_partition_mem(svc->firehose, part, out_data, out_len);
    svc_log_detail(svc, "【耗时】读取分区到内存%s%s：%llu ms%s",
                   part && part->name[0] ? " " : "",
                   part && part->name[0] ? part->name : "",
                   (unsigned long long)(svc_now_ms() - start_ms),
                   err == EDL_OK ? "" : "（失败或取消）");
    return err;
}

void edl_service_set_write_options(edl_service_t *svc,
                                   bool pad_short_image_to_gpt,
                                   bool program_read_back_verify)
{
    if (!svc || !svc->firehose)
        return;

    edl_firehose_set_write_options(svc->firehose, pad_short_image_to_gpt,
                                   program_read_back_verify);
}

edl_error_t edl_service_write_partition(edl_service_t *svc,
                                        const edl_partition_info_t *part,
                                        const char *image_path)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_write_partition(svc->firehose, part, image_path);
    if (err == EDL_OK && part && svc_part_is_gpt_label(part->name))
        svc_clear_partition_cache(svc);
    svc_log_elapsed(svc, part && part->name[0] ? part->name : "写入分区", err, start_ms);
    return err;
}

edl_error_t edl_service_write_partition_mem(edl_service_t *svc,
                                            const edl_partition_info_t *part,
                                            const uint8_t *data, int data_len)
{
    if (!svc || !svc->connected || !svc->firehose || !part)
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_write_partition_mem(svc->firehose, part, data, data_len);
    if (err == EDL_OK && svc_part_is_gpt_label(part->name))
        svc_clear_partition_cache(svc);
    svc_log_elapsed(svc, part->name[0] ? part->name : "写入分区(内存)", err, start_ms);
    return err;
}

edl_error_t edl_service_erase_partition(edl_service_t *svc,
                                        const edl_partition_info_t *part)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    const uint64_t start_ms = svc_now_ms();
    edl_error_t err = edl_firehose_erase_partition(svc->firehose, part);
    svc_log_elapsed(svc, part && part->name[0] ? part->name : "擦除分区", err, start_ms);
    return err;
}

edl_error_t edl_service_reboot(edl_service_t *svc, const char *mode)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;
    return edl_firehose_reboot(svc->firehose, mode);
}

int edl_service_detect_ports(edl_detected_port_t *ports, int max_ports)
{
    return edl_port_detect(ports, max_ports);
}
