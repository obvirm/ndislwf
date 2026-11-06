/*
 * ndisapi.dll shim — kompatibilitas biner tool lama legacy filter.
 *
 * Tool lama memanggil ndisapi.dll; shim ini membeku API yang sama tetapi
 * memetakan semua panggilan ke nlbridge ABI (driver ndislwf):
 *   GetTcpipBoundAdaptersInfo  -> nl_enum
 *   SetAdapterMode             -> nl_set_mode (TUNNEL diperlakukan LISTEN)
 *   ReadPacket / ReadPackets   -> nl_read_ring
 *   SendPacketToAdapter        -> nl_inject_to (target handle)
 *   SendPacketToMstcp          -> nl_inject_rx
 *
 * Keterbatasan vs legacy filter asli: mode TUNNEL tidak menahan paket di
 * jaringan (driver kami tidak memblokir aliran asli di mode capture) — asli
 * tetap berjalan; shim hanya memberikan capture/inject dua arah.
 *
 * Adapter handle = (nl_index + 1) — nilai tak-transparan nghiệp.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "nlbridge.h"

#define NL_API_SHIM __declspec(dllexport)

#define ADAPTER_NAME_SIZE   256
#define ADAPTER_LIST_SIZE   32
#define ETHER_ADDR_LENGTH   6
#define MAX_ETHER_FRAME     1514

#define MSTCP_FLAG_SENT_TUNNEL     0x00000001
#define MSTCP_FLAG_RECV_TUNNEL     0x00000002
#define MSTCP_FLAG_SENT_LISTEN     0x00000004
#define MSTCP_FLAG_RECV_LISTEN     0x00000008
#define MSTCP_FLAG_FILTER_DIRECT   0x00000010
#define MSTCP_FLAG_LOOPBACK_FILTER 0x00000020
#define MSTCP_FLAG_LOOPBACK_BLOCK  0x00000040

#define PACKET_FLAG_ON_SEND      0x00000001
#define PACKET_FLAG_ON_RECEIVE   0x00000002

#pragma pack(push, 1)

typedef struct _INTERNAL_LIST_ENTRY {
    void* Flink;
    void* Blink;
} INTERNAL_LIST_ENTRY;

typedef union _IB_HEADER_UNION {
    void*               AdapterHandle;
    INTERNAL_LIST_ENTRY ListEntry;
} IB_HEADER_UNION;

typedef struct _INTERMEDIATE_BUFFER {
    IB_HEADER_UNION Header;
    unsigned long   DeviceFlags;
    unsigned long   Length;
    unsigned long   Flags;
    unsigned long   Vlan8021Q;
    unsigned long   FilterID;
    unsigned long   Reserved[4];
    unsigned char   Buffer[MAX_ETHER_FRAME];
} INTERMEDIATE_BUFFER, *PINTERMEDIATE_BUFFER;

typedef struct _nlwf_ETH_Packet {
    PINTERMEDIATE_BUFFER Buffer;
} nlwf_ETH_Packet, *Pnlwf_ETH_Packet;

typedef struct _ETH_REQUEST {
    HANDLE           hAdapterHandle;
    nlwf_ETH_Packet EthPacket;
} ETH_REQUEST, *PETH_REQUEST;

typedef struct _ETH_M_REQUEST {
    HANDLE           hAdapterHandle;
    unsigned long    dwPacketsNumber;
    unsigned long    dwPacketsSuccess;
    nlwf_ETH_Packet EthPacket[1];
} ETH_M_REQUEST, *PETH_M_REQUEST;

typedef struct _TCP_AdapterList {
    unsigned long AdapterCount;
    char          NameList[ADAPTER_LIST_SIZE][ADAPTER_NAME_SIZE];
    HANDLE        Handles[ADAPTER_LIST_SIZE];
    unsigned long Mediums[ADAPTER_LIST_SIZE];
    unsigned char CurrentAddresses[ADAPTER_LIST_SIZE][ETHER_ADDR_LENGTH];
    unsigned short Mtus[ADAPTER_LIST_SIZE];
} TCP_AdapterList, *PTCP_AdapterList;

typedef struct _ADAPTER_MODE {
    HANDLE        hAdapterHandle;
    unsigned long dwFlags;
} ADAPTER_MODE, *PADAPTER_MODE;

#pragma pack(pop)

static_assert(sizeof(INTERMEDIATE_BUFFER) == 1566, "layout INTERMEDIATE_BUFFER harus 1566 (union hdr 16 + 20 + reserved 16 + data 1514)");
static_assert(sizeof(ETH_REQUEST) == 16, "layout ETH_REQUEST harus 16");
static_assert(sizeof(ETH_M_REQUEST) == 24, "layout ETH_M_REQUEST harus 24");
static_assert(sizeof(TCP_AdapterList) == 8836, "layout TCP_AdapterList harus 8836");
static_assert(sizeof(ADAPTER_MODE) == 12, "layout ADAPTER_MODE harus 12");

/* adapter handle <-> nl index */
static unsigned nlIdxFromHandle(HANDLE h)
{
    uintptr_t u = (uintptr_t)h;
    if (u == 0 || u > ADAPTER_LIST_SIZE) { return 0xFFFFFFFFu; }
    return (unsigned)(u - 1);
}

static HANDLE handleFromIdx(unsigned idx) { return (HANDLE)(uintptr_t)(idx + 1); }

static LONG gOpenRef = 0;

extern "C" {

/* --- driver lifecycle ------------------------------------------------------ */

NL_API_SHIM HANDLE __stdcall OpenFilterDriver(const char* /*pDriverName*/)
{
    if (gOpenRef == 0 && nl_open() != 0) { return INVALID_HANDLE_VALUE; }
    InterlockedIncrement(&gOpenRef);
    return (HANDLE)1;   /* pseudo driver handle */
}

NL_API_SHIM VOID __stdcall CloseFilterDriver(HANDLE /*hOpen*/)
{
    if (InterlockedDecrement(&gOpenRef) == 0) { nl_close(); }
}

NL_API_SHIM BOOL __stdcall IsDriverLoaded(HANDLE /*hOpen*/)
{
    uint32_t magic = 0, ver = 0;
    return nl_version(&magic, &ver) == 0;
}

NL_API_SHIM DWORD __stdcall GetDriverVersion(HANDLE /*hOpen*/)
{
    /* 3.2-style version resource (atau tahan IOCTL versi driver ada nl_version) */
    return 0x00030200;
}

/* --- enumeration ------------------------------------------------------------ */

NL_API_SHIM BOOL __stdcall GetTcpipBoundAdaptersInfo(HANDLE /*hOpen*/, PTCP_AdapterList OutList)
{
    nl_adapter_t ad[NL_MAX_ADAPTERS];
    int n = 0, r, i;

    if (OutList == NULL) { return FALSE; }
    memset(OutList, 0, sizeof(*OutList));
    memset(ad, 0, sizeof ad);
    r = nl_enum(ad, NL_MAX_ADAPTERS, &n);
    if (r != 0) { return FALSE; }
    if (n > ADAPTER_LIST_SIZE) { n = ADAPTER_LIST_SIZE; }

    OutList->AdapterCount = (unsigned long)n;
    for (i = 0; i < n; ++i)
    {
        _snprintf_s(OutList->NameList[i], ADAPTER_NAME_SIZE, _TRUNCATE,
                    "\\DEVICE\\nl%d", ad[i].index);
        OutList->Handles[i] = handleFromIdx(ad[i].index);
        OutList->Mediums[i] = ad[i].medium;
        memcpy(OutList->CurrentAddresses[i], ad[i].mac, ETHER_ADDR_LENGTH);
        OutList->Mtus[i] = 1514;
    }
    return TRUE;
}

NL_API_SHIM BOOL __stdcall ConvertWindows2000AdapterName(
    const char* szAdapterName, char* szUserFriendlyName, DWORD dwUserFriendlyNameLength)
{
    if (szAdapterName == NULL || szUserFriendlyName == NULL || dwUserFriendlyNameLength < 2)
    {
        return FALSE;
    }
    _snprintf_s(szUserFriendlyName, dwUserFriendlyNameLength, _TRUNCATE, "%s", szAdapterName);
    return TRUE;
}

NL_API_SHIM BOOL __stdcall ConvertWindowsNTAdapterName(
    const char* szAdapterName, char* szUserFriendlyName, DWORD dwUserFriendlyNameLength)
{
    return ConvertWindows2000AdapterName(szAdapterName, szUserFriendlyName, dwUserFriendlyNameLength);
}

/* --- adapter mode ------------------------------------------------------------ */

NL_API_SHIM BOOL __stdcall SetAdapterMode(HANDLE /*hOpen*/, PADAPTER_MODE pMode)
{
    unsigned idx, nlMode = 0;
    if (pMode == NULL) { return FALSE; }
    idx = nlIdxFromHandle(pMode->hAdapterHandle);
    if (idx == 0xFFFFFFFFu) { return FALSE; }
    if (pMode->dwFlags & (MSTCP_FLAG_SENT_TUNNEL | MSTCP_FLAG_SENT_LISTEN)) { nlMode |= NL_MODE_TX; }
    if (pMode->dwFlags & (MSTCP_FLAG_RECV_TUNNEL | MSTCP_FLAG_RECV_LISTEN)) { nlMode |= NL_MODE_RX; }
    /* MSTCP_FLAG_FILTER_DIRECT/LOOPBACK_* tidak terpetakan (no-op) */
    return nl_set_mode(idx, nlMode) == 0;
}

NL_API_SHIM BOOL __stdcall GetAdapterMode(HANDLE /*hOpen*/, PADAPTER_MODE pMode)
{
    /* tanpa query mode di nlbridge — laporkan queue kosong (listen mati). */
    if (pMode == NULL) { return FALSE; }
    pMode->dwFlags = MSTCP_FLAG_FILTER_DIRECT;
    return TRUE;
}

/* --- capture ----------------------------------------------------------------- */

static void FillIB(PINTERMEDIATE_BUFFER ib, const nl_ring_entry_t* e)
{
    memset(ib, 0, sizeof(*ib));
    ib->Length = e->len;
    ib->DeviceFlags = (e->flags & NL_RING_TX) ? PACKET_FLAG_ON_SEND : PACKET_FLAG_ON_RECEIVE;
    memcpy(ib->Buffer, e->data, (e->len < MAX_ETHER_FRAME) ? e->len : MAX_ETHER_FRAME);
}

NL_API_SHIM BOOL __stdcall ReadPacket(HANDLE, PETH_REQUEST pReq)
{
    unsigned idx;
    nl_ring_entry_t e;
    int n = 0;

    if (pReq == NULL || pReq->EthPacket.Buffer == NULL) { return FALSE; }
    idx = nlIdxFromHandle(pReq->hAdapterHandle);
    if (idx == 0xFFFFFFFFu) { return FALSE; }
    if (nl_read_ring(&idx, &e, 1, &n) != 0) { return FALSE; }
    if (n == 0) { pReq->EthPacket.Buffer->Length = 0; return TRUE; }
    FillIB(pReq->EthPacket.Buffer, &e);
    return TRUE;
}

NL_API_SHIM BOOL __stdcall ReadPackets(HANDLE, PETH_M_REQUEST pReq)
{
    unsigned idx;
    nl_ring_entry_t eb[256];
    int n = 0, i, maxBatch;

    if (pReq == NULL || pReq->dwPacketsNumber == 0) { return FALSE; }
    idx = nlIdxFromHandle(pReq->hAdapterHandle);
    if (idx == 0xFFFFFFFFu) { return FALSE; }

    maxBatch = (int)pReq->dwPacketsNumber;
    if (maxBatch > 256) { maxBatch = 256; }
    if (nl_read_ring(&idx, eb, maxBatch, &n) != 0) { return FALSE; }

    for (i = 0; i < n; ++i)
    {
        if (pReq->EthPacket[i].Buffer == NULL) { break; }
        FillIB(pReq->EthPacket[i].Buffer, &eb[i]);
    }
    pReq->dwPacketsSuccess = (unsigned long)i;
    return TRUE;
}

/* --- injection ---------------------------------------------------------------- */

NL_API_SHIM BOOL __stdcall SendPacketToAdapter(HANDLE, PETH_REQUEST pReq)
{
    unsigned idx;
    if (pReq == NULL || pReq->EthPacket.Buffer == NULL) { return FALSE; }
    idx = nlIdxFromHandle(pReq->hAdapterHandle);
    if (idx == 0xFFFFFFFFu) { return FALSE; }
    if (pReq->EthPacket.Buffer->Length == 0 || pReq->EthPacket.Buffer->Length > MAX_ETHER_FRAME)
    {
        return FALSE;
    }
    return nl_inject_to(idx, pReq->EthPacket.Buffer->Buffer, pReq->EthPacket.Buffer->Length) == 0;
}

NL_API_SHIM BOOL __stdcall SendPacketToMstcp(HANDLE, PETH_REQUEST pReq)
{
    if (pReq == NULL || pReq->EthPacket.Buffer == NULL) { return FALSE; }
    if (pReq->EthPacket.Buffer->Length == 0 || pReq->EthPacket.Buffer->Length > MAX_ETHER_FRAME)
    {
        return FALSE;
    }
    return nl_inject_rx(pReq->EthPacket.Buffer->Buffer, pReq->EthPacket.Buffer->Length) == 0;
}

} /* extern "C" */
