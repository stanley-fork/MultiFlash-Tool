#include "edl/port_detect.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

/* Qualcomm USB VIDs/PIDs */
#define QC_VID      "VID_05C6"
#define QC_PID_9008 "PID_9008"
#define QC_PID_9006 "PID_9006"
#define QC_PID_9091 "PID_9091"

/* GUID for ports device class */
static const GUID GUID_PORTS = {
    0x4D36E978, 0xE325, 0x11CE, {0xBF,0xC1,0x08,0x00,0x2B,0xE1,0x03,0x18}
};

static edl_port_type_t classify_device_id(const char *device_id)
{
    if (strstr(device_id, QC_PID_9008)) return EDL_PORT_EDL_9008;
    if (strstr(device_id, QC_PID_9006)) return EDL_PORT_DLOAD_9006;
    if (strstr(device_id, QC_PID_9091)) return EDL_PORT_DIAG_9091;
    return EDL_PORT_OTHER;
}

static bool extract_com_port(const char *friendly_name, char *port_name, size_t size)
{
    const char *p = strstr(friendly_name, "(COM");
    if (!p) return false;
    p++;
    const char *e = strchr(p, ')');
    if (!e) return false;
    size_t len = (size_t)(e - p);
    if (len >= size) len = size - 1;
    memcpy(port_name, p, len);
    port_name[len] = '\0';
    return true;
}

int edl_port_detect(edl_detected_port_t *ports, int max_ports)
{
    if (!ports || max_ports <= 0) return 0;

    HDEVINFO dev_info = SetupDiGetClassDevsA(
        &GUID_PORTS, NULL, NULL,
        DIGCF_PRESENT
    );
    if (dev_info == INVALID_HANDLE_VALUE) return 0;

    SP_DEVINFO_DATA dev_data;
    dev_data.cbSize = sizeof(dev_data);
    int count = 0;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(dev_info, i, &dev_data) && count < max_ports; i++) {
        char device_id[256] = {0};
        char friendly[128] = {0};

        /* Get hardware ID */
        if (!SetupDiGetDeviceInstanceIdA(dev_info, &dev_data, device_id, sizeof(device_id), NULL))
            continue;

        /* Filter for Qualcomm VID */
        if (strstr(device_id, QC_VID) == NULL)
            continue;

        /* Get friendly name */
        SetupDiGetDeviceRegistryPropertyA(
            dev_info, &dev_data, SPDRP_FRIENDLYNAME,
            NULL, (PBYTE)friendly, sizeof(friendly), NULL);

        edl_detected_port_t *p = &ports[count];
        memset(p, 0, sizeof(*p));

        snprintf(p->device_id, sizeof(p->device_id), "%s", device_id);
        snprintf(p->description, sizeof(p->description), "%s", friendly);

        if (!extract_com_port(friendly, p->port_name, sizeof(p->port_name))) {
            /* Try registry key for port name */
            HKEY key = SetupDiOpenDevRegKey(dev_info, &dev_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key != INVALID_HANDLE_VALUE) {
                char com_name[16];
                DWORD com_size = sizeof(com_name);
                if (RegQueryValueExA(key, "PortName", NULL, NULL, (LPBYTE)com_name, &com_size) == ERROR_SUCCESS) {
                    snprintf(p->port_name, sizeof(p->port_name), "%s", com_name);
                }
                RegCloseKey(key);
            }
        }

        if (p->port_name[0] == '\0') continue;

        p->type = classify_device_id(device_id);
        count++;
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return count;
}

edl_error_t edl_port_detect_first_edl(char *port_name, size_t name_size)
{
    edl_detected_port_t found[16];
    int count = edl_port_detect(found, 16);

    /* Prefer 9008 */
    for (int i = 0; i < count; i++) {
        if (found[i].type == EDL_PORT_EDL_9008) {
            snprintf(port_name, name_size, "%s", found[i].port_name);
            return EDL_OK;
        }
    }

    /* Fallback to 9006 */
    for (int i = 0; i < count; i++) {
        if (found[i].type == EDL_PORT_DLOAD_9006) {
            snprintf(port_name, name_size, "%s", found[i].port_name);
            return EDL_OK;
        }
    }

    return EDL_ERR_PORT_NOT_FOUND;
}

bool edl_port_detect_has_edl_port(const char *port_name)
{
    if (!port_name || !port_name[0]) return false;

    edl_detected_port_t found[32];
    int count = edl_port_detect(found, 32);
    for (int i = 0; i < count; i++) {
        if (_stricmp(found[i].port_name, port_name) != 0)
            continue;
        return (found[i].type == EDL_PORT_EDL_9008 || found[i].type == EDL_PORT_DLOAD_9006);
    }
    return false;
}

#endif /* _WIN32 */
