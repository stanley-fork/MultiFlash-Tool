/*
 * MSM / OEM 映射维护：
 * - bkerler/edl：edlclient/Config/qualcomm_config.py（GPLv3）
 *   https://github.com/bkerler/edl/blob/master/edlclient/Config/qualcomm_config.py
 *   msmids、vendor 可定期 diff 合并。
 * - hoplik/Firehose-Finder（MIT）：机型 / Firehose / PK Hash 对照更丰富
 *   https://github.com/hoplik/Firehose-Finder
 *   批量解析 ForFilter.xml → CSV/C 片段：tools/parse_firehose_finder_filter.py
 *   有用数据归纳（库外 MSM、名称差异、空名称等）：tools/analyze_firehose_data.py
 *   库外条目与 edl_chip_name 模糊规则核对：tools/verify_firehose_chip_db.py → build 报告
 *   · ForFilter.xml：上游多为 Access 导出 XML（<ForFilter><HWID>…）；旧版亦有按行文本，解析脚本均支持。
 *   · fh_collection/<哈希前缀>/：按 PK 前缀归档的 programmer 集合。
 *   · README 中 temblast 等外链为 Loader 检索表（注意版权与更新频率）。
 * - 注意：同一 MSM 在不同数据源中可能对应不同平台（例如 0x001750E1 在 bkerler 为
 *   nicobar IoT，在 Firehose-Finder 样本中为 QCM6125/trinket）。本表以当前 Sahara 日志
 *   与 bkerler 为主；冲突时请以实机 + Loader 侧行为准。
 * - 本表消费级名称在 codename 上归纳；与高通公开 brief 不一致时以设备实测为准。
 */
#include "edl/chip_db.h"
#include <string.h>
#include <stdio.h>

/* ===== MSM ID → 芯片名 ===== */

typedef struct { uint32_t id; const char *name; } msm_entry_t;

static const msm_entry_t msm_table[] = {
    /* Snapdragon 2xx */
    {0x009600E1,"MSM8909 (Snapdragon 210)"},{0x007220E1,"MSM8909 (Snapdragon 210)"},
    {0x007200E1,"APQ8009 (Snapdragon 212)"},{0x001AE0E1,"QCM2290"},
    {0x0015A0E1,"SM4125 (Snapdragon 215)"},
    /* Snapdragon 4xx */
    {0x007050E1,"MSM8916 (Snapdragon 410)"},{0x007060E1,"APQ8016 (Snapdragon 410)"},
    {0x007070E1,"MSM8916 (Snapdragon 410)"},{0x009150E1,"MSM8226 (Snapdragon 400)"},
    {0x000460E1,"MSM8953 (Snapdragon 625)"},{0x0004F0E1,"MSM8937 (Snapdragon 430)"},
    {0x000510E1,"MSM8917 (Snapdragon 425)"},{0x0009A0E1,"SDM450 (Snapdragon 450)"},
    {0x000BE0E1,"SDM429 (Snapdragon 429)"},{0x000BF0E1,"SDM439 (Snapdragon 439)"},
    {0x001610E1,"SDM439 (Snapdragon 439)"},
    {0x001190E1,"SM4350 (Snapdragon 480)"},{0x0013F0E1,"SM4250 (Snapdragon 460)"},
    {0x001B90E1,"SM4375 (Snapdragon 4 Gen 1)"},{0x001BD0E1,"SM4450 (Snapdragon 4 Gen 2)"},
    {0x001FD0E1,"SM4635 (Snapdragon 4s Gen 2)"},{0x0027A0E1,"SM4550 (Snapdragon 4 Gen 3)"},
    {0x002980E1,"SM4635 (Snapdragon 4s Gen 2)"},
    /* Snapdragon 6xx */
    {0x000AC0E1,"SDM630 (Snapdragon 630)"},{0x0007E0E1,"SDM630 (Snapdragon 630)"},
    {0x000CC0E1,"SDM636 (Snapdragon 636)"},
    {0x0008C0E1,"SDM660 (Snapdragon 660)"},{0x0009C0E1,"SDM660 (Snapdragon 660)"},
    {0x000BA0E1,"SDM632 (Snapdragon 632)"},
    {0x000950E1,"SM6150 (Snapdragon 675)"},{0x0010E0E1,"SM6125 (Snapdragon 665)"},
    {0x0013E0E1,"SM6115 (Snapdragon 662)"},{0x0015E0E1,"SM6350 (Snapdragon 690)"},
    {0x0019E0E1,"SM6375 (Snapdragon 695)"},{0x001BE0E1,"SM6225 (Snapdragon 680)"},
    {0x001B80E1,"SM6225 (Snapdragon 680)"},
    {0x0021E0E1,"SM6450 (Snapdragon 6 Gen 1)"},{0x001DB0E1,"SM6450 (Snapdragon 6 Gen 1)"},
    {0x002790E1,"SM6475-AB (Snapdragon 6 Gen 3)"},{0x002DC0E1,"SM6475-AB (Snapdragon 6 Gen 3)"},
    {0x009720E1,"MSM8952 (Snapdragon 617)"},{0x009840E1,"MSM8952 (Snapdragon 617)"},
    /* Snapdragon 7xx */
    {0x000910E1,"SDM670 (Snapdragon 670)"},{0x000DB0E1,"SDM710 (Snapdragon 710)"},
    {0x000DD0E1,"SDM712 (Snapdragon 712)"},{0x001080E1,"SDM712 (Snapdragon 712)"},
    {0x000E70E1,"SM7150-AA (Snapdragon 730)"},{0x000E60E1,"SM7150-AA (Snapdragon 730)"},
    {0x000E80E1,"SM7150-AB (Snapdragon 730G)"},{0x0011E0E1,"SM7250-AA (Snapdragon 765)"},
    {0x0011F0E1,"SM7250-AC (Snapdragon 768G)"},    {0x0017C0E1,"SM7225-2-AB (Snapdragon 750G)"},
    /* Snapdragon 750G 社区样本长期稳定落在 SM7225-2-AB / bitra_h 记法；用于显示与 loader 区分。 */
    {0x001920E1,"SM7325/SM7315 (Snapdragon 778G)"},{0x001630E1,"SM7225-2-AB (Snapdragon 750G)"},
    {0x001B50E1,"SM7325/SM7315 (Snapdragon 778G)"},
    {0x001CE0E1,"SM7435-AB (Snapdragon 7s Gen 2)"},{0x0026F0E1,"SM7435-AB (Snapdragon 7s Gen 2)"},
    {0x001DE0E1,"SM7450-0/-1-AB (Snapdragon 7 Gen 1)"},{0x000710E1,"SM7450-0/-1-AB (Snapdragon 7 Gen 1)"},
    {0x001DF0E1,"SM7475-AB (Snapdragon 7+ Gen 2)"},{0x0023E0E1,"SM7550-AB (Snapdragon 7 Gen 3)"},
    {0x0026B0E1,"SM7550-AB (Snapdragon 7 Gen 3)"},
    {0x0025E0E1,"SM7675-AB (Snapdragon 7+ Gen 3)"},{0x002BB0E1,"SM7675-AB (Snapdragon 7+ Gen 3)"},
    /* Snapdragon 8xx */
    {0x007B00E1,"MSM8974 (Snapdragon 800)"},{0x007B20E1,"MSM8974-AB (Snapdragon 801)"},
    {0x007B60E1,"MSM8974-AB (Snapdragon 801)"},
    {0x009400E1,"MSM8994 (Snapdragon 810)"},{0x009470E1,"MSM8996 (Snapdragon 820)"},
    {0x009690E1,"MSM8992 (Snapdragon 808)"},
    {0x0005F0E1,"MSM8996Pro (Snapdragon 821)"},{0x0005E0E1,"MSM8998 (Snapdragon 835)"},
    {0x0008B0E1,"SDM845 (Snapdragon 845)"},{0x000A50E1,"SM8150 (Snapdragon 855)"},
    {0x000E90E1,"SM8150 (Snapdragon 855)"},
    {0x000A60E1,"SM8150-AC (Snapdragon 855+)"},{0x000C30E1,"SM8250 (Snapdragon 865)"},
    {0x000C40E1,"SM8250-AB (Snapdragon 865+)"},{0x001350E1,"SM8350 (Snapdragon 888)"},
    {0x001360E1,"SM8350-AC (Snapdragon 888+)"},{0x001620E1,"SM8450 (Snapdragon 8 Gen 1)"},
    {0x001870E1,"SM8450 (Snapdragon 8 Gen 1)"},
    {0x001900E1,"SM8475/SM8425 (Snapdragon 8+ Gen 1)"},{0x001D90E1,"SM8475/SM8425 (Snapdragon 8+ Gen 1)"},
    /* 8 Gen 2/3/Elite 官方 brief 列出多个 part number，但未公开到具体 MSM ID，故按官方可见范围展示。 */
    {0x001CA0E1,"SM8550-AB/AC (Snapdragon 8 Gen 2)"},{0x001CB0E1,"SM8550-AB/AC (Snapdragon 8 Gen 2)"},
    {0x0022A0E1,"SM8650-AA/AB/AC (Snapdragon 8 Gen 3)"},{0x002280E1,"SM8650-AA/AB/AC (Snapdragon 8 Gen 3)"},
    {0x002270E1,"SM8650-AA/AB/AC (Snapdragon 8 Gen 3)"},
    {0x0026A0E1,"SM8635 (Snapdragon 8s Gen 3)"},{0x002750E1,"SM8635 (Snapdragon 8s Gen 3)"},
    {0x0028C0E1,"SM8750-AB/AC (Snapdragon 8 Elite)"},{0x0028D0E1,"SM8750-AB/AC (Snapdragon 8 Elite)"},
    {0x0028C0E2,"SM8750-AB/AC (Snapdragon 8 Elite)"},
    /* 基带 */
    {0x000320E1,"Snapdragon X16 LTE"},{0x0004A0E1,"Snapdragon X5 LTE Modem"},
    {0x0007D0E1,"Snapdragon X20 LTE Modem"},
    {0x0009E0E1,"SDX55 (X55 5G)"},{0x000CF0E1,"SDX55 (X55 5G)"},
    {0x001650E1,"SDX60 (X60 5G)"},
    {0x001600E1,"SDX65 (X65 5G)"},{0x0022D0E1,"SDX75 (X75 5G)"},
    {0x002850E1,"SDX80 (X80 5G)"},{0x001890E1,"Snapdragon X12-2"},
    {0x007F10E1,"Snapdragon X5 LTE Modem"},{0x007F40E1,"Snapdragon X5 LTE Modem"},
    {0x009220E1,"MDM9635 (X7)"},{0x009500E1,"Snapdragon LTE Modem"},
    {0x009510E1,"Snapdragon X12 LTE（蜂窝模组）"}, /* Firehose-Finder ForFilter.xml */
    /* 计算平台 */
    {0x001D00E1,"SC8380X (Snapdragon X Elite)"},{0x0014A0E1,"SC8280X (Snapdragon 8cx Gen 3)"},
    /* IoT / 机器人 / 视觉 */
    {0x000B20E1,"QCS605"},{0x001510E1,"QCS6490"},{0x001C60E1,"QCS8550"},
    {0x0011C0E1,"QCS610 (Vision)"},{0x001290E1,"QCS410 (SM6150 IoT Low)"},
    {0x001250E1,"SA515M (车载通信)"},
    /*
     * 非标准/厂商导出 HWID：
     * - 0x30020000 来自 Firehose-Finder 的 ZTE Nubia Z17S 样本，FullName=Snapdragon 835。
     *   这不是常规 Qualcomm HWID 形态，仅用于显示兜底，不参与标准 Sahara HWID 推断。
     */
    {0x30020000,"MSM8998 (Snapdragon 835)"},
    /*
     * 以下 MSM 主要来自 bkerler msmids；亦可对照 Firehose-Finder ForFilter.xml 补名称。
     * 与上行主表 ID 不重复。格式：内部代号 + 常见骁龙/场景说明。
     */
    /* 早期 AP / 低端 */
    {0x000160E1,"QCA4020 (IoT)"},{0x000400E1,"rennell_cb (720G CB)"},
    {0x000480E1,"MDM9207 (Snapdragon X5 LTE Modem)"},{0x000500E1,"APQ8037"},
    {0x000520E1,"APQ8009w"},{0x000550E1,"APQ8017"},{0x000560E1,"MSM8917 (425)"},
    {0x000580E1,"MSM8217"},{0x000620E1,"APQ8098"},{0x000630E1,"MSM8996AU"},
    {0x000640E1,"APQ8096SG"},{0x000660E1,"APQ8053 (652)"},{0x0006B0E1,"MSM8940"},
    {0x0006C0E1,"MSM8997"},{0x0006F0E1,"MSM8996AU"},{0x000860E1,"MSM8920"},
    {0x000940E1,"MSM8905"},{0x000960E1,"SDX24 (Snapdragon X24 LTE Modem)"},{0x000970E1,"SDX24M"},
    {0x0004E0E1,"APQ8096AU"},{0x006220E1,"MSM7227A"},
    {0x008000E1,"MSM8226 (400)"},{0x008010E1,"MSM8626"},{0x008020E1,"MSM8526"},
    {0x008030E1,"MSM8126"},{0x008040E1,"APQ8026"},{0x008050E1,"MSM8926 (400)"},
    {0x008060E1,"MSM8326 (S4)"},{0x008080E1,"MSM8512 (200)"},{0x0080A0E1,"FSM9965"},
    {0x0080C0E1,"FSM9950"},{0x0080D0E1,"FSM9915"},{0x0080E0E1,"FSM9910"},
    {0x0080F0E1,"FSM9900"},{0x008100E1,"MSM8110 (200)"},{0x008110E1,"MSM8210"},
    {0x008120E1,"MSM8610 (200)"},{0x008130E1,"MSM8810 (200)"},{0x008140E1,"MSM8212"},
    {0x008150E1,"MSM8612"},{0x008160E1,"MSM8112 (200)"},{0x008170E1,"MSM8510 (200)"},
    {0x007190E1,"APQ8064"},{0x007210E1,"MSM8930"},{0x0072C0E1,"MSM8960"},
    {0x007B30E1,"APQ8074"},{0x007B40E1,"MSM8974AB (801)"},{0x007B50E1,"msm8674_pro (801)"},
    {0x007B80E1,"MSM8974Pro (801)"},{0x007BC0E1,"MSM8974ABv3"},{0x007BD0E1,"MSM8674_AA (800)"},
    {0x006B10E1,"MSM8974AC (801)"},{0x008D0E1,"SDM658"},
    {0x008E0E1,"SDA845"},{0x008F0E1,"SDM830"},{0x009020E1,"MDM9635 (X7)"},
    {0x009100E1,"MSM8962"},{0x009110E1,"MSM8262"},{0x009130E1,"APQ8028 (400)"},
    {0x009140E1,"MSM8128"},{0x009160E1,"MSM8528 (400)"},{0x009170E1,"MSM8628 (400)"},
    {0x009180E1,"MSM8928 (400)"},{0x009300E1,"APQ8092"},{0x009570E1,"MSM8239 (615)"},
    {0x009640E1,"MSM8992 (Snapdragon 808)"},{0x009660E1,"MDM9309"},
    {0x009820E1,"MSM8976 (Snapdragon 652)"},
    {0x009830E1,"APQ8076 (Snapdragon 652)"},{0x009900E1,"MSM8956 (Snapdragon 650)"},
    {0x009B00E1,"MSM8956 (Snapdragon 650) alt"},
    {0x0090E0E1,"MSM8236"},{0x0090F0E1,"APQ8037"},{0x009770E1,"APQ8052"},
    {0x009F00E1,"APQ8056 (650)"},{0x009120E1,"APQ8062"},{0x009D70E1,"MSM8229"},
    {0x0008A0E1,"APQ807x"},{0x009000E1,"APQ8084"},{0x009010E1,"APQ8084 (805)"},
    {0x009410E1,"APQ8094 (810)"},{0x009630E1,"APQ8092"},{0x009680E1,"APQ8009"},
    {0x0090B0E1,"MSM8939 (615)"},{0x0090C0E1,"APQ8036"},{0x0090D0E1,"APQ8039 (615)"},
    {0x0091B0E1,"MSM8929 (415)"},{0x009620E1,"MSM8208 (205)"},
    {0x0094B0E1,"MSM9055"},{0x009440E1,"QDF2432"},
    /* 中高端 / PC / 7 系变体 */
    {0x000B70E1,"SDM850 (ACPC)"},{0x000B90E1,"SDA450"},{0x000BC0E1,"SDA630"},
    {0x000CB0E1,"SM8150 (Snapdragon 855)"},{0x000CE0E1,"SM8250-AB (Snapdragon 865+)"},
    {0x000DA0E1,"SC8180xp (8cx)"},{0x000EC0E1,"SM6150p (675)"},
    {0x000EF0E1,"SDM660 (660)"},{0x000F20E1,"SA4155p (车载)"},{0x000F50E1,"SM6155 (SA6155)"},
    {0x0009D0E1,"SDA658 (660/658)"},{0x001060E1,"QM215 (215 Mobile Platform)"},{0x001070E1,"MDM9205"},
    {0x001230E1,"SA8189P (8cx 汽车)"},{0x001260E1,"IPQ6018 (路由器)"},
    {0x001280E1,"fsm100xx"},{0x0012A0E1,"SM7125 (Snapdragon 720G)"},{0x0012B0E1,"SM7125 (Snapdragon 720G)"},
    {0x0012C0E1,"SC7180 (Snapdragon 7c)"},{0x001410E1,"SM6350 (Snapdragon 690)"},
    {0x001420E1,"SM8350 (Snapdragon 888)"},{0x001430E1,"SM7250-AB (Snapdragon 765G)"},
    {0x001440E1,"SM6375 (Snapdragon 695)"},{0x001450E1,"agatti_mdm"},{0x001470E1,"moselle"},
    {0x0014B0E1,"SA8295P"},
    {0x001490E1,"SM7125 (Snapdragon 720G)"},{0x0014D0E1,"SM6115 (662) SDM662"},
    {0x0014F0E1,"QCM2150 agatti"},{0x001520E1,"SM8350 (Snapdragon 888)"},{0x001530E1,"IPQ5018"},
    /* QAIRT release notes: Mannar.LA -> SM4350；Kamorta.LA -> SM6115 / SM4250。 */
    /* 0x001590E1 的 Firehose 样本机型为 Xiaomi MI 11 Lite 5G (renoir)；官方 SoC 为 SM7350-AB / Snapdragon 780G。 */
    {0x001560E1,"SM8250 (Snapdragon 865)"},{0x001590E1,"SM7350-AB (Snapdragon 780G)"},{0x0016E0E1,"SM4350 (Snapdragon 480)"},
    {0x0016F0E1,"SM4350-AC (Snapdragon 480+)"},{0x001730E1,"SM6115/SM4250 family (Kamorta IoT modem)"},
    {0x001740E1,"SM6115/SM4250 family (Kamorta IoT APQ)"},{0x001850E1,"agatti_mdm_iot"},
    {0x001860E1,"QCS2290"},{0x0018A0E1,"SM7325-AE (Snapdragon 778G+)"},{0x0018B0E1,"qtang2"},
    {0x001930E1,"SC7280 (7c Gen 3)"},{0x001940E1,"SC7295"},{0x001970E1,"QCM6490"},
    {0x001980E1,"QCS6490"},{0x001990E1,"SDX65 (X65 5G)"},{0x001A40E1,"Vordonisi (888 系)"},
    {0x001A90E1,"SM6375 (Snapdragon 695)"},{0x001C70E1,"SM6115/SM4250 family (Kamorta QRB)"},
    /* 连接 / Wi‑Fi / 射频 / XR */
    {0x000AB0E1,"QCA6290"},{0x000D30E1,"QCN7605"},{0x000D50E1,"QCN7606"},
    {0x000D70E1,"QCA6595"},    {0x000D90E1,"QCA6390"},{0x000ED0E1,"SXR1120 (XR)"},
    {0x000EA0E1,"SXR1130 (XR)"},{0x001310E1,"QCA6480"},{0x0012D0E1,"QCA6491"},
    {0x0012E0E1,"QCA6481"},{0x0010A0E1,"SM6125 (Snapdragon 665)"},{0x0010B0E1,"QCN9000"},
    {0x0010C0E1,"QCN9001"},{0x0010D0E1,"QCN9003"},
    /* 0x0010E0E1 主表已为 SM6125，与 bkerler QCN9010 冲突故不收录 */
    {0x0010F0E1,"QCN9011"},{0x001110E1,"QCN9012"},{0x001140E1,"QCN9013"},
    {0x001150E1,"QCN9002"},
    /* 0x001750E1 的 ForFilter 多个样本直接标为 QCM6125；0x001760E1 仍保留 nicobar APQ family 表达。 */
    {0x001750E1,"QCM6125 (nicobar IoT modem)"},{0x001760E1,"QCM6125 family (nicobar IoT APQ)"},
    {0x001A60E1,"WCN7850"},{0x001A70E1,"WCN7851"},
    /* QCS IoT 系列 */
    {0x000E30E1,"QCS401"},{0x000E40E1,"QCS403"},{0x001040E1,"QCS404"},
    {0x000AF0E1,"QCS405"},{0x000EB0E1,"QCS407"},
    /* 路由器 / 网络 (常见刷机场景) */
    {0x009780E1,"IPQ4018"},{0x009790E1,"IPQ4019"},{0x009D00E1,"APQ8076"},
    {0x001AD0E1,"Networking Pro 620"},
};

#define MSM_TABLE_SIZE (sizeof(msm_table)/sizeof(msm_table[0]))

const char *edl_chip_name(uint32_t msm_id)
{
    for (int i = 0; i < (int)MSM_TABLE_SIZE; i++)
        if (msm_table[i].id == msm_id) return msm_table[i].name;

    /* 尝试替换末字节为 E1 */
    if ((msm_id & 0xFF) != 0xE1) {
        uint32_t alt = (msm_id & 0xFFFFFF00) | 0xE1;
        for (int i = 0; i < (int)MSM_TABLE_SIZE; i++)
            if (msm_table[i].id == alt) return msm_table[i].name;
    }

    /* 模糊匹配核心 ID */
    uint32_t core = msm_id & 0x00FFFFF0;
    for (int i = 0; i < (int)MSM_TABLE_SIZE; i++)
        if ((msm_table[i].id & 0x00FFFFF0) == core) return msm_table[i].name;

    return "Unknown";
}

const char *edl_chip_codename(uint32_t msm_id)
{
    const char *full = edl_chip_name(msm_id);
    if (strcmp(full, "Unknown") == 0) return NULL;
    /* 返回括号前的代号 */
    static char buf[64];
    const char *p = strchr(full, '(');
    if (p && p > full) {
        int len = (int)(p - full);
        while (len > 0 && full[len-1] == ' ') len--;
        if (len > 0 && len < (int)sizeof(buf)) { memcpy(buf, full, len); buf[len] = 0; return buf; }
    }
    return full;
}

/* ===== OEM ID → 厂商 ===== */

typedef struct { uint16_t id; const char *name; } vendor_entry_t;

/* OEM 号：合并 bkerler qualcomm_config.py vendor{} + 社区补充（0x0040 等） */
static const vendor_entry_t vendor_table[] = {
    {0x0000,"Qualcomm"},{0x0001,"Foxconn/Sony"},{0x0004,"ZTE"},
    {0x0011,"Smartisan"},{0x0015,"Huawei"},{0x0017,"Lenovo"},
    {0x0020,"Samsung"},{0x0029,"Asus"},{0x0030,"Haier"},{0x0031,"LG"},
    {0x0035,"Foxconn/Nokia"},{0x0040,"Lenovo"},{0x0042,"TCL / Alcatel"},
    {0x0045,"Nokia"},{0x0048,"YuLong / Coolpad"},{0x0051,"OPLUS"},
    {0x0070,"Google"},{0x0072,"Xiaomi / Redmi / POCO"},{0x0073,"Vivo / iQOO"},{0x00C8,"Motorola"},
    {0x0110,"POCO"},{0x0130,"GlocalMe"},{0x0139,"Lyf"},{0x0168,"Motorola"},
    {0x0187,"HMD / Nokia"},{0x01A4,"Honor / Huawei"},{0x01B0,"Motorola"},
    {0x01CF,"Nothing"},{0x0200,"Realme"},{0x0208,"Motorola"},{0x0228,"Motorola"},
    {0x0250,"Redmi"},{0x0260,"Honor"},{0x0270,"iQOO"},{0x0290,"Nothing"},
    {0x02E8,"Lenovo / Motorola"},{0x0300,"Sony"},{0x0328,"Motorola"},{0x0348,"Motorola"},
    {0x0368,"Motorola"},{0x03C8,"Motorola"},{0x1043,"Asus"},{0x1111,"Asus"},
    {0x143A,"Asus"},{0x1978,"Blackphone"},{0x2A70,"OnePlus"},{0x2A96,"Micromax"},
};

#define VENDOR_TABLE_SIZE (sizeof(vendor_table)/sizeof(vendor_table[0]))

const char *edl_vendor_by_oem(uint16_t oem_id)
{
    for (int i = 0; i < (int)VENDOR_TABLE_SIZE; i++)
        if (vendor_table[i].id == oem_id) return vendor_table[i].name;
    return NULL;
}

/* SM8350 / SD888 及之后 8 系旗舰 MSM_ID（与 chip 表一致） */
static const uint32_t realme_modern_msm_ids[] = {
    0x001350E1, 0x001360E1, /* SM8350 / 888+ */
    0x001620E1,             /* SM8450 */
    0x001900E1,             /* SM8475 */
    0x001CA0E1,             /* SM8550 */
    0x002280E1, 0x0022A0E1, /* SM8650 */
    0x0026A0E1,             /* SM8635 */
    0x0028C0E1, 0x0028C0E2, /* SM8750 */
};

typedef struct {
    uint16_t oem_id;
    uint16_t model_id;
    const char *brand;
} precise_brand_entry_t;

static const precise_brand_entry_t precise_brand_table[] = {
    /* OnePlus */
    {0x0051,0x459B,"OnePlus"},{0x0051,0x4985,"OnePlus"},
    {0x0051,0x4D67,"OnePlus"},{0x0051,0x4D6D,"OnePlus"},
    {0x0051,0x5141,"OnePlus"},{0x0051,0x5152,"OnePlus"},
    {0x0051,0x5192,"OnePlus"},{0x0051,0x5198,"OnePlus"},
    {0x2A70,0x3DB9,"OnePlus"},
    {0x2A70,0x41DB,"OnePlus"},
    /* Xiaomi / Vivo / Huawei groups */
    {0x0072,0x1024,"Xiaomi"},
    {0x0073,0x0003,"Vivo"},{0x0073,0x0004,"Vivo"},
    /* TCL / Alcatel family */
    {0x0042,0x0004,"Google"},{0x0042,0x0006,"Google"},
    {0x0042,0x0011,"Alcatel"},{0x0042,0x0018,"Alcatel"},
    {0x0042,0x0030,"Alcatel"},{0x0042,0x0032,"Alcatel"},
    {0x0042,0x0050,"Nokia"},{0x0042,0x0052,"TCL"},
    {0x0042,0x005D,"TCL"},
    /* Huawei family */
    {0x0015,0x0020,"Google"},{0x0015,0x0021,"Huawei"},
    {0x0015,0x003A,"Huawei"},{0x0015,0x0040,"Huawei"},
    {0x0015,0x0041,"Huawei"},{0x0015,0x0042,"Huawei"},
    {0x0015,0x0062,"Huawei"},{0x0015,0x0063,"Huawei"},
    {0x0015,0x0065,"Huawei"},{0x0015,0x0066,"Huawei"},
    {0x0015,0x0067,"Huawei"},{0x0015,0x0069,"Huawei"},
    {0x0015,0x006D,"Huawei"},{0x0015,0x006F,"Huawei"},
    {0x01A4,0x0001,"Huawei"},{0x01A4,0x0002,"Huawei"},
    /* Additional stable OEM+Model exact mappings (samples >= 2) */
    {0x0029,0x000F,"Asus"},{0x0029,0x0014,"Asus"},
    {0x0029,0x0020,"Asus"},{0x0029,0x0021,"Asus"},
    {0x0029,0x0028,"Asus"},{0x0029,0x003C,"Asus"},
    {0x0029,0x0041,"Asus"},{0x0029,0x0865,"Asus"},
    {0x0029,0x0875,"Asus"},{0x1111,0x2222,"Asus"},
    {0x1234,0x0001,"Asus"},{0x143A,0x8953,"Asus"},
    {0x2016,0xA239,"Asus"},
    {0x2324,0x3732,"Cat"},
    {0x0066,0x0008,"Google"},{0x0066,0x000A,"Google"},
    {0x1234,0x1234,"Hytera"},{0x001A,0x0001,"Inseego"},
    {0x0039,0x0029,"KYOCERA"},{0x0039,0x0060,"KYOCERA"},
    {0x0039,0x0065,"KYOCERA"},
    {0x0031,0x026C,"LG"},
    {0x0017,0x0004,"Lenovo"},
    {0x0040,0x0002,"Lenovo"},{0x0040,0x0040,"Lenovo"},
    {0x2016,0x1721,"Meizu"},
    {0x0035,0x9571,"Nokia"},
    {0x0038,0x1EFC,"SHARP"},{0x0038,0x579A,"SHARP"},
    {0x0038,0x66DA,"SHARP"},{0x0038,0xE110,"SHARP"},
    {0x0000,0xA0A8,"netgear"},
};

#define PRECISE_BRAND_TABLE_SIZE (sizeof(precise_brand_table)/sizeof(precise_brand_table[0]))

static const char *edl_precise_brand_by_pk_hash(const char *pk_hash);

static const char *edl_brand_by_oem_model(uint16_t oem_id, uint16_t model_id)
{
    if (model_id == 0)
        return NULL;

    for (int i = 0; i < (int)PRECISE_BRAND_TABLE_SIZE; i++) {
        if (precise_brand_table[i].oem_id == oem_id &&
            precise_brand_table[i].model_id == model_id)
            return precise_brand_table[i].brand;
    }
    return NULL;
}

const char *edl_brand_by_ids_ex(uint16_t oem_id, uint16_t model_id, const char *pk_hash,
                                edl_brand_source_t *source)
{
    const char *brand = edl_brand_by_oem_model(oem_id, model_id);
    if (brand) {
        if (source) *source = EDL_BRAND_SOURCE_OEM_MODEL;
        return brand;
    }

    brand = edl_precise_brand_by_pk_hash(pk_hash);
    if (brand) {
        if (source) *source = EDL_BRAND_SOURCE_PK_HASH;
        return brand;
    }

    brand = edl_vendor_by_oem(oem_id);
    if (brand) {
        if (source) *source = EDL_BRAND_SOURCE_OEM_FAMILY;
        return brand;
    }

    if (source) *source = EDL_BRAND_SOURCE_NONE;
    return NULL;
}

const char *edl_brand_by_ids(uint16_t oem_id, uint16_t model_id, const char *pk_hash)
{
    return edl_brand_by_ids_ex(oem_id, model_id, pk_hash, NULL);
}

const char *edl_brand_source_name(edl_brand_source_t source)
{
    switch (source) {
    case EDL_BRAND_SOURCE_PK_HASH: return "PK Hash";
    case EDL_BRAND_SOURCE_OEM_MODEL: return "OEM+机型";
    case EDL_BRAND_SOURCE_OEM_FAMILY: return "OEM 家族";
    default: return "未知";
    }
}

bool edl_realme_is_modern_platform(uint32_t msm_id)
{
    for (size_t i = 0; i < sizeof(realme_modern_msm_ids) / sizeof(realme_modern_msm_ids[0]); i++) {
        if (realme_modern_msm_ids[i] == msm_id)
            return true;
    }
    /* 同核心不同末字节 */
    uint32_t core = msm_id & 0xFFFFFF00;
    for (size_t i = 0; i < sizeof(realme_modern_msm_ids) / sizeof(realme_modern_msm_ids[0]); i++) {
        if ((realme_modern_msm_ids[i] & 0xFFFFFF00) == core)
            return true;
    }
    const char *name = edl_chip_name(msm_id);
    if (name && strcmp(name, "Unknown") != 0) {
        if (strstr(name, "SM8350") || strstr(name, "SM8450") || strstr(name, "SM8475") ||
            strstr(name, "SM8550") || strstr(name, "SM8650") || strstr(name, "SM8635") ||
            strstr(name, "SM8750") || strstr(name, "8 Gen") || strstr(name, "8+ Gen") ||
            strstr(name, "8s Gen") || strstr(name, "8 Elite"))
            return true;
    }
    return false;
}

/* ===== PK Hash 前缀 → 厂商 ===== */

typedef struct { const char *prefix; const char *vendor; } pk_entry_t;

static const pk_entry_t pk_table[] = {
    /* OPPO */
    {"2be76cee","OPPO"},{"d8e3b5a8","OPPO"},{"d53f19d2","OPPO"},
    {"13d7a19a","OPPO"},{"08239eab","OPPO"},{"daedb40c","OPPO"},
    {"f10bd691","OPPO"},
    /* OnePlus */
    {"2acf3a85","OnePlus"},{"7c15a98d","OnePlus"},{"a26bc257","OnePlus"},
    {"3cceb55b","OnePlus"},{"24de7daf","OnePlus"},{"3e18a198","OnePlus"},
    {"6519c91c","OnePlus"},{"8aabc662","OnePlus"},{"267bac27","OnePlus"},
    {"a469caf8","OnePlus"},
    /* Xiaomi */
    {"57158eaf","Xiaomi"},{"355d47f9","Xiaomi"},{"a7b8b825","Xiaomi"},
    {"1c845b80","Xiaomi"},{"58b4add1","Xiaomi"},{"dd0cba2f","Xiaomi"},
    {"1bebe386","Xiaomi"},{"c924a35f","Xiaomi"},
    /* Vivo */
    {"60ba997f","Vivo"},{"2c0a52ff","Vivo"},{"2e8bd2f5","Vivo"},
    /* Samsung */
    {"6e1f1dfa","Samsung"},{"893ed73f","Samsung"},{"79f3c689","Samsung"},
    {"b2f2bb07","Samsung"},{"7dad1baf","Samsung"},{"4dcefbb1","Samsung"},
    /* Motorola */
    {"628be3f4","Motorola"},{"99cbafe8","Motorola"},{"140f82e9","Motorola"},
    /* Lenovo */
    {"5cb51521","Lenovo"},{"99c8c13e","Lenovo"},
    /* ZTE */
    {"168d0bad","ZTE"},{"07cb63f6","ZTE"},
    /* Asus */
    {"18000eb7","Asus"},{"1e5d0b2a","Asus"},{"872011aa","Asus"},
    /* Nokia */
    {"7fe240dd","Nokia"},{"441e29fd","Nokia"},
    /* Huawei */
    {"6bc36951","Huawei"},{"5ef1d112","Huawei"},
    /* Nothing */
    {"6a4ee8e1","Nothing"},
    /* BlackShark */
    {"acb46529","BlackShark"},{"423e32d3","BlackShark"},
    /* Qualcomm */
    {"cc3153a8","Qualcomm"},{"7be49b72","Qualcomm"},{"afca69d4","Qualcomm"},
    /* Google */
    {"9ab13b3e","Google"},{"6fb2b36f","Google"},
    /* Realme */
    {"4c8e7a2d","Realme"},{"b7d93f6a","Realme"},{"e2a45c8f","Realme"},
    /* Redmi */
    {"d5e7f8a9","Redmi"},{"7c3b4a5d","Redmi"},
    /* POCO */
    {"6e9d2c7f","POCO"},{"a4b8c5d3","POCO"},
    /* iQOO */
    {"f9e8d7c6","iQOO"},{"5a6b7c8d","iQOO"},
};

#define PK_TABLE_SIZE (sizeof(pk_table)/sizeof(pk_table[0]))

const char *edl_vendor_by_pk_hash(const char *pk_hash)
{
    if (!pk_hash || strlen(pk_hash) < 8) return NULL;
    char prefix[9];
    for (int i = 0; i < 8; i++)
        prefix[i] = (pk_hash[i] >= 'A' && pk_hash[i] <= 'F') ? pk_hash[i] + 32 : pk_hash[i];
    prefix[8] = 0;

    for (int i = 0; i < (int)PK_TABLE_SIZE; i++)
        if (strcmp(pk_table[i].prefix, prefix) == 0) return pk_table[i].vendor;
    return NULL;
}

static const pk_entry_t precise_display_pk_table[] = {
    /* OPLUS family */
    {"82446e2f","OPPO"},{"c8b4fe1c","OPPO"},{"0cf6c9ce","OPPO"},
    {"24406932","OPPO"},{"77a22b92","OPPO"},
    {"dd7c5f2e","OnePlus"},{"c0c66e27","OnePlus"},{"de7a7d5a","OnePlus"},
    {"32942e37","Realme"},
    /* Xiaomi ecosystem */
    {"355d47f9","Xiaomi"},{"a7b8b825","Xiaomi"},{"c924a35f","Xiaomi"},
    {"e30429a5","Xiaomi"},
    /* BBK family */
    {"2c0a52ff","Vivo"},{"f188b3bc","Vivo"},
    /* Lenovo / Motorola */
    {"13d7a19a","Motorola"},{"172aff60","Motorola"},{"4aac65a9","Motorola"},
    {"982b2ece","Motorola"},{"81266d71","Motorola"},{"acbcf5a7","Motorola"},
    {"ba390623","Motorola"},{"f541e698","Motorola"},
    {"99c8c13e","Lenovo"},{"0374637d","Lenovo"},{"6db709fa","Lenovo"},
    /* Huawei / Honor */
    {"0c4261f3","Huawei"},{"a1a5c298","Huawei"},
    /* Additional stable exact PK prefixes (samples >= 2) */
    {"c7182735","AGM"},{"598fb494","Alcatel"},{"df561473","Alcatel"},
    {"18000eb7","Asus"},{"49d087ca","Asus"},{"54736aeb","Asus"},
    {"8ecf3eaa","Asus"},{"ad38beb6","Asus"},{"f069a87f","Asus"},
    {"fbd4de36","Asus"},
    {"ee43ab11","Cat"},{"5b49d99a","Dexp"},
    {"778b0aef","Google"},{"e693a06f","Google"},
    {"95b22c86","HTC"},{"06a0604b","Hisense"},{"007ce702","Honeywell"},
    {"0528e45a","Hytera"},
    {"1030cd12","LG"},{"2cf7619a","LG"},
    {"7c24b3be","LeEco"},{"b155b8bf","LeEco"},{"ea490ebb","LeEco"},
    {"315cbf03","Meizu"},{"40e4aeba","Meizu"},{"80215663","Meizu"},
    {"06917d46","Nokia"},{"1163cb4d","Nokia"},{"441e29fd","Nokia"},
    {"7fe240dd","Nokia"},{"857b5151","Nokia"},
    {"07046415","SHARP"},{"0fbb4bc2","SHARP"},{"ca68b4c0","SHARP"},
    {"1738585d","Samsung"},{"289c7c57","Samsung"},{"3cc959ac","Samsung"},
    {"57ee29c2","Samsung"},{"66a8583c","Samsung"},{"b2df7c76","Samsung"},
    {"aa0e1191","Yandex"},
    {"168d0bad","ZTE"},{"3767512b","ZTE"},{"404d181e","ZTE"},
    {"4232191d","ZTE"},{"693e38e5","ZTE"},{"eadae385","ZTE"},
    {"f4734e47","ZTE"},{"f89cac66","ZTE"},{"fdfc20db","ZTE"},
    {"b10d082f","Zebra Technologies"},{"15e7119b","vsmart"},
};

#define PRECISE_DISPLAY_PK_TABLE_SIZE (sizeof(precise_display_pk_table)/sizeof(precise_display_pk_table[0]))

static const char *edl_precise_brand_by_pk_hash(const char *pk_hash)
{
    if (!pk_hash || strlen(pk_hash) < 8) return NULL;
    char prefix[9];
    for (int i = 0; i < 8; i++)
        prefix[i] = (pk_hash[i] >= 'A' && pk_hash[i] <= 'F') ? pk_hash[i] + 32 : pk_hash[i];
    prefix[8] = 0;

    for (int i = 0; i < (int)PRECISE_DISPLAY_PK_TABLE_SIZE; i++)
        if (strcmp(precise_display_pk_table[i].prefix, prefix) == 0)
            return precise_display_pk_table[i].vendor;
    return NULL;
}

bool edl_requires_vip(const char *pk_hash)
{
    const char *v = edl_vendor_by_pk_hash(pk_hash);
    if (!v) return false;
    return strcmp(v, "OPPO") == 0 || strcmp(v, "Realme") == 0;
}

bool edl_is_oneplus(const char *pk_hash)
{
    const char *v = edl_vendor_by_pk_hash(pk_hash);
    return v && strcmp(v, "OnePlus") == 0;
}

bool edl_is_xiaomi(const char *pk_hash)
{
    const char *v = edl_vendor_by_pk_hash(pk_hash);
    if (!v) return false;
    return strcmp(v, "Xiaomi") == 0 || strcmp(v, "Redmi") == 0 || strcmp(v, "POCO") == 0;
}

edl_memory_type_t edl_guess_memory_type(uint32_t msm_id)
{
    const char *name = edl_chip_name(msm_id);
    if (strcmp(name, "Unknown") == 0) return EDL_MEM_UFS;
    if (strstr(name, "SM8") || strstr(name, "SC8") || strstr(name, "SM7") || strstr(name, "SM6"))
        return EDL_MEM_UFS;
    if (strstr(name, "MSM891") || strstr(name, "MSM890") || strstr(name, "SDM4") || strstr(name, "SDM6"))
        return EDL_MEM_EMMC;
    return EDL_MEM_UFS;
}

const char *edl_memory_type_name(edl_memory_type_t type)
{
    switch (type) {
    case EDL_MEM_UFS: return "ufs";
    case EDL_MEM_EMMC: return "emmc";
    case EDL_MEM_NAND: return "nand";
    default: return "unknown";
    }
}

const char *edl_loader_arch_hint_name(edl_loader_arch_hint_t hint)
{
    switch (hint) {
    case EDL_LOADER_ARCH_HINT_32: return "32-bit loader";
    case EDL_LOADER_ARCH_HINT_64: return "64-bit loader";
    default: return "unknown loader";
    }
}

const char *edl_auth_hint_name(edl_auth_hint_t hint)
{
    switch (hint) {
    case EDL_AUTH_HINT_NONE: return "无认证";
    case EDL_AUTH_HINT_OPLUS_VIP: return "OPLUS VIP";
    case EDL_AUTH_HINT_REALME_LEGACY: return "Realme Legacy";
    case EDL_AUTH_HINT_REALME_MODERN: return "Realme Modern";
    case EDL_AUTH_HINT_XIAOMI_BUILTIN: return "Xiaomi builtin";
    case EDL_AUTH_HINT_ONEPLUS: return "OnePlus";
    default: return "未知";
    }
}

const char *edl_guess_ddr_generation(uint32_t msm_id)
{
    const char *name = edl_chip_name(msm_id);
    if (strcmp(name, "Unknown") == 0)
        return "未知（MSM ID 无表项）";

    /* 按 SoC 公开市场常见内存控制器规格粗分；与 UFS/eMMC（edl_guess_memory_type）无关 */

    if (strstr(name, "SM8750") || strstr(name, "SM8650") || strstr(name, "SM8635") ||
        strstr(name, "SM8550") || strstr(name, "SM8475") || strstr(name, "SM8450") ||
        strstr(name, "SM8350") || strstr(name, "SC8380") || strstr(name, "SC8280"))
        return "LPDDR5 / LPDDR5X（SoC 常见规格，非 SPD/丝印）";

    if (strstr(name, "SM8250") || strstr(name, "SM8150") || strstr(name, "SM7250") ||
        strstr(name, "SM7150") || strstr(name, "SM7325") || strstr(name, "SM7225") ||
        strstr(name, "SM712") || strstr(name, "SDM712") || strstr(name, "SDM710"))
        return "LPDDR4x / LPDDR5（代际重叠，依机型，非 SPD）";

    if (strstr(name, "SDM845") || strstr(name, "SDM670") || strstr(name, "MSM8998") ||
        strstr(name, "SDM636") || strstr(name, "SDM660") || strstr(name, "SDM630"))
        return "LPDDR4x（典型，非 SPD）";

    if (strstr(name, "SDM439") || strstr(name, "SDM429") || strstr(name, "MSM891") ||
        strstr(name, "MSM890") || strstr(name, "SDM450"))
        return "LPDDR3 / LPDDR4（低端常见，非 SPD）";

    if (strstr(name, "SM6") || strstr(name, "SM7"))
        return "LPDDR4x / LPDDR5（依机型推断，非 SPD）";

    return "LPDDR4 或更早（粗推断，非 SPD）";
}

void edl_pk_hash_info(const char *pk_hash, char *buf, int buf_size)
{
    if (!pk_hash || strlen(pk_hash) < 8) {
        snprintf(buf, buf_size, "Unknown");
        return;
    }
    /* 检查是否全零 */
    bool all_zero = true;
    for (int i = 0; i < 10 && pk_hash[i]; i++)
        if (pk_hash[i] != '0') { all_zero = false; break; }
    if (all_zero) { snprintf(buf, buf_size, "无安全启动 (已解锁)"); return; }

    const char *v = edl_vendor_by_pk_hash(pk_hash);
    if (v) snprintf(buf, buf_size, "%s SecBoot", v);
    else   snprintf(buf, buf_size, "自定义 OEM");
}
typedef struct { uint32_t id; const char *codename; } precise_codename_entry_t;

static const precise_codename_entry_t precise_codename_table[] = {
    {0x000C30E1,"kona"},{0x000C40E1,"kona"},{0x000CE0E1,"kona"},{0x001560E1,"kona"},
    {0x001350E1,"lahaina"},{0x001360E1,"lahaina"},{0x001420E1,"lahaina"},{0x001520E1,"lahaina"},
    {0x001620E1,"taro"},{0x001870E1,"taro"},
    {0x001900E1,"cape"},{0x001D90E1,"cape"},
    {0x001CA0E1,"kalama"},{0x001CB0E1,"kalama"},
    {0x002280E1,"pineapple"},{0x0022A0E1,"pineapple"},{0x002270E1,"pineapple"},
    {0x0011E0E1,"lito"},{0x0011F0E1,"lito"},{0x001430E1,"lito"},
    {0x0015E0E1,"lagoon"},{0x001410E1,"lagoon"},
    {0x0019E0E1,"holi"},{0x001440E1,"holi"},{0x001590E1,"holi"},
    {0x0010E0E1,"bengal"},{0x0013E0E1,"bengal"},{0x0013F0E1,"bengal"},
};

const char *edl_chip_codename_precise(uint32_t msm_id)
{
    for (size_t i = 0; i < sizeof(precise_codename_table) / sizeof(precise_codename_table[0]); i++) {
        if (precise_codename_table[i].id == msm_id)
            return precise_codename_table[i].codename;
    }

    if ((msm_id & 0xFF) != 0xE1) {
        uint32_t alt = (msm_id & 0xFFFFFF00) | 0xE1;
        for (size_t i = 0; i < sizeof(precise_codename_table) / sizeof(precise_codename_table[0]); i++) {
            if (precise_codename_table[i].id == alt)
                return precise_codename_table[i].codename;
        }
    }

    {
        uint32_t core = msm_id & 0x00FFFFF0;
        for (size_t i = 0; i < sizeof(precise_codename_table) / sizeof(precise_codename_table[0]); i++) {
            if ((precise_codename_table[i].id & 0x00FFFFF0) == core)
                return precise_codename_table[i].codename;
        }
    }

    return NULL;
}

static void chipdb_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static void chipdb_trim_copy(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src || len == 0)
        return;

    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t'))
        len--;
    if (len >= dst_size)
        len = dst_size - 1;
    if (len == 0)
        return;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void chipdb_split_chip_name(const char *chip_name,
                                   char *soc_code, size_t soc_code_size,
                                   char *marketing_name, size_t marketing_name_size)
{
    if (soc_code && soc_code_size > 0)
        soc_code[0] = '\0';
    if (marketing_name && marketing_name_size > 0)
        marketing_name[0] = '\0';
    if (!chip_name || strcmp(chip_name, "Unknown") == 0)
        return;

    const char *open = strchr(chip_name, '(');
    const char *close = open ? strchr(open + 1, ')') : NULL;
    if (open && close && close > open + 1) {
        chipdb_trim_copy(soc_code, soc_code_size, chip_name, (size_t)(open - chip_name));
        chipdb_trim_copy(marketing_name, marketing_name_size, open + 1, (size_t)(close - open - 1));
        return;
    }

    chipdb_copy_text(soc_code, soc_code_size, chip_name);
    chipdb_copy_text(marketing_name, marketing_name_size, chip_name);
}

typedef struct {
    const char *brand;
    const char *model;
    const char *marketing_name;
} device_market_entry_t;

static const device_market_entry_t device_market_table[] = {
    { "Realme", "RMX1901", "realme X" },
    { "Realme", "RMX1901EX", "realme X" },
};

#define DEVICE_MARKET_TABLE_SIZE (sizeof(device_market_table) / sizeof(device_market_table[0]))

static bool chipdb_text_equals_nocase(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs)
        return false;
#ifdef _WIN32
    return _stricmp(lhs, rhs) == 0;
#else
    return strcasecmp(lhs, rhs) == 0;
#endif
}

static bool chipdb_brand_matches(const char *expected, const char *actual)
{
    if (!expected || !expected[0])
        return true;
    if (!actual || !actual[0])
        return false;
    return chipdb_text_equals_nocase(expected, actual);
}

static bool chipdb_model_matches(const char *expected, const char *actual)
{
    size_t expected_len = 0;
    char tail = '\0';

    if (!expected || !expected[0] || !actual || !actual[0])
        return false;

    expected_len = strlen(expected);
#ifdef _WIN32
    if (_strnicmp(expected, actual, (int)expected_len) != 0)
        return false;
#else
    if (strncasecmp(expected, actual, expected_len) != 0)
        return false;
#endif

    tail = actual[expected_len];
    return tail == '\0' || tail == '_' || tail == '-' || tail == ' ' || tail == '/';
}

const char *edl_device_marketing_name_by_model(const char *brand, const char *model)
{
    if (!model || !model[0])
        return NULL;

    for (size_t i = 0; i < DEVICE_MARKET_TABLE_SIZE; i++) {
        const device_market_entry_t *entry = &device_market_table[i];
        if (!chipdb_brand_matches(entry->brand, brand))
            continue;
        if (chipdb_model_matches(entry->model, model))
            return entry->marketing_name;
    }

    for (size_t i = 0; i < DEVICE_MARKET_TABLE_SIZE; i++) {
        const device_market_entry_t *entry = &device_market_table[i];
        if (chipdb_model_matches(entry->model, model))
            return entry->marketing_name;
    }

    return NULL;
}

static edl_loader_arch_hint_t chipdb_guess_loader_arch_hint(uint32_t msm_id)
{
    const char *name = edl_chip_name(msm_id);
    if (!name || strcmp(name, "Unknown") == 0)
        return EDL_LOADER_ARCH_HINT_UNKNOWN;

    if (strstr(name, "SM") || strstr(name, "SC") || strstr(name, "QCS") ||
        strstr(name, "QCM") || strstr(name, "SDX55") || strstr(name, "SDX60") ||
        strstr(name, "SDX65") || strstr(name, "SDX75") || strstr(name, "SDX80") ||
        strstr(name, "MSM8996") || strstr(name, "MSM8998") || strstr(name, "SDM"))
        return EDL_LOADER_ARCH_HINT_64;

    if (strstr(name, "MSM891") || strstr(name, "MSM890") || strstr(name, "MSM893") ||
        strstr(name, "MSM8940") || strstr(name, "MSM8952") || strstr(name, "MSM8953") ||
        strstr(name, "MSM822") || strstr(name, "MSM861") || strstr(name, "APQ80"))
        return EDL_LOADER_ARCH_HINT_32;

    return EDL_LOADER_ARCH_HINT_UNKNOWN;
}

static edl_auth_hint_t chipdb_guess_auth_hint(uint32_t msm_id, uint16_t oem_id,
                                              uint16_t model_id, const char *pk_hash)
{
    const char *brand = edl_brand_by_ids(oem_id, model_id, pk_hash);
    const char *oem = edl_vendor_by_oem(oem_id);

    if (brand && strcmp(brand, "Realme") == 0)
        return edl_realme_is_modern_platform(msm_id)
             ? EDL_AUTH_HINT_REALME_MODERN
             : EDL_AUTH_HINT_REALME_LEGACY;
    if (oem && strcmp(oem, "Realme") == 0)
        return edl_realme_is_modern_platform(msm_id)
             ? EDL_AUTH_HINT_REALME_MODERN
             : EDL_AUTH_HINT_REALME_LEGACY;
    if (edl_is_xiaomi(pk_hash))
        return EDL_AUTH_HINT_XIAOMI_BUILTIN;
    if (edl_is_oneplus(pk_hash))
        return EDL_AUTH_HINT_ONEPLUS;
    if (edl_requires_vip(pk_hash))
        return EDL_AUTH_HINT_OPLUS_VIP;
    return EDL_AUTH_HINT_NONE;
}

bool edl_query_platform_profile(uint32_t msm_id, uint16_t oem_id, uint16_t model_id,
                                const char *pk_hash, edl_platform_profile_t *out)
{
    if (!out)
        return false;

    memset(out, 0, sizeof(*out));
    out->msm_id = msm_id;

    const char *chip_name = edl_chip_name(msm_id);
    const char *codename = edl_chip_codename(msm_id);
    const char *precise_codename = edl_chip_codename_precise(msm_id);

    chipdb_copy_text(out->chip_name, sizeof(out->chip_name), chip_name);
    chipdb_copy_text(out->codename, sizeof(out->codename), codename);
    chipdb_copy_text(out->precise_codename, sizeof(out->precise_codename), precise_codename);
    chipdb_split_chip_name(chip_name, out->soc_code, sizeof(out->soc_code),
                           out->marketing_name, sizeof(out->marketing_name));

    if (!out->soc_code[0] && codename)
        chipdb_copy_text(out->soc_code, sizeof(out->soc_code), codename);
    if (!out->marketing_name[0] && chip_name && strcmp(chip_name, "Unknown") != 0)
        chipdb_copy_text(out->marketing_name, sizeof(out->marketing_name), chip_name);

    out->memory_type = edl_guess_memory_type(msm_id);
    out->loader_arch_hint = chipdb_guess_loader_arch_hint(msm_id);
    out->auth_hint = chipdb_guess_auth_hint(msm_id, oem_id, model_id, pk_hash);

    return chip_name && strcmp(chip_name, "Unknown") != 0;
}
