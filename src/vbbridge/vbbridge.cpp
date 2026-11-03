/* vbbridge.cpp -- transport user-mode untuk driver ndislwf.sys.
 *
 * Satu handle global; tidak ekspektasi multithread pada API ini.
 * Kode status: 0 = sukses, selain itu last error Win32.
 */
#include "vbbridge.h"
#include <windows.h>

static HANDLE g_h = NULL;

VB_API int vb_open(void)
{
    if (g_h != NULL)
    {
        return 0;
    }
    HANDLE h = CreateFileW(L"\\\\.\\ndislwf",
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        return (int)GetLastError();
    }
    g_h = h;
    return 0;
}

VB_API void vb_close(void)
{
    if (g_h != NULL)
    {
        CloseHandle(g_h);
        g_h = NULL;
    }
}

static int vb_ioctl(DWORD code,
                    void* in_buf, DWORD in_len,
                    void* out_buf, DWORD out_len,
                    DWORD* ret_len)
{
    if (g_h == NULL)
    {
        int e = vb_open();
        if (e != 0)
        {
            return e;
        }
    }
    DWORD ret = 0;
    if (!DeviceIoControl(g_h, code, in_buf, in_len, out_buf, out_len, &ret, NULL))
    {
        return (int)GetLastError();
    }
    if (ret_len != NULL)
    {
        *ret_len = ret;
    }
    return 0;
}

VB_API int vb_version(uint32_t* magic, uint32_t* ver)
{
    if (magic == NULL || ver == NULL) { return ERROR_INVALID_PARAMETER; }
    struct { uint32_t magic; uint32_t ver; } v = { 0, 0 };
    int e = vb_ioctl(VB_IOC_VERSION, NULL, 0, &v, (DWORD)sizeof(v), NULL);
    if (e == 0)
    {
        *magic = v.magic;
        *ver = v.ver;
    }
    return e;
}

VB_API int vb_enum(vb_adapter_t* out, int max, int* count)
{
    if (out == NULL || count == NULL || max <= 0) { return ERROR_INVALID_PARAMETER; }
    if (max > VB_MAX_ADAPTERS) { max = VB_MAX_ADAPTERS; }

    /* Enum mengambil buffer adapter; size tetap 544 byte per entri. */
    static_assert(sizeof(vb_adapter_t) == 544, "layout vb_adapter harus 544");
    static_assert(sizeof(vb_stats_t) == 56, "layout vb_stats harus 56");
    static_assert(sizeof(vb_ring_entry_t) == 1524, "layout vb_ring_entry harus 1524");
    static_assert(sizeof(vb_inject_t) == 1524, "layout vb_inject harus 1524");
    static_assert(sizeof(vb_mode) == 8, "layout vb_mode harus 8");

    vb_adapter_t buf[VB_MAX_ADAPTERS];
    memset(buf, 0, sizeof(buf));
    DWORD ret = 0;
    int e = vb_ioctl(VB_IOC_ENUM_ADPT, NULL, 0, buf, (DWORD)sizeof(buf), &ret);
    if (e != 0)
    {
        return e;
    }
    int n = (int)(ret / sizeof(vb_adapter_t));
    if (n < 0) { n = 0; }
    if (n > max) { n = max; }
    memcpy(out, buf, (size_t)n * sizeof(vb_adapter_t));
    *count = n;
    return 0;
}

VB_API int vb_set_mode(uint32_t adapter_index, uint32_t flags)
{
    vb_mode_t m;
    m.adapter_index = adapter_index;
    m.flags = flags;
    return vb_ioctl(VB_IOC_SET_MODE, &m, (DWORD)sizeof(m), NULL, 0, NULL);
}

VB_API int vb_stats(vb_stats_t* out)
{
    if (out == NULL) { return ERROR_INVALID_PARAMETER; }
    memset(out, 0, sizeof(*out));
    return vb_ioctl(VB_IOC_STATS, NULL, 0, out, (DWORD)sizeof(*out), NULL);
}

VB_API int vb_read_ring(vb_ring_entry_t* out, int max, int* count)
{
    if (out == NULL || count == NULL || max <= 0) { return ERROR_INVALID_PARAMETER; }

    /* OUT: [u32 count][entry...] -- total <= 4 + max*1524 */
    uint32_t total = 4 + (uint32_t)max * sizeof(vb_ring_entry_t);
    uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (buf == NULL)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    DWORD ret = 0;
    int e = vb_ioctl(VB_IOC_READ_RING, NULL, 0, buf, total, &ret);
    if (e != 0)
    {
        HeapFree(GetProcessHeap(), 0, buf);
        return e;
    }
    if (ret < 4)
    {
        HeapFree(GetProcessHeap(), 0, buf);
        *count = 0;
        return 0;
    }
    uint32_t n = 0;
    memcpy(&n, buf, 4);
    if ((uint32_t)max < n)
    {
        HeapFree(GetProcessHeap(), 0, buf);
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (n > 0)
    {
        memcpy(out, buf + 4, (size_t)n * sizeof(vb_ring_entry_t));
    }
    *count = (int)n;
    HeapFree(GetProcessHeap(), 0, buf);
    return 0;
}

VB_API int vb_drain_ring(int* tx, int* rx, uint64_t* bytes)
{
    if (tx == NULL || rx == NULL || bytes == NULL) { return ERROR_INVALID_PARAMETER; }
    *tx = 0;
    *rx = 0;
    *bytes = 0;

    vb_ring_entry_t buf[32];
    int guard = 0;
    while (guard++ < 96)
    {
        int n = 0;
        int e = vb_read_ring(buf, 32, &n);
        if (e != 0) { return e; }
        if (n == 0) { break; }
        for (int i = 0; i < n; i++)
        {
            if ((buf[i].flags & VB_RING_TX) != 0) { (*tx)++; } else { (*rx)++; }
            *bytes += buf[i].len;
        }
    }
    return 0;
}

VB_API int vb_inject(const void* data, uint32_t len)
{
    if (data == NULL || len == 0 || len > VB_FRAME_MAX) { return ERROR_INVALID_PARAMETER; }
    vb_inject_t in;
    memset(&in, 0, sizeof(in));
    in.len = len;
    memcpy(in.data, data, len);
    return vb_ioctl(VB_IOC_INJECT, &in, (DWORD)sizeof(in), NULL, 0, NULL);
}

VB_API int vb_clear(void)
{
    return vb_ioctl(VB_IOC_CLEAR, NULL, 0, NULL, 0, NULL);
}
