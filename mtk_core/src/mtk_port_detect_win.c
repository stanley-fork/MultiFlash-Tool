#include "mtk/mtk_port_detect.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

static const GUID GUID_PORTS = {
    0x4D36E978, 0xE325, 0x11CE, {0xBF,0xC1,0x08,0x00,0x2B,0xE1,0x03,0x18}
};

#define MTK_VID           "VID_0E8D"
#define MTK_PID_BROM_A    "PID_0003"
#define MTK_PID_BROM_B    "PID_2000"
#define MTK_PID_PRELOADER "PID_2001"

static mtk_port_type_t classify_device_id(const char* device_id) {
    if (!device_id) return MTK_PORT_UNKNOWN;
    if (strstr(device_id, MTK_PID_PRELOADER)) return MTK_PORT_PRELOADER;
    if (strstr(device_id, MTK_PID_BROM_A) || strstr(device_id, MTK_PID_BROM_B)) return MTK_PORT_BROM;
    return MTK_PORT_UNKNOWN;
}

static int extract_com_port(const char* friendly_name, char* port_name, size_t size) {
    const char* p;
    const char* e;
    size_t len;
    if (!friendly_name || !port_name || size == 0) return 0;
    p = strstr(friendly_name, "(COM");
    if (!p) return 0;
    p++;
    e = strchr(p, ')');
    if (!e) return 0;
    len = (size_t)(e - p);
    if (len >= size) len = size - 1;
    memcpy(port_name, p, len);
    port_name[len] = '\0';
    return 1;
}

int mtk_port_detect(mtk_detected_port_t* ports, int max_ports) {
    HDEVINFO dev_info;
    SP_DEVINFO_DATA dev_data;
    int count = 0;
    if (!ports || max_ports <= 0) return 0;

    dev_info = SetupDiGetClassDevsA(&GUID_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) return 0;

    dev_data.cbSize = sizeof(dev_data);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(dev_info, i, &dev_data) && count < max_ports; i++) {
        char device_id[256] = {0};
        char friendly[128] = {0};
        mtk_detected_port_t* p;

        if (!SetupDiGetDeviceInstanceIdA(dev_info, &dev_data, device_id, sizeof(device_id), NULL))
            continue;

        if (strstr(device_id, MTK_VID) == NULL)
            continue;

        SetupDiGetDeviceRegistryPropertyA(dev_info, &dev_data, SPDRP_FRIENDLYNAME,
                                          NULL, (PBYTE)friendly, sizeof(friendly), NULL);

        p = &ports[count];
        memset(p, 0, sizeof(*p));
        snprintf(p->device_id, sizeof(p->device_id), "%s", device_id);
        snprintf(p->description, sizeof(p->description), "%s", friendly);

        if (!extract_com_port(friendly, p->port_name, sizeof(p->port_name))) {
            HKEY key = SetupDiOpenDevRegKey(dev_info, &dev_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key != INVALID_HANDLE_VALUE) {
                char com_name[32];
                DWORD com_size = sizeof(com_name);
                if (RegQueryValueExA(key, "PortName", NULL, NULL, (LPBYTE)com_name, &com_size) == ERROR_SUCCESS) {
                    snprintf(p->port_name, sizeof(p->port_name), "%s", com_name);
                }
                RegCloseKey(key);
            }
        }

        if (!p->port_name[0])
            continue;

        p->type = classify_device_id(device_id);
        count++;
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return count;
}

mtk_error_t mtk_port_detect_first(char* port_name, size_t name_size) {
    mtk_detected_port_t found[16];
    int count = mtk_port_detect(found, 16);
    int i;
    for (i = 0; i < count; ++i) {
        if (found[i].type == MTK_PORT_BROM || found[i].type == MTK_PORT_PRELOADER) {
            snprintf(port_name, name_size, "%s", found[i].port_name);
            return MTK_OK;
        }
    }
    return MTK_E_NOT_FOUND;
}

int mtk_port_detect_has_port(const char* port_name) {
    mtk_detected_port_t found[32];
    int count;
    int i;
    if (!port_name || !*port_name) return 0;
    count = mtk_port_detect(found, 32);
    for (i = 0; i < count; ++i) {
        if (_stricmp(found[i].port_name, port_name) == 0)
            return 1;
    }
    return 0;
}

#endif
