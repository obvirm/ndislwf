/* nlbridge.cpp -- transport user-mode untuk driver ndislwf.sys.
 *
 * Satu handle global; tidak ekspektasi multithread pada API ini.
 * Kode status: 0 = sukses, selain itu last error Win32.
 */
#include "nlbridge.h"
#include <windows.h>

static HANDLE g_h = NULL;

NL_API int nl_open(void)
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

NL_API void nl_close(void)
{
    if (g_h != NULL)
    {
        CloseHandle(g_h);
        g_h = NULL;
    }
}

static int nl_ioctl(DWORD code,
                    void* in_buf, DWORD in_len,
                    void* out_buf, DWORD out_len,
                    DWORD* ret_len)
{
    if (g_h == NULL)
    {
        int e = nl_open();
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

NL_API int nl_version(uint32_t* magic, uint32_t* ver)
{
    if (magic == NULL || ver == NULL) { return ERROR_INVALID_PARAMETER; }
    struct { uint32_t magic; uint32_t ver; } v = { 0, 0 };
    int e = nl_ioctl(NL_IOC_VERSION, NULL, 0, &v, (DWORD)sizeof(v), NULL);
    if (e == 0)
    {
        *magic = v.magic;
        *ver = v.ver;
    }
    return e;
}

NL_API int nl_enum(nl_adapter_t* out, int max, int* count)
{
    if (out == NULL || count == NULL || max <= 0) { return ERROR_INVALID_PARAMETER; }
    if (max > NL_MAX_ADAPTERS) { max = NL_MAX_ADAPTERS; }

    /* Enum mengambil buffer adapter; size tetap 544 byte per entri. */
    static_assert(sizeof(nl_adapter_t) == 544, "layout nl_adapter harus 544");
    static_assert(sizeof(nl_stats_t) == 56, "layout nl_stats harus 56");
    static_assert(sizeof(nl_ring_entry_t) == 1524, "layout nl_ring_entry harus 1524");
    static_assert(sizeof(nl_inject_t) == 1524, "layout nl_inject harus 1524");
    static_assert(sizeof(nl_mode) == 8, "layout nl_mode harus 8");

    nl_adapter_t buf[NL_MAX_ADAPTERS];
    memset(buf, 0, sizeof(buf));
    DWORD ret = 0;
    int e = nl_ioctl(NL_IOC_ENUM_ADPT, NULL, 0, buf, (DWORD)sizeof(buf), &ret);
    if (e != 0)
    {
        return e;
    }
    int n = (int)(ret / sizeof(nl_adapter_t));
    if (n < 0) { n = 0; }
    if (n > max) { n = max; }
    memcpy(out, buf, (size_t)n * sizeof(nl_adapter_t));
    *count = n;
    return 0;
}

NL_API int nl_set_mode(uint32_t adapter_index, uint32_t flags)
{
    nl_mode_t m;
    m.adapter_index = adapter_index;
    m.flags = flags;
    return nl_ioctl(NL_IOC_SET_MODE, &m, (DWORD)sizeof(m), NULL, 0, NULL);
}

NL_API int nl_stats(const uint32_t* adapter, nl_stats_t* out)
{
    if (out == NULL) { return ERROR_INVALID_PARAMETER; }
    memset(out, 0, sizeof(*out));
    if (adapter == NULL)
    {
        return nl_ioctl(NL_IOC_STATS, NULL, 0, out, (DWORD)sizeof(*out), NULL);
    }
    return nl_ioctl(NL_IOC_STATS, (void*)adapter, (DWORD)sizeof(*adapter), out, (DWORD)sizeof(*out), NULL);
}

NL_API int nl_read_ring(const uint32_t* adapter, nl_ring_entry_t* out, int max, int* count)
{
    if (out == NULL || count == NULL || max <= 0) { return ERROR_INVALID_PARAMETER; }

    /* OUT: [u32 count][entry...] -- total <= 4 + max*1524 */
    uint32_t total = 4 + (uint32_t)max * sizeof(nl_ring_entry_t);
    uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (buf == NULL)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    DWORD ret = 0;
    int e;
    if (adapter == NULL)
    {
        e = nl_ioctl(NL_IOC_READ_RING, NULL, 0, buf, total, &ret);
    }
    else
    {
        e = nl_ioctl(NL_IOC_READ_RING, (void*)adapter, (DWORD)sizeof(*adapter), buf, total, &ret);
    }
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
        memcpy(out, buf + 4, (size_t)n * sizeof(nl_ring_entry_t));
    }
    *count = (int)n;
    HeapFree(GetProcessHeap(), 0, buf);
    return 0;
}

NL_API int nl_drain_ring(int* tx, int* rx, uint64_t* bytes)
{
    if (tx == NULL || rx == NULL || bytes == NULL) { return ERROR_INVALID_PARAMETER; }
    *tx = 0;
    *rx = 0;
    *bytes = 0;

    nl_ring_entry_t buf[32];
    int guard = 0;
    while (guard++ < 96)
    {
        int n = 0;
        int e = nl_read_ring(NULL, buf, 32, &n);
        if (e != 0) { return e; }
        if (n == 0) { break; }
        for (int i = 0; i < n; i++)
        {
            if ((buf[i].flags & NL_RING_TX) != 0) { (*tx)++; } else { (*rx)++; }
            *bytes += buf[i].len;
        }
    }
    return 0;
}

NL_API int nl_inject(const void* data, uint32_t len)
{
    if (data == NULL || len == 0 || len > NL_FRAME_MAX) { return ERROR_INVALID_PARAMETER; }
    nl_inject_t in;
    memset(&in, 0, sizeof(in));
    in.len = len;
    memcpy(in.data, data, len);
    return nl_ioctl(NL_IOC_INJECT, &in, (DWORD)sizeof(in), NULL, 0, NULL);
}

NL_API int nl_clear(void)
{
    return nl_ioctl(NL_IOC_CLEAR, NULL, 0, NULL, 0, NULL);
}

/* --- API v2 --------------------------------------------------------------- */

static_assert(sizeof(nl_policy_t) == 456, "layout nl_policy harus 456");
static_assert(sizeof(nl_shm_hdr_t) == 96, "layout nl_shm_hdr harus 96");
static_assert(NL_SHM_BYTES == 128 + 2u * 1024u * 1524u, "NL_SHM_BYTES harus 3117184");
static_assert(sizeof(nl_policy_rule_ex_t) == 44, "layout nl_policy_rule_ex harus 44");
static_assert(sizeof(nl_policy_ex_t) == 712, "layout nl_policy_ex harus 712");

NL_API int nl_policy_set(const nl_policy_t* policy)
{
    if (policy == NULL) { return ERROR_INVALID_PARAMETER; }
    if (policy->count > NL_POLICY_MAX_RULES) { return ERROR_INVALID_PARAMETER; }
    nl_policy_t p;
    memset(&p, 0, sizeof(p));
    memcpy(&p, policy, sizeof(p));
    return nl_ioctl(NL_IOC_POLICY, &p, (DWORD)sizeof(p), NULL, 0, NULL);
}

NL_API int nl_policy_ex_set(const nl_policy_ex_t* ex)
{
    if (ex == NULL) { return ERROR_INVALID_PARAMETER; }
    if (ex->count > NL_POLICY_MAX_RULES) { return ERROR_INVALID_PARAMETER; }
    nl_policy_ex_t p;
    memset(&p, 0, sizeof(p));
    memcpy(&p, ex, sizeof(p));
    return nl_ioctl(NL_IOC_POLICY_EX, &p, (DWORD)sizeof(p), NULL, 0, NULL);
}

NL_API int nl_delay_stat(nl_delay_stat_t* out)
{
    static_assert(sizeof(nl_delay_stat_t) == 16, "layout nl_delay_stat harus 16");
    if (out == NULL) { return ERROR_INVALID_PARAMETER; }
    memset(out, 0, sizeof(*out));
    return nl_ioctl(NL_IOC_DELAYSTAT, NULL, 0, out, (DWORD)sizeof(*out), NULL);
}

NL_API int nl_vm_set(const nl_vm_prog_t* prog)
{
    static_assert(sizeof(nl_vm_insn_t) == 8, "layout nl_vm_insn harus 8");
    static_assert(sizeof(nl_vm_prog_t) == 520, "layout nl_vm_prog harus 520");
    if (prog == NULL) { return ERROR_INVALID_PARAMETER; }
    if (prog->count > NL_VM_MAX_INSNS) { return ERROR_INVALID_PARAMETER; }
    nl_vm_prog_t p;
    memset(&p, 0, sizeof(p));
    memcpy(&p, prog, sizeof(p));
    return nl_ioctl(NL_IOC_PROGRAM, &p, (DWORD)sizeof(p), NULL, 0, NULL);
}

NL_API int nl_inject_rx(const void* data, uint32_t len)
{
    if (data == NULL || len == 0 || len > NL_FRAME_MAX) { return ERROR_INVALID_PARAMETER; }
    nl_inject_t in;
    memset(&in, 0, sizeof(in));
    in.len = len;
    memcpy(in.data, data, len);
    return nl_ioctl(NL_IOC_INJECT_RX, &in, (DWORD)sizeof(in), NULL, 0, NULL);
}

/* --- API v6 (M5): MIRROR + REDIRECT ---------------------------------------- */

static_assert(sizeof(nl_mirror_cfg_t) == 12, "layout nl_mirror_cfg harus 12");
static_assert(sizeof(nl_inject_to_t) == 1524, "layout nl_inject_to harus 1524 (pad bawa struct ke kelipatan 4)");

NL_API int nl_mirror_set(const nl_mirror_cfg_t* cfg)
{
    if (cfg == NULL) { return ERROR_INVALID_PARAMETER; }
    nl_mirror_cfg_t p;
    memset(&p, 0, sizeof(p));
    memcpy(&p, cfg, sizeof(p));
    return nl_ioctl(NL_IOC_MIRROR, &p, (DWORD)sizeof(p), NULL, 0, NULL);
}

NL_API int nl_inject_to(uint32_t dst_index, const void* data, uint32_t len)
{
    if (data == NULL || len == 0 || len > NL_FRAME_MAX) { return ERROR_INVALID_PARAMETER; }
    nl_inject_to_t in;
    memset(&in, 0, sizeof(in));
    in.dst_index = dst_index;
    in.len = len;
    memcpy(in.data, data, len);
    return nl_ioctl(NL_IOC_INJECT_TO, &in, (DWORD)sizeof(in), NULL, 0, NULL);
}

NL_API int nl_rule_hits(nl_rule_hits_t* out)
{
    static_assert(sizeof(nl_rule_hits_t) == 144, "layout nl_rule_hits harus 144");
    if (out == NULL) { return ERROR_INVALID_PARAMETER; }
    memset(out, 0, sizeof(*out));
    return nl_ioctl(NL_IOC_RULEHITS, NULL, 0, out, (DWORD)sizeof(*out), NULL);
}

NL_API int nl_ioctl_raw(uint32_t code,
                        void* in_buf, uint32_t in_len,
                        void* out_buf, uint32_t out_len,
                        uint32_t* out_ret_len)
{
    DWORD ret = 0;
    int e = nl_ioctl((DWORD)code, in_buf, (DWORD)in_len,
                     out_buf, (DWORD)out_len, &ret);
    if (out_ret_len != NULL)
    {
        *out_ret_len = (uint32_t)ret;
    }
    return e;
}

NL_API int nl_debug_state(nl_debug_state_t* out)
{
    DWORD ret = 0;
    if (out == NULL) { return ERROR_INVALID_PARAMETER; }
    memset(out, 0, sizeof(*out));
    return nl_ioctl(NL_IOC_DEBUG, NULL, 0, out, (DWORD)sizeof(*out), &ret);
}

NL_API void* nl_shm_alloc(void)
{
    void* p = VirtualAlloc(NULL, (SIZE_T)NL_SHM_BYTES,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p;
}

NL_API int nl_shm_map(uint32_t adapter_index, void* virtual_address, uint32_t len)
{
    if (virtual_address == NULL || len < NL_SHM_BYTES ||
        ((uintptr_t)virtual_address & 0xFFFu) != 0)
    {
        return ERROR_INVALID_PARAMETER;
    }

    nl_map_ring_t m;
    m.adapter_index = adapter_index;
    m.len = len;
    m.user_va = virtual_address;

    return nl_ioctl(NL_IOC_MAP_RING, &m, (DWORD)sizeof(m), NULL, 0, NULL);
}

NL_API int nl_shm_unmap(uint32_t adapter_index)
{
    return nl_ioctl(NL_IOC_UNMAP_RING,
                    &adapter_index, (DWORD)sizeof(adapter_index),
                    NULL, 0, NULL);
}

NL_API int nl_shm_event(uint32_t adapter_index, uint64_t* handle)
{
    if (handle == NULL) { return ERROR_INVALID_PARAMETER; }

    DWORD ret = 0;
    nl_event_t out;
    memset(&out, 0, sizeof(out));

    /* nl_ioctl lancar nl_open jika belum: handle event kernel dibuat di
     * konteks proses caller (IOCTL PASSIVE di sini). */
    int e = nl_ioctl(NL_IOC_GET_EVENT,
                 &adapter_index, (DWORD)sizeof(adapter_index),
                 &out, (DWORD)sizeof(out), &ret);
    if (e != 0)
    {
        return e;
    }
    if (ret < (DWORD)sizeof(out))
    {
        return ERROR_READ_FAULT;
    }
    *handle = out.handle;
    return 0;
}
