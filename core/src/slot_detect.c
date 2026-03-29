#include "edl/slot_detect.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* 与 SakuraEDL SlotAttributes / SlotDetector 对齐（GPT 属性 byte6 = bit 48-55） */
static uint8_t ab_flag_byte(uint64_t attributes)
{
    return (uint8_t)((attributes >> 48) & 0xFF);
}

static int get_priority(uint64_t attributes)
{
    return (int)(ab_flag_byte(attributes) & 0x03);
}

static bool is_active(uint64_t attributes)
{
    return (ab_flag_byte(attributes) & 0x04) != 0;
}

static bool is_successful(uint64_t attributes)
{
    return (ab_flag_byte(attributes) & 0x08) != 0;
}

static bool is_unbootable(uint64_t attributes)
{
    return (ab_flag_byte(attributes) & 0x10) != 0;
}

#ifdef _WIN32
#define EDL_STRICMP _stricmp
#else
#include <strings.h>
#define EDL_STRICMP strcasecmp
#endif

static bool ends_with_ab(const char *name, char *slot_ch)
{
    size_t n = strlen(name);
    if (n < 2 || name[n - 2] != '_')
        return false;
    char c = (char)(name[n - 1]);
    if (c == 'a' || c == 'A') {
        *slot_ch = 'a';
        return true;
    }
    if (c == 'b' || c == 'B') {
        *slot_ch = 'b';
        return true;
    }
    return false;
}

static void strip_ab_suffix(char *base, size_t base_size, const char *name)
{
    char ch;
    if (ends_with_ab(name, &ch)) {
        size_t n = strlen(name);
        if (n >= 2 && n < base_size) {
            memcpy(base, name, n - 2);
            base[n - 2] = '\0';
            return;
        }
    }
    snprintf(base, base_size, "%s", name);
}

static bool key_base_match(const char *base_lower)
{
    static const char *keys[] = {
        "boot", "system", "vendor", "abl", "xbl", "dtbo",
        "vbmeta", "product", "odm", "system_ext",
        "recovery", "modem", "dsp"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (EDL_STRICMP(base_lower, keys[i]) == 0)
            return true;
    }
    return false;
}

static bool is_excluded_name(const char *base_lower)
{
    return EDL_STRICMP(base_lower, "vendor_boot") == 0
        || EDL_STRICMP(base_lower, "init_boot") == 0;
}

typedef struct {
    int has_ab;
    /* 'a','b','u' (undefined), 'n' (nonexistent), '?' (unknown) */
    char slot;
} lun_slot_result_t;

static lun_slot_result_t detect_slot_one_lun(const edl_partition_info_t *parts, int count, int target_lun)
{
    lun_slot_result_t r = {0, 'n'};
    int ab_total = 0;
    for (int i = 0; i < count; i++) {
        if (parts[i].lun != target_lun)
            continue;
        char ch;
        if (ends_with_ab(parts[i].name, &ch))
            ab_total++;
    }
    if (ab_total == 0)
        return r;

    r.has_ab = 1;

    /* 关键分区（与 C# KeyPartitions 一致） */
    int slot_a_active = 0, slot_b_active = 0;
    double slot_a_pri_sum = 0.0, slot_b_pri_sum = 0.0;
    int slot_a_pri_n = 0, slot_b_pri_n = 0;
    int slot_a_succ = 0, slot_b_succ = 0;
    int slot_a_unboot = 0, slot_b_unboot = 0;

    for (int i = 0; i < count; i++) {
        if (parts[i].lun != target_lun)
            continue;
        char ch;
        if (!ends_with_ab(parts[i].name, &ch))
            continue;

        char base[EDL_PART_NAME_MAX];
        strip_ab_suffix(base, sizeof(base), parts[i].name);
        if (is_excluded_name(base))
            continue;
        if (!key_base_match(base))
            continue;

        uint64_t a = parts[i].attributes;
        if (ch == 'a') {
            if (is_active(a))
                slot_a_active++;
            slot_a_pri_sum += (double)get_priority(a);
            slot_a_pri_n++;
            if (is_successful(a))
                slot_a_succ++;
            if (is_unbootable(a))
                slot_a_unboot++;
        } else {
            if (is_active(a))
                slot_b_active++;
            slot_b_pri_sum += (double)get_priority(a);
            slot_b_pri_n++;
            if (is_successful(a))
                slot_b_succ++;
            if (is_unbootable(a))
                slot_b_unboot++;
        }
    }

    /* 若无关键分区命中，退化为所有非排除 A/B 分区（与 C# 一致） */
    if (slot_a_pri_n == 0 && slot_b_pri_n == 0) {
        for (int i = 0; i < count; i++) {
            if (parts[i].lun != target_lun)
                continue;
            char ch;
            if (!ends_with_ab(parts[i].name, &ch))
                continue;
            char base[EDL_PART_NAME_MAX];
            strip_ab_suffix(base, sizeof(base), parts[i].name);
            if (is_excluded_name(base))
                continue;

            uint64_t a = parts[i].attributes;
            if (ch == 'a') {
                if (is_active(a))
                    slot_a_active++;
                slot_a_pri_sum += (double)get_priority(a);
                slot_a_pri_n++;
                if (is_successful(a))
                    slot_a_succ++;
                if (is_unbootable(a))
                    slot_a_unboot++;
            } else {
                if (is_active(a))
                    slot_b_active++;
                slot_b_pri_sum += (double)get_priority(a);
                slot_b_pri_n++;
                if (is_successful(a))
                    slot_b_succ++;
                if (is_unbootable(a))
                    slot_b_unboot++;
            }
        }
    }

    double slot_ap = slot_a_pri_n > 0 ? slot_a_pri_sum / (double)slot_a_pri_n : 0.0;
    double slot_bp = slot_b_pri_n > 0 ? slot_b_pri_sum / (double)slot_b_pri_n : 0.0;

    if (slot_a_active != slot_b_active) {
        r.slot = (slot_a_active > slot_b_active) ? 'a' : 'b';
        return r;
    }
    if (slot_a_pri_n > 0 && slot_b_pri_n > 0 && fabs(slot_ap - slot_bp) > 0.1) {
        r.slot = (slot_ap > slot_bp) ? 'a' : 'b';
        return r;
    }
    if (slot_a_succ != slot_b_succ) {
        r.slot = (slot_a_succ > slot_b_succ) ? 'a' : 'b';
        return r;
    }
    if (slot_a_unboot != slot_b_unboot) {
        r.slot = (slot_a_unboot < slot_b_unboot) ? 'a' : 'b';
        return r;
    }
    if (slot_a_active > 0 && slot_b_active > 0) {
        r.slot = '?';
        return r;
    }
    if (slot_a_active == 0 && slot_b_active == 0) {
        r.slot = 'u';
        return r;
    }
    r.slot = '?';
    return r;
}

static void merge_lun_slots(const edl_partition_info_t *parts, int count,
                              int *lun_a, int *lun_b, char *merged_slot)
{
    *lun_a = *lun_b = 0;
    *merged_slot = 'n';

    for (int L = 0; L < 6; L++) {
        lun_slot_result_t s = detect_slot_one_lun(parts, count, L);
        if (!s.has_ab)
            continue;
        if (*merged_slot == 'n')
            *merged_slot = 'u';
        if (s.slot == 'a')
            (*lun_a)++;
        else if (s.slot == 'b')
            (*lun_b)++;
    }

    if (*lun_a > *lun_b && *lun_a > 0) {
        *merged_slot = 'a';
        return;
    }
    if (*lun_b > *lun_a && *lun_b > 0) {
        *merged_slot = 'b';
        return;
    }
    if (*lun_a > 0 && *lun_b > 0) {
        *merged_slot = '?';
        return;
    }
}

int edl_boot_lun_pick_sakura(const char *storage_type,
                             const edl_partition_info_t *parts, int count,
                             int wrote_a_count, int wrote_b_count,
                             char *detail_buf, size_t detail_len)
{
    if (detail_buf && detail_len > 0)
        detail_buf[0] = '\0';

    bool is_emmc = false;
    if (storage_type && EDL_STRICMP(storage_type, "emmc") == 0)
        is_emmc = true;

    int lun_a = 0, lun_b = 0;
    char merged = 'n';
    merge_lun_slots(parts, count, &lun_a, &lun_b, &merged);

    /* eMMC：通常单 LUN，无多 LUN A/B 合并；与 C# 一致无 A/B 则跳过 */
    if (is_emmc) {
        bool any_ab = false;
        for (int i = 0; i < count; i++) {
            char ch;
            if (ends_with_ab(parts[i].name, &ch)) {
                any_ab = true;
                break;
            }
        }
        if (!any_ab) {
            if (detail_buf && detail_len > 3)
                snprintf(detail_buf, detail_len, "eMMC 无 A/B，跳过 setbootable");
            return -1;
        }
        /* 有 A/B 时仍用属性判断当前槽，再映射到 LUN0（单盘） */
        lun_slot_result_t s0 = detect_slot_one_lun(parts, count, 0);
        if (!s0.has_ab) {
            if (detail_buf && detail_len > 3)
                snprintf(detail_buf, detail_len, "eMMC 未识别 A/B");
            return -1;
        }
        char pick = s0.slot;
        if (pick == 'u' || pick == '?') {
            if (wrote_a_count > wrote_b_count)
                pick = 'a';
            else if (wrote_b_count > wrote_a_count)
                pick = 'b';
            else if (wrote_a_count > 0 && wrote_b_count > 0)
                pick = 'a';
            else {
                if (detail_buf && detail_len > 3)
                    snprintf(detail_buf, detail_len, "eMMC 槽位未确定，跳过");
                return -1;
            }
        }
        if (detail_buf && detail_len > 8)
            snprintf(detail_buf, detail_len, "eMMC 槽位 %c -> LUN0", pick);
        (void)pick;
        return 0;
    }

    /* UFS：GPT 中无任何 *_a / *_b 后缀（单槽 / 与 C# CurrentSlot == nonexistent 一致）→ 不发送 setbootable */
    {
        bool any_ab = false;
        for (int i = 0; i < count; i++) {
            char ch;
            if (ends_with_ab(parts[i].name, &ch)) {
                any_ab = true;
                break;
            }
        }
        if (!any_ab) {
            if (detail_buf && detail_len > 3)
                snprintf(detail_buf, detail_len,
                         "UFS 无 A/B 后缀分区（单槽/nonexistent），跳过 setbootable");
            return -1;
        }
    }

    /* UFS */
    if (merged == 'a') {
        if (detail_buf && detail_len > 8)
            snprintf(detail_buf, detail_len, "UFS 槽位 A（多 LUN 合并）-> LUN1");
        return 1;
    }
    if (merged == 'b') {
        if (detail_buf && detail_len > 8)
            snprintf(detail_buf, detail_len, "UFS 槽位 B（多 LUN 合并）-> LUN2");
        return 2;
    }

    /*
     * 其余 merged 为 u / ?（槽位不明）；merged==n 且走到此处已不可能（无后缀时已在上方返回）。
     * 按本次任务分区名 _a / _b 计数回退（与 C# undefined/unknown 时按写入推断一致）。
     */
    if (wrote_a_count > wrote_b_count) {
        if (detail_buf && detail_len > 8)
            snprintf(detail_buf, detail_len, "槽位未决，按写入 _a 多 -> LUN1");
        return 1;
    }
    if (wrote_b_count > wrote_a_count) {
        if (detail_buf && detail_len > 8)
            snprintf(detail_buf, detail_len, "槽位未决，按写入 _b 多 -> LUN2");
        return 2;
    }
    if (wrote_a_count > 0 && wrote_b_count > 0) {
        if (detail_buf && detail_len > 8)
            snprintf(detail_buf, detail_len, "全量刷机默认 slot_a -> LUN1");
        return 1;
    }
    if (detail_buf && detail_len > 3)
        snprintf(detail_buf, detail_len, "无 A/B 或无法推断，跳过 setbootable");
    return -1;
}
