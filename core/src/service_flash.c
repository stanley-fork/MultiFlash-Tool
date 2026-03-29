#include "service_internal.h"

#include "edl/rawprogram.h"
#include "edl/slot_detect.h"
#include "edl/xml_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <strings.h>
#endif

#define MAX_FLASH_TASKS 512
#define MAX_PATCHES     256

#ifdef _WIN32
#define edl_label_is_gpt(label) \
    (_stricmp((label), "PrimaryGPT") == 0 || _stricmp((label), "BackupGPT") == 0)
#else
#define edl_label_is_gpt(label) \
    (strcasecmp((label), "PrimaryGPT") == 0 || strcasecmp((label), "BackupGPT") == 0)
#endif

static int svc_gpt_label_order(const char *label)
{
#ifdef _WIN32
    if (_stricmp(label, "PrimaryGPT") == 0)
        return 0;
    if (_stricmp(label, "BackupGPT") == 0)
        return 1;
#else
    if (strcasecmp(label, "PrimaryGPT") == 0)
        return 0;
    if (strcasecmp(label, "BackupGPT") == 0)
        return 1;
#endif
    return 2;
}

static bool svc_part_name_has_slot_suffix(const char *name, char slot_suffix)
{
    if (!name || !name[0])
        return false;

    const size_t len = strlen(name);
    if (len < 2 || name[len - 2] != '_')
        return false;

    char tail = name[len - 1];
    if (tail >= 'A' && tail <= 'Z')
        tail = (char)(tail - 'A' + 'a');
    return tail == slot_suffix;
}

static void edl_flash_task_to_partition_info(const edl_flash_task_t *task,
                                             edl_partition_info_t *part)
{
    memset(part, 0, sizeof(*part));
    part->lun = task->lun;
    part->start_sector = task->start_sector;
    snprintf(part->start_sector_expr, sizeof(part->start_sector_expr), "%s",
             task->start_sector_expr);
    part->num_sectors = task->num_sectors;
    part->sector_size = task->sector_size;
    part->file_sector_offset = task->file_sector_offset;

    if (task->label[0]) {
        snprintf(part->name, sizeof(part->name), "%s", task->label);
        return;
    }

    const char *fn = task->filename[0] ? task->filename : task->filepath;
    const char *base = fn;
    const char *s1 = strrchr(fn, '\\');
    const char *s2 = strrchr(fn, '/');
    if (s1 && (!s2 || s1 > s2))
        base = s1 + 1;
    else if (s2)
        base = s2 + 1;
    snprintf(part->name, sizeof(part->name), "%s", base);
}

static edl_error_t svc_load_rawprogram_tasks(edl_service_t *svc,
                                             const char *rawprogram_path,
                                             const char *base_dir,
                                             edl_flash_task_t *tasks,
                                             int capacity,
                                             int *out_task_count)
{
    if (!svc || !rawprogram_path || !tasks || !out_task_count || capacity <= 0)
        return EDL_ERR_INVALID_PARAM;

    int task_count = edl_rawprogram_parse(rawprogram_path, base_dir, tasks, capacity);
    if (task_count < 0) {
        svc_log(svc, "解析 rawprogram XML 失败");
        return EDL_ERR_RP_PARSE;
    }

    int dev_sector_size = edl_service_sector_size(svc);
    if (dev_sector_size > 0) {
        for (int i = 0; i < task_count; i++)
            tasks[i].sector_size = dev_sector_size;
    }

    for (int i = 0; i < task_count; i++)
        edl_flash_task_infer_sectors_from_image(&tasks[i]);

    *out_task_count = task_count;
    return EDL_OK;
}

static edl_error_t svc_apply_patch_entries(edl_service_t *svc,
                                           const edl_patch_entry_t *patches,
                                           int patch_count,
                                           const char *summary)
{
    if (!svc || !patches || patch_count <= 0)
        return EDL_ERR_INVALID_PARAM;

    edl_error_t ping_err = edl_firehose_ping(svc->firehose);
    if (ping_err != EDL_OK) {
        svc_log(svc, "%s前 NOP 失败: %s", summary, edl_error_str(ping_err));
        return ping_err;
    }

    for (int i = 0; i < patch_count; i++) {
        if (svc_is_cancelled(svc)) {
            svc_log(svc, "%s已取消（%d/%d）", summary, i + 1, patch_count);
            return EDL_ERR_CANCELLED;
        }

        const edl_patch_entry_t *patch = &patches[i];
        edl_error_t err = edl_firehose_apply_patch(svc->firehose,
                                                   patch->lun,
                                                   patch->start_sector,
                                                   patch->byte_offset,
                                                   patch->size_in_bytes,
                                                   patch->value);
        if (err != EDL_OK) {
            svc_log(svc, "补丁 %d/%d 应用失败: %s", i + 1, patch_count, edl_error_str(err));
            return err;
        }
    }

    svc_clear_partition_cache(svc);
    return EDL_OK;
}

edl_error_t edl_service_apply_patch_file(edl_service_t *svc, const char *patch_path)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;
    if (!patch_path || !patch_path[0])
        return EDL_ERR_INVALID_PARAM;

    edl_patch_entry_t *patches =
        (edl_patch_entry_t *)calloc(MAX_PATCHES, sizeof(edl_patch_entry_t));
    if (!patches)
        return EDL_ERR_NO_MEMORY;

    int patch_count = edl_patch_parse(patch_path, patches, MAX_PATCHES);
    if (patch_count < 0) {
        free(patches);
        svc_log(svc, "无法读取 patch 文件: %s", patch_path);
        return EDL_ERR_FILE_NOT_FOUND;
    }
    if (patch_count == 0) {
        free(patches);
        svc_log(svc, "patch 文件中无 <patch ...> 条目: %s", patch_path);
        return EDL_OK;
    }

    svc_log(svc, "正在应用 %d 条补丁（%s）...", patch_count, patch_path);
    edl_error_t err = svc_apply_patch_entries(svc, patches, patch_count, "应用补丁");
    free(patches);
    if (err != EDL_OK)
        return err;

    svc_log(svc, "补丁应用完成");
    return EDL_OK;
}

edl_error_t edl_service_write_gpt_from_rawprogram_xml(edl_service_t *svc,
                                                      const char *rawprogram_path,
                                                      const char *base_dir,
                                                      unsigned flags)
{
    if (!svc || !svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;
    if (!rawprogram_path || !rawprogram_path[0])
        return EDL_ERR_INVALID_PARAM;

    edl_flash_task_t *tasks =
        (edl_flash_task_t *)calloc(MAX_FLASH_TASKS, sizeof(edl_flash_task_t));
    if (!tasks)
        return EDL_ERR_NO_MEMORY;

    int task_count = 0;
    edl_error_t load_err = svc_load_rawprogram_tasks(svc, rawprogram_path, base_dir,
                                                     tasks, MAX_FLASH_TASKS, &task_count);
    if (load_err != EDL_OK) {
        free(tasks);
        return load_err;
    }

    int gpt_idx[64];
    int gpt_n = 0;
    for (int i = 0; i < task_count && gpt_n < 64; i++) {
        edl_flash_task_t *task = &tasks[i];
        if (task->type != EDL_TASK_PROGRAM || !edl_label_is_gpt(task->label))
            continue;
        if (!task->filepath[0]) {
            svc_log(svc, "跳过 %s：无镜像路径（XML 中 program 需含 filename）",
                    task->label[0] ? task->label : "?");
            continue;
        }
        if (task->num_sectors <= 0) {
            svc_log(svc, "%s 扇区数无效（<=0）", task->label[0] ? task->label : "GPT 项");
            free(tasks);
            return EDL_ERR_INVALID_PARAM;
        }
        gpt_idx[gpt_n++] = i;
    }

    if (gpt_n == 0) {
        free(tasks);
        svc_log(svc, "未找到可写入的 PrimaryGPT/BackupGPT（需 label 正确且含 filename）");
        return EDL_ERR_INVALID_PARAM;
    }

    for (int a = 0; a < gpt_n - 1; a++) {
        for (int b = a + 1; b < gpt_n; b++) {
            const char *lhs = tasks[gpt_idx[a]].label;
            const char *rhs = tasks[gpt_idx[b]].label;
            if (svc_gpt_label_order(rhs) < svc_gpt_label_order(lhs)) {
                int tmp = gpt_idx[a];
                gpt_idx[a] = gpt_idx[b];
                gpt_idx[b] = tmp;
            }
        }
    }

    svc_log(svc, "写 GPT（XML）：%s，共 %d 项", rawprogram_path, gpt_n);

    edl_error_t ping_err = edl_firehose_ping(svc->firehose);
    if (ping_err != EDL_OK) {
        free(tasks);
        svc_log(svc, "写入 GPT 前 NOP 失败: %s", edl_error_str(ping_err));
        return ping_err;
    }

    for (int i = 0; i < gpt_n; i++) {
        edl_partition_info_t part;
        edl_flash_task_to_partition_info(&tasks[gpt_idx[i]], &part);
        svc_log(svc, "[%d/%d] 写入 %s，LUN%d 起始扇区 %lld",
                i + 1, gpt_n, part.name, part.lun, (long long)part.start_sector);
        edl_error_t err = edl_service_write_partition(svc, &part, tasks[gpt_idx[i]].filepath);
        if (err != EDL_OK) {
            svc_log(svc, "写入 %s 失败: %s", part.name, edl_error_str(err));
            free(tasks);
            return err;
        }
    }
    free(tasks);

    if (flags & EDL_FLASH_XML_RUN_FIXGPT) {
        svc_log(svc, "修复 GPT（主备同步 + CRC）...");
        edl_error_t fix_err = edl_service_fix_gpt(svc);
        if (fix_err != EDL_OK)
            svc_log(svc, "GPT 修复失败: %s", edl_error_str(fix_err));
        else
            svc_log(svc, "GPT 修复成功");
    } else {
        svc_log(svc, "已跳过 fixgpt（与「刷机后执行 fixgpt」选项一致：未勾选则跳过）");
    }

    svc_log(svc, "写 GPT（XML）完成");
    return EDL_OK;
}

edl_error_t edl_service_flash_xml_ex(edl_service_t *svc,
                                     const char *rawprogram_path,
                                     const char *patch_path,
                                     const char *base_dir,
                                     unsigned flags)
{
    if (!svc || !svc->connected || !rawprogram_path)
        return EDL_ERR_INVALID_PARAM;

    edl_flash_task_t *tasks =
        (edl_flash_task_t *)calloc(MAX_FLASH_TASKS, sizeof(edl_flash_task_t));
    if (!tasks)
        return EDL_ERR_NO_MEMORY;

    int task_count = 0;
    edl_error_t load_err = svc_load_rawprogram_tasks(svc, rawprogram_path, base_dir,
                                                     tasks, MAX_FLASH_TASKS, &task_count);
    if (load_err != EDL_OK) {
        free(tasks);
        return load_err;
    }

    int program_task_count = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].type == EDL_TASK_PROGRAM)
            program_task_count++;
    }

    svc_log(svc, "从 %s 加载了 %d 个刷写任务（PROGRAM %d 个）",
            rawprogram_path, task_count, program_task_count);

    edl_error_t ping_err = edl_firehose_ping(svc->firehose);
    if (ping_err != EDL_OK) {
        svc_log(svc, "刷写前 NOP 失败: %s", edl_error_str(ping_err));
        free(tasks);
        return ping_err;
    }

    int programs_completed = 0;
    int wrote_a_count = 0;
    int wrote_b_count = 0;
    bool patch_xml_applied = false;

    for (int i = 0; i < task_count; i++) {
        if (svc_is_cancelled(svc)) {
            svc_log(svc, "刷写已取消（%d/%d）", i + 1, task_count);
            free(tasks);
            return EDL_ERR_CANCELLED;
        }

        edl_flash_task_t *task = &tasks[i];
        svc_log(svc, "[%d/%d] %s: %s (LUN%d 扇区 %lld)",
                i + 1, task_count,
                task->type == EDL_TASK_PROGRAM ? "PROGRAM" :
                task->type == EDL_TASK_ERASE ? "ERASE" : "ZEROOUT",
                task->label[0] ? task->label : task->filename,
                task->lun, (long long)task->start_sector);

        edl_error_t err = EDL_OK;
        if (task->type == EDL_TASK_PROGRAM) {
            edl_partition_info_t part;
            edl_flash_task_to_partition_info(task, &part);
            err = edl_service_write_partition(svc, &part, task->filepath);
            if (err == EDL_OK) {
                programs_completed++;
                if (svc_part_name_has_slot_suffix(part.name, 'a'))
                    wrote_a_count++;
                else if (svc_part_name_has_slot_suffix(part.name, 'b'))
                    wrote_b_count++;

                if (program_task_count >= 6 && programs_completed % 5 == 0 &&
                    programs_completed < program_task_count) {
                    svc_log(svc, "批量刷写：已完成 %d/%d 个 PROGRAM，发送 NOP...",
                            programs_completed, program_task_count);
                    ping_err = edl_firehose_ping(svc->firehose);
                    if (ping_err != EDL_OK) {
                        svc_log(svc, "批量 NOP 失败: %s", edl_error_str(ping_err));
                        free(tasks);
                        return ping_err;
                    }
                }
            }
        } else {
            if (task->num_sectors <= 0) {
                svc_log(svc, "任务 %d: ERASE/ZEROOUT 的 num_partition_sectors 无效（<=0）",
                        i + 1);
                free(tasks);
                return EDL_ERR_INVALID_PARAM;
            }

            char xml[2048];
            char resp[256];
            if (task->type == EDL_TASK_ZEROOUT) {
                edl_xml_build_zeroout(xml, sizeof(xml), task->sector_size, task->lun,
                                      task->start_sector, task->num_sectors);
            } else {
                edl_xml_build_erase(xml, sizeof(xml), task->sector_size, task->lun,
                                    task->start_sector, task->num_sectors);
            }
            err = edl_firehose_send_xml(svc->firehose, xml, resp, sizeof(resp));
        }

        if (err != EDL_OK) {
            svc_log(svc, "刷写任务 %d 失败: %s", i + 1, edl_error_str(err));
            free(tasks);
            return err;
        }
    }

    if (patch_path && patch_path[0]) {
        edl_patch_entry_t *patches =
            (edl_patch_entry_t *)calloc(MAX_PATCHES, sizeof(edl_patch_entry_t));
        if (!patches) {
            free(tasks);
            return EDL_ERR_NO_MEMORY;
        }

        int patch_count = edl_patch_parse(patch_path, patches, MAX_PATCHES);
        if (patch_count > 0) {
            svc_log(svc, "正在应用 %d 个补丁（patch XML，PROGRAM 任务数 %d）...",
                    patch_count, program_task_count);
            edl_error_t err = svc_apply_patch_entries(svc, patches, patch_count, "补丁应用");
            free(patches);
            if (err != EDL_OK) {
                free(tasks);
                return err;
            }
            patch_xml_applied = true;
        } else {
            free(patches);
        }
    }

    if (flags & EDL_FLASH_XML_RUN_FIXGPT) {
        svc_log(svc, "修复 GPT（主备同步 + CRC）...");
        edl_error_t fix_err = edl_service_fix_gpt(svc);
        if (fix_err != EDL_OK)
            svc_log(svc, "GPT 修复失败（可能影响启动）: %s", edl_error_str(fix_err));
        else
            svc_log(svc, "GPT 修复成功");
    } else {
        svc_log(svc, "已跳过 fixgpt（与「仅写 GPT」工具行为一致）");
    }

    if (flags & EDL_FLASH_XML_SET_BOOTABLE_AT_END) {
        if (!patch_xml_applied) {
            svc_log(svc, "已跳过刷写末尾启动分区激活：本次未应用 patch XML");
        } else {
            edl_partition_info_t gparts[EDL_SERVICE_MAX_PARTITIONS];
            int gpt_count = EDL_SERVICE_MAX_PARTITIONS;
            int max_lun = edl_service_default_gpt_max_lun(svc);

            svc_log(svc, "检测到已应用 patch：激活启动分区前按安全模式回读 GPT");
            if (edl_service_read_gpt_ex(svc, gparts, &gpt_count, max_lun, 0u) == EDL_OK &&
                gpt_count > 0) {
                edl_service_activate_boot_lun_sakura(svc, gparts, gpt_count,
                                                     wrote_a_count, wrote_b_count);
            } else {
                svc_log(svc, "安全模式回读 GPT 失败：已跳过刷写末尾启动分区激活");
            }
        }
    }

    free(tasks);
    svc_log(svc, "刷写完成!");
    return EDL_OK;
}

edl_error_t edl_service_flash_xml(edl_service_t *svc,
                                  const char *rawprogram_path,
                                  const char *patch_path,
                                  const char *base_dir)
{
    return edl_service_flash_xml_ex(svc, rawprogram_path, patch_path, base_dir,
                                    EDL_FLASH_XML_RUN_FIXGPT);
}

edl_error_t edl_service_misc_write_image_and_reset(edl_service_t *svc,
                                                   const char *image_path,
                                                   const char *part_name)
{
    if (!svc || !image_path || !image_path[0])
        return EDL_ERR_INVALID_PARAM;
    if (!svc->connected || !svc->firehose)
        return EDL_ERR_PORT_CLOSED;

    edl_error_t err = edl_service_ensure_gpt_cache_ex(svc, 0u);
    if (err != EDL_OK)
        return err;

    const edl_partition_info_t *part = NULL;
    if (part_name && part_name[0]) {
        part = edl_service_find_partition(svc, part_name);
    } else {
        static const char *const candidates[] = { "misc", "MISC", NULL };
        for (int i = 0; candidates[i] && !part; i++)
            part = edl_service_find_partition(svc, candidates[i]);
    }
    if (!part) {
        svc_log(svc, "未找到 misc 分区（请先读取 GPT）");
        return EDL_ERR_FH_PARTITION_NOT_FOUND;
    }

    svc_log(svc, "正在写入 misc 并发送重启指令...");
    err = edl_service_write_partition(svc, part, image_path);
    if (err != EDL_OK)
        return err;

    return edl_service_reboot(svc, "reset");
}
