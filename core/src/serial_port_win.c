#include "edl/serial_port.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EDL_PORT_RX_UNREAD_MAX 4096

struct edl_port {
    HANDLE  handle;
    char    name[32];
    bool    is_open;
    int     last_read_timeout_ms;
    int     write_timeout_ms;
    int     transfer_window_bytes;
    int     write_chunk_bytes;
    bool    last_read_bulk_mode;
    uint8_t rx_unread[EDL_PORT_RX_UNREAD_MAX];
    size_t  rx_unread_len;
};

enum {
    EDL_PORT_WINDOW_MIN_BYTES = 64 * 1024,
    EDL_PORT_WINDOW_DEFAULT_BYTES = 128 * 1024,
    EDL_PORT_WINDOW_MAX_BYTES = 512 * 1024
};

static int edl_port_clamp_transfer_window(int payload_bytes)
{
    int window = payload_bytes > 0
        ? (payload_bytes / 4)
        : EDL_PORT_WINDOW_DEFAULT_BYTES;

    if (window < EDL_PORT_WINDOW_MIN_BYTES)
        window = EDL_PORT_WINDOW_MIN_BYTES;
    if (window > EDL_PORT_WINDOW_MAX_BYTES)
        window = EDL_PORT_WINDOW_MAX_BYTES;
    return window;
}

static void edl_port_apply_timeouts(edl_port_t *port, int read_timeout_ms, bool bulk_mode)
{
    COMMTIMEOUTS ct;

    if (!port || port->handle == INVALID_HANDLE_VALUE)
        return;
    if (read_timeout_ms < 1)
        read_timeout_ms = 1;

    if (port->last_read_timeout_ms == read_timeout_ms &&
        port->last_read_bulk_mode == bulk_mode) {
        return;
    }

    memset(&ct, 0, sizeof(ct));
    ct.ReadIntervalTimeout = bulk_mode ? 0u : 1u;
    ct.ReadTotalTimeoutMultiplier = 0;
    ct.ReadTotalTimeoutConstant = (DWORD)read_timeout_ms;
    ct.WriteTotalTimeoutMultiplier = 0;
    ct.WriteTotalTimeoutConstant = (DWORD)(port->write_timeout_ms > 0
        ? port->write_timeout_ms
        : 30000);
    SetCommTimeouts(port->handle, &ct);

    port->last_read_timeout_ms = read_timeout_ms;
    port->last_read_bulk_mode = bulk_mode;
}

static int edl_port_read_core(edl_port_t *port, uint8_t *buf, int len, int timeout_ms, bool bulk_mode)
{
    int total = 0;

    if (!port || !port->is_open || !buf || len <= 0)
        return -1;

    if (port->rx_unread_len > 0) {
        size_t n = port->rx_unread_len < (size_t)len ? port->rx_unread_len : (size_t)len;
        memcpy(buf, port->rx_unread, n);
        memmove(port->rx_unread, port->rx_unread + n, port->rx_unread_len - n);
        port->rx_unread_len -= n;
        total = (int)n;
        if (total >= len)
            return total;
        buf += total;
        len -= total;
    }

    edl_port_apply_timeouts(port, timeout_ms, bulk_mode);

    if (port->transfer_window_bytes > 0 && len > port->transfer_window_bytes)
        len = port->transfer_window_bytes;

    {
        DWORD got = 0;
        if (!ReadFile(port->handle, buf, (DWORD)len, &got, NULL))
            return total > 0 ? total : -1;
        return total + (int)got;
    }
}

static int edl_port_pick_exact_slice_ms(int count, int timeout_ms)
{
    int slice = 250;

    if (count >= 512 * 1024)
        slice = 1000;
    else if (count >= 128 * 1024)
        slice = 500;

    if (timeout_ms > 0 && slice > timeout_ms)
        slice = timeout_ms;
    if (slice < 1)
        slice = 1;
    return slice;
}

static void edl_port_apply_transfer_window(edl_port_t *port, int payload_bytes)
{
    int window = 0;

    if (!port)
        return;

    window = edl_port_clamp_transfer_window(payload_bytes);
    port->transfer_window_bytes = window;
    port->write_chunk_bytes = window;

    if (port->handle != INVALID_HANDLE_VALUE) {
        /* 固定 32MB 队列会让部分 QDLoader 驱动出现大块堆积，保守窗口更稳。 */
        (void)SetupComm(port->handle, (DWORD)window, (DWORD)window);
    }
}

edl_port_t *edl_port_create(void)
{
    edl_port_t *p = (edl_port_t *)calloc(1, sizeof(edl_port_t));
    if (p) {
        p->handle = INVALID_HANDLE_VALUE;
        p->is_open = false;
        p->last_read_timeout_ms = -1;
        p->write_timeout_ms = 30000;
        p->transfer_window_bytes = EDL_PORT_WINDOW_DEFAULT_BYTES;
        p->write_chunk_bytes = EDL_PORT_WINDOW_DEFAULT_BYTES;
        p->last_read_bulk_mode = false;
    }
    return p;
}

void edl_port_destroy(edl_port_t *port)
{
    if (!port) return;
    edl_port_close(port);
    free(port);
}

edl_error_t edl_port_open(edl_port_t *port, const char *name,
                           int baud_rate, int read_timeout_ms, int write_timeout_ms)
{
    if (!port || !name) return EDL_ERR_INVALID_PARAM;

    edl_port_close(port);

    /* Build device path: \\.\COMx */
    char path[64];
    if (name[0] == '\\')
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "\\\\.\\%s", name);

    port->handle = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (port->handle == INVALID_HANDLE_VALUE)
        return EDL_ERR_PORT_OPEN;

    /* Configure DCB */
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(port->handle, &dcb)) {
        CloseHandle(port->handle);
        port->handle = INVALID_HANDLE_VALUE;
        return EDL_ERR_PORT_CONFIG;
    }

    dcb.BaudRate = (DWORD)baud_rate;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(port->handle, &dcb)) {
        CloseHandle(port->handle);
        port->handle = INVALID_HANDLE_VALUE;
        return EDL_ERR_PORT_CONFIG;
    }

    /*
     * SetupComm 设置驱动 RX/TX 队列。大块读写时较大队列可减少内核往返、提高吞吐。
     * 16 MB 对齐 max_payload_size（FH_OPTIMAL_PAYLOAD），可一次性容纳完整载荷 + ACK。
     */
    edl_port_apply_transfer_window(port, EDL_PORT_WINDOW_DEFAULT_BYTES);

    port->write_timeout_ms = write_timeout_ms > 0 ? write_timeout_ms : 30000;
    port->last_read_timeout_ms = -1;
    port->last_read_bulk_mode = false;

    /* 与 SakuraEDL C# 一致：打开时不清 RX。9008 在枚举后常会先发出 Hello，
     * 若 PURGE_RXCLEAR 会丢掉该包，主机只能干等下一次 Hello（表现为“已连接后卡死”）。 */
    PurgeComm(port->handle, PURGE_TXCLEAR);

    snprintf(port->name, sizeof(port->name), "%s", name);
    port->is_open = true;
    edl_port_apply_timeouts(port, read_timeout_ms, false);
    return EDL_OK;
}

void edl_port_close(edl_port_t *port)
{
    if (!port || !port->is_open) return;
    if (port->handle != INVALID_HANDLE_VALUE) {
        PurgeComm(port->handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
        CloseHandle(port->handle);
        port->handle = INVALID_HANDLE_VALUE;
    }
    port->rx_unread_len = 0;
    port->last_read_timeout_ms = -1;
    port->last_read_bulk_mode = false;
    port->write_timeout_ms = 30000;
    port->is_open = false;
}

bool edl_port_is_open(const edl_port_t *port)
{
    return port && port->is_open && port->handle != INVALID_HANDLE_VALUE;
}

int edl_port_write(edl_port_t *port, const uint8_t *data, int len)
{
    if (!port || !port->is_open || !data || len <= 0) return -1;

    int total = 0;
    int zero_write_spins = 0;
    const int write_chunk_bytes = port->write_chunk_bytes > 0
        ? port->write_chunk_bytes
        : EDL_PORT_WINDOW_DEFAULT_BYTES;

    while (total < len) {
        int want = len - total;
        if (want > write_chunk_bytes)
            want = write_chunk_bytes;

        DWORD written = 0;
        if (!WriteFile(port->handle, data + total, (DWORD)want, &written, NULL))
            return total > 0 ? total : -1;

        if (written == 0) {
            if (++zero_write_spins >= 8)
                return total > 0 ? total : -1;
            Sleep(1);
            continue;
        }

        zero_write_spins = 0;
        total += (int)written;
    }

    return total;
}

int edl_port_read(edl_port_t *port, uint8_t *buf, int len, int timeout_ms)
{
    return edl_port_read_core(port, buf, len, timeout_ms, false);
}

void edl_port_set_transfer_window(edl_port_t *port, int payload_bytes)
{
    edl_port_apply_transfer_window(port, payload_bytes);
}

int edl_port_read_exact_ex(edl_port_t *port, uint8_t *buf, int count, int timeout_ms,
                           edl_cancel_cb cancel_cb, void *user_data)
{
    if (!port || !port->is_open || !buf || count <= 0) return -1;

    DWORD start = GetTickCount();
    int total = 0;
    const int slice_cap_ms = edl_port_pick_exact_slice_ms(count, timeout_ms);

    while (total < count) {
        if (cancel_cb && cancel_cb(user_data))
            return EDL_ERR_CANCELLED;

        DWORD elapsed = GetTickCount() - start;
        if ((int)elapsed >= timeout_ms)
            return total > 0 ? total : -1;

        int remaining_time = timeout_ms - (int)elapsed;
        if (remaining_time < 1)
            remaining_time = 1;

        int slice = remaining_time > slice_cap_ms ? slice_cap_ms : remaining_time;
        int n = edl_port_read_core(port, buf + total, count - total, slice, true);
        if (n <= 0) {
            Sleep(0);
            continue;
        }
        total += n;
    }
    return total;
}

int edl_port_read_exact(edl_port_t *port, uint8_t *buf, int count, int timeout_ms)
{
    return edl_port_read_exact_ex(port, buf, count, timeout_ms, NULL, NULL);
}

int edl_port_bytes_available(edl_port_t *port)
{
    if (!port || !port->is_open) return 0;

    COMSTAT stat;
    DWORD errors;
    if (!ClearCommError(port->handle, &errors, &stat))
        return (int)port->rx_unread_len;
    return (int)port->rx_unread_len + (int)stat.cbInQue;
}

void edl_port_purge(edl_port_t *port)
{
    if (!port || !port->is_open) return;
    port->rx_unread_len = 0;
    PurgeComm(port->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

void edl_port_discard_rx(edl_port_t *port)
{
    if (!port || !port->is_open) return;
    port->rx_unread_len = 0;
    PurgeComm(port->handle, PURGE_RXCLEAR);
}

int edl_port_push_rx(edl_port_t *port, const uint8_t *data, int len)
{
    if (!port || !data || len <= 0) return -1;
    if ((size_t)len + port->rx_unread_len > EDL_PORT_RX_UNREAD_MAX)
        return -1;
    memmove(port->rx_unread + len, port->rx_unread, port->rx_unread_len);
    memcpy(port->rx_unread, data, (size_t)len);
    port->rx_unread_len += (size_t)len;
    return 0;
}

void edl_port_flush(edl_port_t *port)
{
    if (!port || !port->is_open) return;
    FlushFileBuffers(port->handle);
}

const char *edl_port_name(const edl_port_t *port)
{
    return port ? port->name : "";
}

#endif /* _WIN32 */
