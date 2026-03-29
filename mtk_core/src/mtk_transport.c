#include "mtk/mtk_transport.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MTK_PORT_RX_UNREAD_MAX 512

struct mtk_transport {
    mtk_callbacks_t cb;
    HANDLE handle;
    int is_open;
    char port_name[64];
    uint8_t rx_unread[MTK_PORT_RX_UNREAD_MAX];
    size_t rx_unread_len;
};

static void mtk_log(mtk_transport_t* t, mtk_log_level_t level, const char* msg) {
    if (t && t->cb.log) {
        t->cb.log(t->cb.user, level, msg);
    }
}

mtk_transport_t* mtk_transport_create(const mtk_callbacks_t* cb) {
    mtk_transport_t* t = (mtk_transport_t*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (cb) t->cb = *cb;
    t->handle = INVALID_HANDLE_VALUE;
    return t;
}

void mtk_transport_destroy(mtk_transport_t* transport) {
    if (!transport) return;
    mtk_transport_close(transport);
    free(transport);
}

mtk_error_t mtk_transport_open(mtk_transport_t* transport, const char* port_name) {
    DCB dcb;
    COMMTIMEOUTS timeouts;
    char path[64];
    if (!transport || !port_name || !*port_name) return MTK_E_INVALID_ARG;

    mtk_transport_close(transport);

    if (port_name[0] == '\\')
        lstrcpynA(path, port_name, (int)sizeof(path));
    else
        wsprintfA(path, "\\\\.\\%s", port_name);

    transport->handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (transport->handle == INVALID_HANDLE_VALUE)
        return MTK_E_IO;

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(transport->handle, &dcb)) {
        CloseHandle(transport->handle);
        transport->handle = INVALID_HANDLE_VALUE;
        return MTK_E_IO;
    }

    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(transport->handle, &dcb)) {
        CloseHandle(transport->handle);
        transport->handle = INVALID_HANDLE_VALUE;
        return MTK_E_IO;
    }

    SetupComm(transport->handle, 4 * 1024 * 1024, 4 * 1024 * 1024);

    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 1;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(transport->handle, &timeouts);

    PurgeComm(transport->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    lstrcpynA(transport->port_name, port_name, (int)sizeof(transport->port_name));
    transport->is_open = 1;
    mtk_log(transport, MTK_LOG_INFO, "mtk transport opened");
    return MTK_OK;
}

void mtk_transport_close(mtk_transport_t* transport) {
    if (!transport || !transport->is_open) return;
    if (transport->handle != INVALID_HANDLE_VALUE) {
        PurgeComm(transport->handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
        CloseHandle(transport->handle);
        transport->handle = INVALID_HANDLE_VALUE;
    }
    transport->rx_unread_len = 0;
    transport->port_name[0] = '\0';
    transport->is_open = 0;
}

int mtk_transport_is_open(const mtk_transport_t* transport) {
    return transport && transport->is_open && transport->handle != INVALID_HANDLE_VALUE;
}

const char* mtk_transport_port_name(const mtk_transport_t* transport) {
    return transport ? transport->port_name : NULL;
}

int mtk_transport_write(mtk_transport_t* transport, const uint8_t* data, int len) {
    DWORD written = 0;
    if (!transport || !transport->is_open || !data || len <= 0) return -1;
    if (!WriteFile(transport->handle, data, (DWORD)len, &written, NULL))
        return -1;
    return (int)written;
}

int mtk_transport_read(mtk_transport_t* transport, uint8_t* buf, int len, int timeout_ms) {
    COMMTIMEOUTS ct;
    DWORD got = 0;
    int total = 0;
    if (!transport || !transport->is_open || !buf || len <= 0) return -1;

    if (transport->rx_unread_len > 0) {
        size_t n = transport->rx_unread_len < (size_t)len ? transport->rx_unread_len : (size_t)len;
        memcpy(buf, transport->rx_unread, n);
        memmove(transport->rx_unread, transport->rx_unread + n, transport->rx_unread_len - n);
        transport->rx_unread_len -= n;
        total = (int)n;
        if (total >= len) return total;
        buf += total;
        len -= total;
    }

    memset(&ct, 0, sizeof(ct));
    ct.ReadIntervalTimeout = 1;
    ct.ReadTotalTimeoutMultiplier = 0;
    ct.ReadTotalTimeoutConstant = (DWORD)(timeout_ms > 0 ? timeout_ms : 1);
    SetCommTimeouts(transport->handle, &ct);

    if (!ReadFile(transport->handle, buf, (DWORD)len, &got, NULL))
        return total > 0 ? total : -1;
    return total + (int)got;
}

int mtk_transport_read_exact(mtk_transport_t* transport, uint8_t* buf, int count, int timeout_ms) {
    DWORD start;
    int total = 0;
    if (!transport || !buf || count <= 0) return -1;
    start = GetTickCount();
    while (total < count) {
        DWORD elapsed = GetTickCount() - start;
        int remaining;
        int slice;
        int n;
        if ((int)elapsed >= timeout_ms) return total > 0 ? total : -1;
        remaining = timeout_ms - (int)elapsed;
        if (remaining < 1) remaining = 1;
        slice = remaining > 1000 ? 1000 : remaining;
        n = mtk_transport_read(transport, buf + total, count - total, slice);
        if (n <= 0) {
            Sleep(0);
            continue;
        }
        total += n;
    }
    return total;
}

int mtk_transport_bytes_available(mtk_transport_t* transport) {
    COMSTAT stat;
    DWORD errors;
    if (!transport || !transport->is_open) return 0;
    if (!ClearCommError(transport->handle, &errors, &stat))
        return (int)transport->rx_unread_len;
    return (int)transport->rx_unread_len + (int)stat.cbInQue;
}

void mtk_transport_purge(mtk_transport_t* transport) {
    if (!transport || !transport->is_open) return;
    transport->rx_unread_len = 0;
    PurgeComm(transport->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

#else

struct mtk_transport {
    mtk_callbacks_t cb;
};

mtk_transport_t* mtk_transport_create(const mtk_callbacks_t* cb) {
    mtk_transport_t* t = (mtk_transport_t*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (cb) t->cb = *cb;
    return t;
}

void mtk_transport_destroy(mtk_transport_t* transport) { free(transport); }
mtk_error_t mtk_transport_open(mtk_transport_t* transport, const char* port_name) { (void)transport; (void)port_name; return MTK_E_UNSUPPORTED; }
void mtk_transport_close(mtk_transport_t* transport) { (void)transport; }
int mtk_transport_is_open(const mtk_transport_t* transport) { (void)transport; return 0; }
const char* mtk_transport_port_name(const mtk_transport_t* transport) { (void)transport; return NULL; }
int mtk_transport_write(mtk_transport_t* transport, const uint8_t* data, int len) { (void)transport; (void)data; (void)len; return -1; }
int mtk_transport_read(mtk_transport_t* transport, uint8_t* buf, int len, int timeout_ms) { (void)transport; (void)buf; (void)len; (void)timeout_ms; return -1; }
int mtk_transport_read_exact(mtk_transport_t* transport, uint8_t* buf, int count, int timeout_ms) { (void)transport; (void)buf; (void)count; (void)timeout_ms; return -1; }
int mtk_transport_bytes_available(mtk_transport_t* transport) { (void)transport; return 0; }
void mtk_transport_purge(mtk_transport_t* transport) { (void)transport; }

#endif
