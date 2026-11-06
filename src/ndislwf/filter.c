/*++

Copyright (c) Microsoft Corporation

Module Name:

    Filter.c

Abstract:

    Sample NDIS Lightweight filter driver

--*/

#include "precomp.h"

#define __FILENUMBER    'PNPF'

// This directive puts the DriverEntry function into the INIT segment of the
// driver.  To conserve memory, the code will be discarded when the driver's
// DriverEntry function returns.  You can declare other functions used only
// during initialization here.
#pragma NDIS_INIT_FUNCTION(DriverEntry)

//
// Global variables
//
NDIS_HANDLE         FilterDriverHandle; // NDIS handle for filter driver
NDIS_HANDLE         FilterDriverObject;
NDIS_HANDLE         NdisFilterDeviceHandle = NULL;
PDEVICE_OBJECT      NdisDeviceObject = NULL;

FILTER_LOCK         FilterListLock;
LIST_ENTRY          FilterModuleList;
static NL_RING_ENTRY* NlTxRing;
static NL_RING_ENTRY* NlRxRing;

volatile LONG64    NlInj;
volatile LONG64    NlBlocked;

/* v3 M1: block per arah (NlBlocked = jumlah keduanya) + hit tertinggal per
 * rule; index mengikuti posisi rule di policy global (Rules[i]). */
volatile LONG64    NlBlockedTx;
volatile LONG64    NlBlockedRx;
volatile LONG64    NlRuleHits[NL_POLICY_MAX_RULES];
#define NL_RULE_NONE   0xFFFFFFFFUL

/* ----------------------------------------------------------------
 * v4 M3: aksi netem per rule. Tabel EX (NlPolicyEx) aktif memotong
 * evaluasi frame KESELURUHAN; tabel policy lama menjadi fallback.
 * Semua state stokastik (RNG drop_pct, token bucket rate) dibaca/
 * ditulis hanya di bawah NlFlowLock bersama snapshot tabelnya.
 * Antrian delay satu driver (server DPC KeSetTimer): item TX bytes
 * di-replay lewat core inject TX, RX bytes lewat core indicate RX,
 * dan NBL TX yang ditahan di-forward-ulang apa adanya.
 * ---------------------------------------------------------------- */
static NL_POLICY_EX        NlPolicyEx;
static volatile LONG       NlExEnabled = 0;      /* mask = Enabled EX; 0 = EX mati */
static NL_VM_PROG          NlVm;
static volatile LONG       NlVmActive = 0;       /* 1 = program VM tayang */
static volatile LONG       NlVmEn = 0;           /* mask arah program VM */
VOID NlVmSet(IN const PNL_VM_PROG Src);
static BOOLEAN NlVmRun(IN const PUCHAR Data, IN ULONG Len, OUT PULONG RetOut);
/* v6 (M5): mirror + inject-to target */
static PMS_FILTER NlMirrorTake(IN PMS_FILTER pSrc, IN BOOLEAN Tx);
static PMS_FILTER NlFilterByIdxLocked(IN ULONG Idx);
static VOID NlMirrorDetach(IN PMS_FILTER pFilter);
static VOID NlPushTx(IN PMS_FILTER pFilter, IN PUCHAR Data, IN ULONG Len);
static VOID NlPushRx(IN PMS_FILTER pFilter, IN PUCHAR Data, IN ULONG Len);
static ULONG               NlRngSeed[NL_POLICY_MAX_RULES];
static ULONG64             NlRateTok[NL_POLICY_MAX_RULES];
static LARGE_INTEGER       NlRateLast[NL_POLICY_MAX_RULES];

/* policy block TX (global, simpel: satu set aturan untuk semua adapter)
 * SEMUA pembaca aturan lewat snapshot bawah NlFlowLock — global ini
 * tidak pernah dibaca liar (tulis di NlPolicySet pun di kunci sama). */
static NL_POLICY   NlPolicy;

#define NL_DELAY_MAX          4096
#define NL_DELAY_KIND_TX      1      /* bytes    -> NlInjectTxCore  */
#define NL_DELAY_KIND_RX      2      /* bytes    -> NlInjectRxCore  */
#define NL_DELAY_KIND_NBL     3      /* NBL asli -> forward send    */

typedef struct _NL_DELAY_ITEM {
    LIST_ENTRY     Link;
    PMS_FILTER     Fil;           /* hanya KIND_NBL (pemilik OutstandingSends) */
    ULONG          Kind;
    ULONG          Flags;         /* SendFlags asli untuk KIND_NBL */
    ULONG          Len;
    PVOID          P;             /* NBL untuk KIND_NBL */
    LARGE_INTEGER  Due;
    UCHAR          Data[NL_FRAME_MAX];
} NL_DELAY_ITEM, *PNL_DELAY_ITEM;

static KSPIN_LOCK             NlDelayLock;
static LIST_ENTRY             NlDelayList;
volatile LONG64               NlDelayCount;
volatile LONG64               NlDelayFired;
static KTIMER                 NlDelayTimer;
static KDPC                   NlDelayDpc;
static LARGE_INTEGER          NlDelayArmed;      /* due yang sedang dipasang */
static BOOLEAN                NlDelayArmedValid;

static NDIS_STATUS NlInjectTxCore(PUCHAR Data, ULONG Len, PMS_FILTER Target);
static NDIS_STATUS NlInjectRxCore(PUCHAR Data, ULONG Len);
static VOID NlDelayFire(PNL_DELAY_ITEM it);

static VOID
NlDelayDpcFn(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    );

/* v4: gerbang arah (gate) yang CONSISTEN dengan pemilihan tabel aktif:
 * VM aktif = mask Enabled program; EX hidup = mask Enabled EX;
 * keduanya mati = mask policy lama. */
static ULONG
NlGate(
    IN ULONG Dir
    )
{
    ULONG Ext = (ULONG)NlExEnabled;

    /* v5 (M4): program VM menggantikan SEMUA tabel, mask ikut program */
    if (InterlockedCompareExchange(&NlVmActive, 0, 0) != 0)
    {
        return ((ULONG)NlVmEn) & Dir;
    }

    if (Ext != 0)
    {
        return Ext & Dir;
    }
    return NlPolicy.Enabled & Dir;
}

/* ----------------------------------------------------------------
 * v4: antrian delay. Item dimatikan lewat satu DPC; due terkecil
 * TIDAK PERNAH ditimpa due lebih besar — KeSetTimer hanya dari
 * bawah NlDelayLock lewat NlDelayArm, dan armed-due adalah nilai
 * timer yang hidup saat ini.
 * ---------------------------------------------------------------- */
BOOLEAN
NlDelayAdd(
    IN PMS_FILTER Fil,          /* pemilik hanya untuk KIND_NBL */
    IN ULONG Kind,
    IN ULONG Flags,
    IN ULONG Len,
    IN PVOID Nbl,
    IN PUCHAR Bytes,
    IN ULONG DelayMs
    )
{
    PNL_DELAY_ITEM it;
    KIRQL OldIrql;
    LARGE_INTEGER Now;

    if (DelayMs == 0 || DelayMs > 60000)
    {
        return FALSE;
    }
    it = (PNL_DELAY_ITEM)NdisAllocateMemoryWithTagPriority(
                             FilterDriverHandle,
                             sizeof(NL_DELAY_ITEM),
                             FILTER_ALLOC_TAG,
                             NormalPoolPriority);
    if (it == NULL)
    {
        return FALSE;
    }
    NdisZeroMemory(it, sizeof(NL_DELAY_ITEM));
    it->Fil = Fil;
    it->Kind = Kind;
    it->Flags = Flags;
    it->Len = Len;
    it->P = Nbl;
    if (Bytes != NULL && Len != 0)
    {
        NdisMoveMemory(it->Data, Bytes, Len);
    }
    KeQuerySystemTimePrecise(&Now);
    it->Due.QuadPart = Now.QuadPart + (LONGLONG)DelayMs * 10000LL;

    KeAcquireSpinLock(&NlDelayLock, &OldIrql);
    if (NlDelayCount >= NL_DELAY_MAX)
    {
        KeReleaseSpinLock(&NlDelayLock, OldIrql);
        NdisFreeMemory(it, sizeof(NL_DELAY_ITEM), 0);
        return FALSE;
    }
    InsertTailList(&NlDelayList, &it->Link);
    NlDelayCount++;
    /* release + arm di dalam helper yang sama (lih. NlDelayArm) */
    if (!NlDelayArmedValid || it->Due.QuadPart < NlDelayArmed.QuadPart)
    {
        NlDelayArmed.QuadPart = it->Due.QuadPart;
        NlDelayArmedValid = TRUE;
        KeSetTimer(&NlDelayTimer, it->Due, &NlDelayDpc);
    }
    KeReleaseSpinLock(&NlDelayLock, OldIrql);
    return TRUE;
}

static VOID
NlDelayDpcFn(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )
{
    LIST_ENTRY   Work;
    KIRQL        OldIrql;
    LARGE_INTEGER Now;
    PLIST_ENTRY  Link;
    PNL_DELAY_ITEM it;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeQuerySystemTimePrecise(&Now);
    InitializeListHead(&Work);

    KeAcquireSpinLock(&NlDelayLock, &OldIrql);
    NlDelayArmedValid = FALSE;   /* due timer yang mati ini sudah dikonsumsi */
    Link = NlDelayList.Flink;
    while (Link != &NlDelayList)
    {
        PNL_DELAY_ITEM Next = CONTAINING_RECORD(Link, NL_DELAY_ITEM, Link);
        Link = Link->Flink;
        if (Next->Due.QuadPart <= Now.QuadPart)
        {
            RemoveEntryList(&Next->Link);
            NlDelayCount--;
            InsertTailList(&Work, &Next->Link);
        }
    }
    KeReleaseSpinLock(&NlDelayLock, OldIrql);

    /* proses LUAR kunci: forward/indicate bisa berat */
    while (!IsListEmpty(&Work))
    {
        Link = RemoveHeadList(&Work);
        it = CONTAINING_RECORD(Link, NL_DELAY_ITEM, Link);
        NlDelayFire(it);
    }

    /* re-arm bila masih ada sisa: due terkecil berikutnya */
    KeAcquireSpinLock(&NlDelayLock, &OldIrql);
    NlDelayArmedValid = FALSE;
    if (!IsListEmpty(&NlDelayList))
    {
        LARGE_INTEGER Next;
        Next.QuadPart = MAXLONGLONG;
        Link = NlDelayList.Flink;
        while (Link != &NlDelayList)
        {
            PNL_DELAY_ITEM Rem = CONTAINING_RECORD(Link, NL_DELAY_ITEM, Link);
            Link = Link->Flink;
            if (Rem->Due.QuadPart < Next.QuadPart)
            {
                Next.QuadPart = Rem->Due.QuadPart;
            }
        }
        if (Next.QuadPart != MAXLONGLONG)
        {
            NlDelayArmed.QuadPart = Next.QuadPart;
            NlDelayArmedValid = TRUE;
            KeSetTimer(&NlDelayTimer, Next, &NlDelayDpc);
        }
    }
    KeReleaseSpinLock(&NlDelayLock, OldIrql);
}

static VOID
NlDelayFire(
    IN PNL_DELAY_ITEM it
    )
{
    PMS_FILTER pFilter = it->Fil;

    InterlockedIncrement64(&NlDelayFired);

    if (it->Kind == NL_DELAY_KIND_NBL)
    {
        if (pFilter != NULL && pFilter->State == FilterRunning)
        {
            NdisFSendNetBufferLists(pFilter->FilterHandle,
                                    (PNET_BUFFER_LIST)it->P,
                                    NDIS_DEFAULT_PORT_NUMBER,
                                    NDIS_SEND_FLAGS_DISPATCH_LEVEL);
        }
        else
        {
            /* sudah pause/detach saat item pop-out: balikin gagal seperti
             * path drop policy (OutstandingSends dikoreksi manual). */
            if (pFilter == NULL)
            {
                NdisFreeMemory(it, sizeof(NL_DELAY_ITEM), 0);
                return;
            }
            if (pFilter->TrackSends)
            {
                FILTER_ACQUIRE_LOCK(&pFilter->Lock, TRUE);
                pFilter->OutstandingSends--;
                FILTER_RELEASE_LOCK(&pFilter->Lock, TRUE);
            }
            NET_BUFFER_LIST_STATUS((PNET_BUFFER_LIST)it->P) = NDIS_STATUS_FAILURE;
            NdisFSendNetBufferListsComplete(pFilter->FilterHandle,
                                            (PNET_BUFFER_LIST)it->P,
                                            NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL);
        }
    }
    else if (it->Kind == NL_DELAY_KIND_TX)
    {
        (VOID)NlInjectTxCore(it->Data, it->Len, NULL);
    }
    else if (it->Kind == NL_DELAY_KIND_RX)
    {
        (VOID)NlInjectRxCore(it->Data, it->Len);
    }
    NdisFreeMemory(it, sizeof(NL_DELAY_ITEM), 0);
}

VOID
NlDelayDrain(
    IN struct _MS_FILTER *pFilter
    )
/*++  Buang semua item KIND_NBL milik instance ini (pasang State != Running):
     di-complete sendiri dengan NDIS_STATUS_FAILURE dan koreksi
     OutstandingSends manual — dipanggil dari FilterPause (invarian pause:
     tidak ada send tersestar) dan tak berbahaya bila tak ada sisa. --*/
{
    LIST_ENTRY   Work;
    KIRQL        OldIrql;
    PLIST_ENTRY  Link;
    PNL_DELAY_ITEM it;
    PNET_BUFFER_LIST dropHead = NULL, dropTail = NULL, cbl;
    ULONG        DroppedCount = 0;

    InitializeListHead(&Work);
    KeAcquireSpinLock(&NlDelayLock, &OldIrql);
    Link = NlDelayList.Flink;
    while (Link != &NlDelayList)
    {
        PNL_DELAY_ITEM Next = CONTAINING_RECORD(Link, NL_DELAY_ITEM, Link);
        Link = Link->Flink;
        if (Next->Kind == NL_DELAY_KIND_NBL && Next->Fil == pFilter)
        {
            RemoveEntryList(&Next->Link);
            NlDelayCount--;
            InsertTailList(&Work, &Next->Link);
        }
    }
    KeReleaseSpinLock(&NlDelayLock, OldIrql);

    while (!IsListEmpty(&Work))
    {
        Link = RemoveHeadList(&Work);
        it = CONTAINING_RECORD(Link, NL_DELAY_ITEM, Link);
        NET_BUFFER_LIST_NEXT_NBL((PNET_BUFFER_LIST)it->P) = NULL;
        if (dropTail != NULL) { NET_BUFFER_LIST_NEXT_NBL(dropTail) = (PNET_BUFFER_LIST)it->P; }
        else { dropHead = (PNET_BUFFER_LIST)it->P; }
        dropTail = (PNET_BUFFER_LIST)it->P;
        DroppedCount++;
        NdisFreeMemory(it, sizeof(NL_DELAY_ITEM), 0);
    }

    if (dropHead != NULL)
    {
        if (pFilter->TrackSends)
        {
            FILTER_ACQUIRE_LOCK(&pFilter->Lock, FALSE);
            pFilter->OutstandingSends -= DroppedCount;
            FILTER_RELEASE_LOCK(&pFilter->Lock, FALSE);
        }
        cbl = dropHead;
        while (cbl != NULL)
        {
            NET_BUFFER_LIST_STATUS(cbl) = NDIS_STATUS_FAILURE;
            cbl = NET_BUFFER_LIST_NEXT_NBL(cbl);
        }
        NdisFSendNetBufferListsComplete(pFilter->FilterHandle, dropHead, 0);
    }
}

VOID
NlDelayUnload(
    VOID
    )
/*++  Bebas sisa item saat unload (NBL tidak di-complete: semua module
     sudah lepas — hanya telan sinkron). --*/
{
    KIRQL        OldIrql;
    PLIST_ENTRY  Link;
    PNL_DELAY_ITEM it;

    KeCancelTimer(&NlDelayTimer);
    KeAcquireSpinLock(&NlDelayLock, &OldIrql);
    NlDelayArmedValid = FALSE;
    while (!IsListEmpty(&NlDelayList))
    {
        Link = RemoveHeadList(&NlDelayList);
        it = CONTAINING_RECORD(Link, NL_DELAY_ITEM, Link);
        NdisFreeMemory(it, sizeof(NL_DELAY_ITEM), 0);
    }
    NlDelayCount = 0;
    KeReleaseSpinLock(&NlDelayLock, OldIrql);
}

VOID
NlDelayInit(
    VOID
    )
{
    InitializeListHead(&NlDelayList);
    NlDelayCount = 0;
    NlDelayFired = 0;
    NlDelayArmedValid = FALSE;
    KeInitializeSpinLock(&NlDelayLock);
    KeInitializeTimer(&NlDelayTimer);
    KeInitializeDpc(&NlDelayDpc, NlDelayDpcFn, NULL);
}

/* v2: flow cache 5-tuple (keputusan policy per ARAH TX di-cache) */
typedef struct _NL_FLOW_KEY {
    ULONG   Sip;
    ULONG   Dip;
    USHORT  Sp;
    USHORT  Dp;
    UCHAR   Proto;
    UCHAR   Pad;
} NL_FLOW_KEY, *PNL_FLOW_KEY;

typedef struct _NL_FLOW_ENT {
    volatile LONG64 Gen;        /* generasi policy saat entri sah; 0 = invalid */
    NL_FLOW_KEY     Key;
    volatile LONG   Blocked;
    volatile LONG   Rule;       /* rule pembuat verdict; -1 = tidak tahu */
} NL_FLOW_ENT, *PNL_FLOW_ENT;

#define NL_FLOW_TABLE_BIT   14
#define NL_FLOW_ENTRIES     (1UL << NL_FLOW_TABLE_BIT)
#define NL_FLOW_MASK        (NL_FLOW_ENTRIES - 1UL)

static NL_FLOW_ENT        *NlFlowTab = NULL;
static KSPIN_LOCK          NlFlowLock;
static volatile LONG64     NlFlowGen = 1;
static volatile LONG64     NlFlowHit;
static volatile LONG64     NlFlowMiss;

LONG                NlInstanceCounter = -1;  /* ordinal adapter */

NDIS_FILTER_PARTIAL_CHARACTERISTICS DefaultChars = {
{ 0, 0, 0},
      0,
      FilterSendNetBufferLists,
      FilterSendNetBufferListsComplete,
      NULL,
      FilterReceiveNetBufferLists,
      FilterReturnNetBufferLists
};


_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT      DriverObject,
    PUNICODE_STRING     RegistryPath
    )
/*++

Routine Description:

    First entry point to be called, when this driver is loaded.
    Register with NDIS as a filter driver and create a device
    for communication with user-mode.

Arguments:

    DriverObject - pointer to the system's driver object structure
                   for this driver

    RegistryPath - system's registry path for this driver

Return Value:

    STATUS_SUCCESS if all initialization is successful, STATUS_XXX
    error code if not.

--*/
{
    NDIS_STATUS Status;
    NDIS_FILTER_DRIVER_CHARACTERISTICS      FChars;
    NDIS_STRING ServiceName  = RTL_CONSTANT_STRING(FILTER_SERVICE_NAME);
    NDIS_STRING UniqueName   = RTL_CONSTANT_STRING(FILTER_UNIQUE_NAME);
    NDIS_STRING FriendlyName = RTL_CONSTANT_STRING(FILTER_FRIENDLY_NAME);
    BOOLEAN bFalse = FALSE;

    UNREFERENCED_PARAMETER(RegistryPath);

    DEBUGP(DL_TRACE, "===>DriverEntry...\n");

    FilterDriverObject = DriverObject;

    do
    {
        NdisZeroMemory(&FChars, sizeof(NDIS_FILTER_DRIVER_CHARACTERISTICS));
        FChars.Header.Type = NDIS_OBJECT_TYPE_FILTER_DRIVER_CHARACTERISTICS;
        FChars.Header.Size = sizeof(NDIS_FILTER_DRIVER_CHARACTERISTICS);
#if NDIS_SUPPORT_NDIS61
        FChars.Header.Revision = NDIS_FILTER_CHARACTERISTICS_REVISION_2;
#else
        FChars.Header.Revision = NDIS_FILTER_CHARACTERISTICS_REVISION_1;
#endif

        FChars.MajorNdisVersion = FILTER_MAJOR_NDIS_VERSION;
        FChars.MinorNdisVersion = FILTER_MINOR_NDIS_VERSION;
        FChars.MajorDriverVersion = 1;
        FChars.MinorDriverVersion = 0;
        FChars.Flags = 0;

        FChars.FriendlyName = FriendlyName;
        FChars.UniqueName = UniqueName;
        FChars.ServiceName = ServiceName;

        //
        // TODO: Most handlers are optional, however, this sample includes them
        // all for illustrative purposes.  If you do not need a particular
        // handler, set it to NULL and NDIS will more efficiently pass the
        // operation through on your behalf.
        //
        FChars.SetOptionsHandler = FilterRegisterOptions;
        FChars.AttachHandler = FilterAttach;
        FChars.DetachHandler = FilterDetach;
        FChars.RestartHandler = FilterRestart;
        FChars.PauseHandler = FilterPause;
        FChars.SetFilterModuleOptionsHandler = FilterSetModuleOptions;
        FChars.OidRequestHandler = FilterOidRequest;
        FChars.OidRequestCompleteHandler = FilterOidRequestComplete;
        FChars.CancelOidRequestHandler = FilterCancelOidRequest;

        FChars.SendNetBufferListsHandler = FilterSendNetBufferLists;
        FChars.ReturnNetBufferListsHandler = FilterReturnNetBufferLists;
        FChars.SendNetBufferListsCompleteHandler = FilterSendNetBufferListsComplete;
        FChars.ReceiveNetBufferListsHandler = FilterReceiveNetBufferLists;
        FChars.DevicePnPEventNotifyHandler = FilterDevicePnPEventNotify;
        FChars.NetPnPEventHandler = FilterNetPnPEvent;
        FChars.StatusHandler = FilterStatus;
        FChars.CancelSendNetBufferListsHandler = FilterCancelSendNetBufferLists;

        DriverObject->DriverUnload = FilterUnload;

        FilterDriverHandle = NULL;

        //
        // Initialize spin locks
        //
        FILTER_INIT_LOCK(&FilterListLock);

        InitializeListHead(&FilterModuleList);

        Status = NdisFRegisterFilterDriver(DriverObject,
                                           (NDIS_HANDLE)FilterDriverObject,
                                           &FChars,
                                           &FilterDriverHandle);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, "Register filter driver failed.\n");
            break;
        }
        NlInjectInit();
        NlDelayInit();
        NlFlowInit();

        Status = FilterRegisterDevice();

        if (Status != NDIS_STATUS_SUCCESS)
        {
            NlFlowUninit();
            NlInjectUninit();
            NdisFDeregisterFilterDriver(FilterDriverHandle);
            FILTER_FREE_LOCK(&FilterListLock);
            DEBUGP(DL_WARN, "Register device for the filter driver failed.\n");
            break;
        }


    }
    while(bFalse);


    DEBUGP(DL_TRACE, "<===DriverEntry, Status = %8x\n", Status);
    return Status;

}

_Use_decl_annotations_
NDIS_STATUS
FilterRegisterOptions(
    NDIS_HANDLE  NdisFilterDriverHandle,
    NDIS_HANDLE  FilterDriverContext
    )
/*++

Routine Description:

    Register optional handlers with NDIS.  This sample does not happen to
    have any optional handlers to register, so this routine does nothing
    and could simply have been omitted.  However, for illustrative purposes,
    it is presented here.

Arguments:

    NdisFilterDriverHandle - pointer the driver handle received from
                             NdisFRegisterFilterDriver

    FilterDriverContext    - pointer to our context passed into
                             NdisFRegisterFilterDriver

Return Value:

    NDIS_STATUS_SUCCESS

--*/
{
    DEBUGP(DL_TRACE, "===>FilterRegisterOptions\n");

    ASSERT(NdisFilterDriverHandle == FilterDriverHandle);
    ASSERT(FilterDriverContext == (NDIS_HANDLE)FilterDriverObject);

    if ((NdisFilterDriverHandle != (NDIS_HANDLE)FilterDriverHandle) ||
        (FilterDriverContext != (NDIS_HANDLE)FilterDriverObject))
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    DEBUGP(DL_TRACE, "<===FilterRegisterOptions\n");

    return NDIS_STATUS_SUCCESS;
}


_Use_decl_annotations_
NDIS_STATUS
FilterAttach(
    NDIS_HANDLE                     NdisFilterHandle,
    NDIS_HANDLE                     FilterDriverContext,
    PNDIS_FILTER_ATTACH_PARAMETERS  AttachParameters
    )
/*++

Routine Description:

    Filter attach routine.
    Create filter's context, allocate NetBufferLists and NetBuffer pools and any
    other resources, and read configuration if needed.

Arguments:

    NdisFilterHandle - Specify a handle identifying this instance of the filter. FilterAttach
                       should save this handle. It is a required  parameter in subsequent calls
                       to NdisFxxx functions.
    FilterDriverContext - Filter driver context passed to NdisFRegisterFilterDriver.

    AttachParameters - attach parameters

Return Value:

    NDIS_STATUS_SUCCESS: FilterAttach successfully allocated and initialize data structures
                         for this filter instance.
    NDIS_STATUS_RESOURCES: FilterAttach failed due to insufficient resources.
    NDIS_STATUS_FAILURE: FilterAttach could not set up this instance of this filter and it has called
                         NdisWriteErrorLogEntry with parameters specifying the reason for failure.

N.B.:  FILTER can use NdisRegisterDeviceEx to create a device, so the upper
    layer can send Irps to the filter.

--*/
{
    PMS_FILTER              pFilter = NULL;
    NDIS_STATUS             Status = NDIS_STATUS_SUCCESS;
    NDIS_FILTER_ATTRIBUTES  FilterAttributes;
    ULONG                   Size;
    BOOLEAN               bFalse = FALSE;

    DEBUGP(DL_TRACE, "===>FilterAttach: NdisFilterHandle %p\n", NdisFilterHandle);

    do
    {
        ASSERT(FilterDriverContext == (NDIS_HANDLE)FilterDriverObject);
        if (FilterDriverContext != (NDIS_HANDLE)FilterDriverObject)
        {
            Status = NDIS_STATUS_INVALID_PARAMETER;
            break;
        }

        // Verify the media type is supported.  This is a last resort; the
        // the filter should never have been bound to an unsupported miniport
        // to begin with.  If this driver is marked as a Mandatory filter (which
        // is the default for this sample; see the INF file), failing to attach
        // here will leave the network adapter in an unusable state.
        //
        // Your setup/install code should not bind the filter to unsupported
        // media types.
        if ((AttachParameters->MiniportMediaType != NdisMedium802_3)
                && (AttachParameters->MiniportMediaType != NdisMediumWan)
                && (AttachParameters->MiniportMediaType != NdisMediumWirelessWan))
        {
           DEBUGP(DL_ERROR, "Unsupported media type.\n");

           Status = NDIS_STATUS_INVALID_PARAMETER;
           break;
        }

        Size = sizeof(MS_FILTER) +
               AttachParameters->FilterModuleGuidName->Length +
               AttachParameters->BaseMiniportInstanceName->Length +
               AttachParameters->BaseMiniportName->Length;

        pFilter = (PMS_FILTER)FILTER_ALLOC_MEM(NdisFilterHandle, Size);
        if (pFilter == NULL)
        {
            DEBUGP(DL_WARN, "Failed to allocate context structure.\n");
            Status = NDIS_STATUS_RESOURCES;
            break;
        }

        NdisZeroMemory(pFilter, sizeof(MS_FILTER));

        pFilter->FilterModuleName.Length = pFilter->FilterModuleName.MaximumLength = AttachParameters->FilterModuleGuidName->Length;
        pFilter->FilterModuleName.Buffer = (PWSTR)((PUCHAR)pFilter + sizeof(MS_FILTER));
        NdisMoveMemory(pFilter->FilterModuleName.Buffer,
                        AttachParameters->FilterModuleGuidName->Buffer,
                        pFilter->FilterModuleName.Length);



        pFilter->MiniportFriendlyName.Length = pFilter->MiniportFriendlyName.MaximumLength = AttachParameters->BaseMiniportInstanceName->Length;
        pFilter->MiniportFriendlyName.Buffer = (PWSTR)((PUCHAR)pFilter->FilterModuleName.Buffer + pFilter->FilterModuleName.Length);
        NdisMoveMemory(pFilter->MiniportFriendlyName.Buffer,
                        AttachParameters->BaseMiniportInstanceName->Buffer,
                        pFilter->MiniportFriendlyName.Length);


        pFilter->MiniportName.Length = pFilter->MiniportName.MaximumLength = AttachParameters->BaseMiniportName->Length;
        pFilter->MiniportName.Buffer = (PWSTR)((PUCHAR)pFilter->MiniportFriendlyName.Buffer +
                                                   pFilter->MiniportFriendlyName.Length);
        NdisMoveMemory(pFilter->MiniportName.Buffer,
                        AttachParameters->BaseMiniportName->Buffer,
                        pFilter->MiniportName.Length);

        pFilter->MiniportIfIndex = AttachParameters->BaseMiniportIfIndex;
        //
        // The filter should initialize TrackReceives and TrackSends properly. For this
        // driver, since its default characteristic has both a send and a receive handler,
        // these fields are initialized to TRUE.
        //
        pFilter->TrackReceives = TRUE;
        pFilter->TrackSends = TRUE;
        pFilter->FilterHandle = NdisFilterHandle;
        /* refcount: 1 milik list module; lepas oleh FilterDetach.
         * IOCTL (MAP_RING/UNMAP/GET_EVENT) menambah ref pribadi sesaat. */
        pFilter->RefCount = 1;
        pFilter->NlIdx = (UINT32)InterlockedIncrement((volatile LONG *)&NlInstanceCounter) - 1;
        NlRingInit(pFilter);
        if (pFilter->NlTxRing == NULL || pFilter->NlRxRing == NULL)
        {
            InterlockedDecrement((volatile LONG *)&NlInstanceCounter);
            FILTER_FREE_MEM(pFilter);
            DEBUGP(DL_WARN, "Failed to allocate per-adapter ring.\n");
            return NDIS_STATUS_RESOURCES;
        }


        NdisZeroMemory(&FilterAttributes, sizeof(NDIS_FILTER_ATTRIBUTES));
        FilterAttributes.Header.Revision = NDIS_FILTER_ATTRIBUTES_REVISION_1;
        FilterAttributes.Header.Size = sizeof(NDIS_FILTER_ATTRIBUTES);
        FilterAttributes.Header.Type = NDIS_OBJECT_TYPE_FILTER_ATTRIBUTES;
        FilterAttributes.Flags = 0;

        NDIS_DECLARE_FILTER_MODULE_CONTEXT(MS_FILTER);
        Status = NdisFSetAttributes(NdisFilterHandle,
                                    pFilter,
                                    &FilterAttributes);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, "Failed to set attributes.\n");
            break;
        }


        pFilter->State = FilterPaused;

        FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
        InsertHeadList(&FilterModuleList, &pFilter->FilterModuleLink);
        FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

    }
    while (bFalse);

    if (Status != NDIS_STATUS_SUCCESS)
    {
        if (pFilter != NULL)
        {
            if (pFilter->NlTxRing != NULL)
            {
                NlRingUninit(pFilter);
            }
            FILTER_FREE_MEM(pFilter);
        }
    }

    DEBUGP(DL_TRACE, "<===FilterAttach:    Status %x\n", Status);
    return Status;
}

_Use_decl_annotations_
NDIS_STATUS
FilterPause(
    NDIS_HANDLE                     FilterModuleContext,
    PNDIS_FILTER_PAUSE_PARAMETERS   PauseParameters
    )
/*++

Routine Description:

    Filter pause routine.
    Complete all the outstanding sends and queued sends,
    wait for all the outstanding recvs to be returned
    and return all the queued receives.

Arguments:

    FilterModuleContext - pointer to the filter context stucture
    PauseParameters     - additional information about the pause

Return Value:

    NDIS_STATUS_SUCCESS if filter pauses successfully, NDIS_STATUS_PENDING
    if not.  No other return value is allowed (pause must succeed, eventually).

N.B.: When the filter is in Pausing state, it can still process OID requests,
    complete sending, and returning packets to NDIS, and also indicate status.
    After this function completes, the filter must not attempt to send or
    receive packets, but it may still process OID requests and status
    indications.

--*/
{
    PMS_FILTER          pFilter = (PMS_FILTER)(FilterModuleContext);
    NDIS_STATUS         Status;
    BOOLEAN               bFalse = FALSE;

    UNREFERENCED_PARAMETER(PauseParameters);

    DEBUGP(DL_TRACE, "===>NDISLWF FilterPause: FilterInstance %p\n", FilterModuleContext);

    //
    // Set the flag that the filter is going to pause
    //
    FILTER_ASSERT(pFilter->State == FilterRunning);

    FILTER_ACQUIRE_LOCK(&pFilter->Lock, bFalse);
    pFilter->State = FilterPausing;
    FILTER_RELEASE_LOCK(&pFilter->Lock, bFalse);

    /* v4: invarian pause — tidak ada send tersestar. NBL yang ditahan
     * antrian delay harus selesai dulu (complete gagal), jika tidak
     * pause akan ada send yang mengayun saat datapath bisa dihentikan. */
    NlDelayDrain(pFilter);

    //
    // Do whatever work is required to bring the filter into the Paused state.
    //
    // If you have diverted and queued any send or receive NBLs, return them
    // now.
    //
    // If you send or receive original NBLs, stop doing that and wait for your
    // NBLs to return to you now.
    //


    Status = NDIS_STATUS_SUCCESS;

    pFilter->State = FilterPaused;

    DEBUGP(DL_TRACE, "<===FilterPause:  Status %x\n", Status);
    return Status;
}

_Use_decl_annotations_
NDIS_STATUS
FilterRestart(
    NDIS_HANDLE                     FilterModuleContext,
    PNDIS_FILTER_RESTART_PARAMETERS RestartParameters
    )
/*++

Routine Description:

    Filter restart routine.
    Start the datapath - begin sending and receiving NBLs.

Arguments:

    FilterModuleContext - pointer to the filter context stucture.
    RestartParameters   - additional information about the restart operation.

Return Value:

    NDIS_STATUS_SUCCESS: if filter restarts successfully
    NDIS_STATUS_XXX: Otherwise.

--*/
{
    NDIS_STATUS     Status;
    PMS_FILTER      pFilter = (PMS_FILTER)FilterModuleContext;
    NDIS_HANDLE     ConfigurationHandle = NULL;


    PNDIS_RESTART_GENERAL_ATTRIBUTES NdisGeneralAttributes;
    PNDIS_RESTART_ATTRIBUTES         NdisRestartAttributes;
    NDIS_CONFIGURATION_OBJECT        ConfigObject;

    DEBUGP(DL_TRACE, "===>FilterRestart:   FilterModuleContext %p\n", FilterModuleContext);

    FILTER_ASSERT(pFilter->State == FilterPaused);

    ConfigObject.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    ConfigObject.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
    ConfigObject.Header.Size = sizeof(NDIS_CONFIGURATION_OBJECT);
    ConfigObject.NdisHandle = FilterDriverHandle;
    ConfigObject.Flags = 0;

    Status = NdisOpenConfigurationEx(&ConfigObject, &ConfigurationHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        //
        // Filter driver can choose to fail the restart if it cannot open the configuration
        //

#if 0
        //
        // The code is here just to demonstrate how to call NDIS to write an
        // event to the eventlog.
        //
        PWCHAR              ErrorString = L"NDISLWF";

        DEBUGP(DL_WARN, "FilterRestart: Cannot open configuration.\n");
        NdisWriteEventLogEntry(FilterDriverObject,
                                EVENT_NDIS_DRIVER_FAILURE,
                                0,
                                1,
                                &ErrorString,
                                sizeof(Status),
                                &Status);
#endif

    }

    //
    // This sample doesn't actually do anything with the configuration handle;
    // it is opened here for illustrative purposes.  If you do not need to
    // read configuration, you may omit the code manipulating the
    // ConfigurationHandle.
    //

    if (Status == NDIS_STATUS_SUCCESS)
    {
        NdisCloseConfiguration(ConfigurationHandle);
    }

    NdisRestartAttributes = RestartParameters->RestartAttributes;

    //
    // If NdisRestartAttributes is not NULL, then the filter can modify generic
    // attributes and add new media specific info attributes at the end.
    // Otherwise, if NdisRestartAttributes is NULL, the filter should not try to
    // modify/add attributes.
    //
    if (NdisRestartAttributes != NULL)
    {
        PNDIS_RESTART_ATTRIBUTES   NextAttributes;

        ASSERT(NdisRestartAttributes->Oid == OID_GEN_MINIPORT_RESTART_ATTRIBUTES);

        NdisGeneralAttributes = (PNDIS_RESTART_GENERAL_ATTRIBUTES)NdisRestartAttributes->Data;

        //
        // Check to see if we need to change any attributes. For example, the
        // driver can change the current MAC address here. Or the driver can add
        // media specific info attributes.
        //
        NdisGeneralAttributes->LookaheadSize = 128;

        //
        // Check each attribute to see whether the filter needs to modify it.
        //
        NextAttributes = NdisRestartAttributes->Next;

        while (NextAttributes != NULL)
        {
            //
            // If somehow the filter needs to change a attributes which requires more space then
            // the current attributes:
            // 1. Remove the attribute from the Attributes list:
            //    TempAttributes = NextAttributes;
            //    NextAttributes = NextAttributes->Next;
            // 2. Free the memory for the current attributes: NdisFreeMemory(TempAttributes, 0 , 0);
            // 3. Dynamically allocate the memory for the new attributes by calling
            //    NdisAllocateMemoryWithTagPriority:
            //    NewAttributes = NdisAllocateMemoryWithTagPriority(Handle, size, Priority);
            // 4. Fill in the new attribute
            // 5. NewAttributes->Next = NextAttributes;
            // 6. NextAttributes = NewAttributes; // Just to make the next statement work.
            //
            NextAttributes = NextAttributes->Next;
        }

        //
        // Add a new attributes at the end
        // 1. Dynamically allocate the memory for the new attributes by calling
        //    NdisAllocateMemoryWithTagPriority.
        // 2. Fill in the new attribute
        // 3. NextAttributes->Next = NewAttributes;
        // 4. NewAttributes->Next = NULL;



    }

    //
    // If everything is OK, set the filter in running state.
    //
    pFilter->State = FilterRunning; // when successful

    // v1: medium tetap NdisMedium802_3; MAC diisi saat frame TX pertama
    pFilter->NlMedium = 0; /* NdisMedium802_3 */

    Status = NDIS_STATUS_SUCCESS;


    //
    // Ensure the state is Paused if restart failed.
    //

    if (Status != NDIS_STATUS_SUCCESS)
    {
        pFilter->State = FilterPaused;
    }


    DEBUGP(DL_TRACE, "<===FilterRestart:  FilterModuleContext %p, Status %x\n", FilterModuleContext, Status);
    return Status;
}


_Use_decl_annotations_
VOID
FilterDetach(
    NDIS_HANDLE     FilterModuleContext
    )
/*++

Routine Description:

    Filter detach routine.
    This is a required function that will deallocate all the resources allocated during
    FilterAttach. NDIS calls FilterAttach to remove a filter instance from a filter stack.

Arguments:

    FilterModuleContext - pointer to the filter context area.

Return Value:
    None.

NOTE: Called at PASSIVE_LEVEL and the filter is in paused state

--*/
{
    PMS_FILTER                  pFilter = (PMS_FILTER)FilterModuleContext;
    BOOLEAN                      bFalse = FALSE;
    BOOLEAN                      deferFree = FALSE;


    DEBUGP(DL_TRACE, "===>FilterDetach:    FilterInstance %p\n", FilterModuleContext);


    //
    // Filter must be in paused state
    //
    FILTER_ASSERT(pFilter->State == FilterPaused);


    //
    // Detach must not fail, so do not put any code here that can possibly fail.
    //

    // v2: lepas mapping shared ring (locked pages) bila masih ada.
    // Tandai dulu di bawah ring lock: MAP_RING yang sedang prepare tidak
    // boleh menginstalasi ke instance yang sudah lepas dari list.
    {
        KIRQL noteIrql;
        KeAcquireSpinLock(&pFilter->NlTxRingLock, &noteIrql);
        pFilter->NlDetached = TRUE;
        KeReleaseSpinLock(&pFilter->NlTxRingLock, noteIrql);
    }
    NlShmUnmap(pFilter);

    /* v4: belt-and-braces — pause sudah men-drain NBL delay milik instance
     * ini; kalau ada yang lolos (TrackSends=FALSE dsb.) buang di sini. */
    NlDelayDrain(pFilter);

    /* v6 (M5): lepas mirror sebagai Src maupun Dst sebelum list-ref turun. */
    NlMirrorDetach(pFilter);

    //
    // Free filter instance name if allocated.
    //
    if (pFilter->FilterName.Buffer != NULL)
    {
        FILTER_FREE_MEM(pFilter->FilterName.Buffer);
    }


    FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
    RemoveEntryList(&pFilter->FilterModuleLink);
    {
        /* ref isi module turun; bila ada IOCTL yang sedang menahan referensi
         * dia yang membebaskan memori ini (filterDereferenceModule). */
        BOOLEAN freeNow = (InterlockedDecrement((volatile LONG *)&pFilter->RefCount) == 0);
        FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
        deferFree = freeNow;
    }

    NlRingUninit(pFilter);

    if (deferFree)
    {
        FILTER_FREE_LOCK(&pFilter->Lock);
        FILTER_FREE_MEM(pFilter);
    }

    DEBUGP(DL_TRACE, "<===FilterDetach Successfully\n");
    return;
}

_Use_decl_annotations_
VOID
FilterUnload(
    PDRIVER_OBJECT      DriverObject
    )
/*++

Routine Description:

    Filter driver's unload routine.
    Deregister the driver from NDIS.

Arguments:

    DriverObject - pointer to the system's driver object structure
                   for this driver

Return Value:

    NONE

--*/
{
#if DBG
    BOOLEAN               bFalse = FALSE;
#endif

    UNREFERENCED_PARAMETER(DriverObject);

    DEBUGP(DL_TRACE, "===>FilterUnload\n");

    //
    // Should free the filter context list
    //
    FilterDeregisterDevice();
    NlInjectUninit();
    NlDelayUnload();
    NlShmUnmapAll();
    /* dereg dulu: semua data path mati karena detach, baru tabel flow aman
     *bebaskan (beda urutan ini dari versi lama yang UAF saat unload). */
    NdisFDeregisterFilterDriver(FilterDriverHandle);
    NlFlowUninit();

#if DBG
    FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
    ASSERT(IsListEmpty(&FilterModuleList));

    FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

#endif

    FILTER_FREE_LOCK(&FilterListLock);

    DEBUGP(DL_TRACE, "<===FilterUnload\n");

    return;

}

_Use_decl_annotations_
NDIS_STATUS
FilterOidRequest(
    NDIS_HANDLE         FilterModuleContext,
    PNDIS_OID_REQUEST   Request
    )
/*++

Routine Description:

    Request handler
    Handle requests from upper layers

Arguments:

    FilterModuleContext   - our filter
    Request               - the request passed down


Return Value:

     NDIS_STATUS_SUCCESS
     NDIS_STATUS_PENDING
     NDIS_STATUS_XXX

NOTE: Called at <= DISPATCH_LEVEL  (unlike a miniport's MiniportOidRequest)

--*/
{
    PMS_FILTER              pFilter = (PMS_FILTER)FilterModuleContext;
    NDIS_STATUS             Status;
    PNDIS_OID_REQUEST       ClonedRequest=NULL;
    BOOLEAN                 bSubmitted = FALSE;
    PFILTER_REQUEST_CONTEXT Context;
    BOOLEAN                 bFalse = FALSE;


    DEBUGP(DL_TRACE, "===>FilterOidRequest: Request %p.\n", Request);

    //
    // Most of the time, a filter will clone the OID request and pass down
    // the clone.  When the clone completes, the filter completes the original
    // OID request.
    //
    // If your filter needs to modify a specific request, it can modify the
    // request before or after sending down the cloned request.  Or, it can
    // complete the original request on its own without sending down any
    // clone at all.
    //
    // If your filter driver does not need to modify any OID requests, then
    // you may simply omit this routine entirely; NDIS will pass OID requests
    // down on your behalf.  This is more efficient than implementing a
    // routine that does nothing but clone all requests, as in the sample here.
    //

    do
    {
        Status = NdisAllocateCloneOidRequest(pFilter->FilterHandle,
                                            Request,
                                            FILTER_TAG,
                                            &ClonedRequest);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, "FilerOidRequest: Cannot Clone Request\n");
            break;
        }

        Context = (PFILTER_REQUEST_CONTEXT)(&ClonedRequest->SourceReserved[0]);
        *Context = Request;

        bSubmitted = TRUE;

        //
        // Use same request ID
        //
        ClonedRequest->RequestId = Request->RequestId;

        pFilter->PendingOidRequest = ClonedRequest;


        Status = NdisFOidRequest(pFilter->FilterHandle, ClonedRequest);

        if (Status != NDIS_STATUS_PENDING)
        {


            FilterOidRequestComplete(pFilter, ClonedRequest, Status);
            Status = NDIS_STATUS_PENDING;
        }


    }while (bFalse);

    if (bSubmitted == FALSE)
    {
        switch(Request->RequestType)
        {
            case NdisRequestMethod:
                Request->DATA.METHOD_INFORMATION.BytesRead = 0;
                Request->DATA.METHOD_INFORMATION.BytesNeeded = 0;
                Request->DATA.METHOD_INFORMATION.BytesWritten = 0;
                break;

            case NdisRequestSetInformation:
                Request->DATA.SET_INFORMATION.BytesRead = 0;
                Request->DATA.SET_INFORMATION.BytesNeeded = 0;
                break;

            case NdisRequestQueryInformation:
            case NdisRequestQueryStatistics:
            default:
                Request->DATA.QUERY_INFORMATION.BytesWritten = 0;
                Request->DATA.QUERY_INFORMATION.BytesNeeded = 0;
                break;
        }

    }
    DEBUGP(DL_TRACE, "<===FilterOidRequest: Status %8x.\n", Status);

    return Status;

}

_Use_decl_annotations_
VOID
FilterCancelOidRequest(
    NDIS_HANDLE             FilterModuleContext,
    PVOID                   RequestId
    )
/*++

Routine Description:

    Cancels an OID request

    If your filter driver does not intercept and hold onto any OID requests,
    then you do not need to implement this routine.  You may simply omit it.
    Furthermore, if the filter only holds onto OID requests so it can pass
    down a clone (the most common case) the filter does not need to implement
    this routine; NDIS will then automatically request that the lower-level
    filter/miniport cancel your cloned OID.

    Most filters do not need to implement this routine.

Arguments:

    FilterModuleContext   - our filter
    RequestId             - identifies the request(s) to cancel

--*/
{
    PMS_FILTER                          pFilter = (PMS_FILTER)FilterModuleContext;
    PNDIS_OID_REQUEST                   Request = NULL;
    PFILTER_REQUEST_CONTEXT             Context;
    PNDIS_OID_REQUEST                   OriginalRequest = NULL;
    BOOLEAN                             bFalse = FALSE;

    FILTER_ACQUIRE_LOCK(&pFilter->Lock, bFalse);

    Request = pFilter->PendingOidRequest;

    if (Request != NULL)
    {
        Context = (PFILTER_REQUEST_CONTEXT)(&Request->SourceReserved[0]);

        OriginalRequest = (*Context);
    }

    if ((OriginalRequest != NULL) && (OriginalRequest->RequestId == RequestId))
    {
        FILTER_RELEASE_LOCK(&pFilter->Lock, bFalse);

        NdisFCancelOidRequest(pFilter->FilterHandle, RequestId);
    }
    else
    {
        FILTER_RELEASE_LOCK(&pFilter->Lock, bFalse);
    }


}

_Use_decl_annotations_
VOID
FilterOidRequestComplete(
    NDIS_HANDLE         FilterModuleContext,
    PNDIS_OID_REQUEST   Request,
    NDIS_STATUS         Status
    )
/*++

Routine Description:

    Notification that an OID request has been completed

    If this filter sends a request down to a lower layer, and the request is
    pended, the FilterOidRequestComplete routine is invoked when the request
    is complete.  Most requests we've sent are simply clones of requests
    received from a higher layer; all we need to do is complete the original
    higher request.

    However, if this filter driver sends original requests down, it must not
    attempt to complete a pending request to the higher layer.

Arguments:

    FilterModuleContext   - our filter context area
    NdisRequest           - the completed request
    Status                - completion status

--*/
{
    PMS_FILTER                          pFilter = (PMS_FILTER)FilterModuleContext;
    PNDIS_OID_REQUEST                   OriginalRequest;
    PFILTER_REQUEST_CONTEXT             Context;
    BOOLEAN                             bFalse = FALSE;

    DEBUGP(DL_TRACE, "===>FilterOidRequestComplete, Request %p.\n", Request);

    Context = (PFILTER_REQUEST_CONTEXT)(&Request->SourceReserved[0]);
    OriginalRequest = (*Context);

    //
    // This is an internal request
    //
    if (OriginalRequest == NULL)
    {
        filterInternalRequestComplete(pFilter, Request, Status);
        return;
    }


    FILTER_ACQUIRE_LOCK(&pFilter->Lock, bFalse);

    ASSERT(pFilter->PendingOidRequest == Request);
    pFilter->PendingOidRequest = NULL;

    FILTER_RELEASE_LOCK(&pFilter->Lock, bFalse);


    //
    // Copy the information from the returned request to the original request
    //
    switch(Request->RequestType)
    {
        case NdisRequestMethod:
            OriginalRequest->DATA.METHOD_INFORMATION.OutputBufferLength =  Request->DATA.METHOD_INFORMATION.OutputBufferLength;
            OriginalRequest->DATA.METHOD_INFORMATION.BytesRead = Request->DATA.METHOD_INFORMATION.BytesRead;
            OriginalRequest->DATA.METHOD_INFORMATION.BytesNeeded = Request->DATA.METHOD_INFORMATION.BytesNeeded;
            OriginalRequest->DATA.METHOD_INFORMATION.BytesWritten = Request->DATA.METHOD_INFORMATION.BytesWritten;
            break;

        case NdisRequestSetInformation:
            OriginalRequest->DATA.SET_INFORMATION.BytesRead = Request->DATA.SET_INFORMATION.BytesRead;
            OriginalRequest->DATA.SET_INFORMATION.BytesNeeded = Request->DATA.SET_INFORMATION.BytesNeeded;
            break;

        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
        default:
            OriginalRequest->DATA.QUERY_INFORMATION.BytesWritten = Request->DATA.QUERY_INFORMATION.BytesWritten;
            OriginalRequest->DATA.QUERY_INFORMATION.BytesNeeded = Request->DATA.QUERY_INFORMATION.BytesNeeded;
            break;
    }


    (*Context) = NULL;

    NdisFreeCloneOidRequest(pFilter->FilterHandle, Request);

    NdisFOidRequestComplete(pFilter->FilterHandle, OriginalRequest, Status);

    DEBUGP(DL_TRACE, "<===FilterOidRequestComplete.\n");
}


_Use_decl_annotations_
VOID
FilterStatus(
    NDIS_HANDLE             FilterModuleContext,
    PNDIS_STATUS_INDICATION StatusIndication
    )
/*++

Routine Description:

    Status indication handler

Arguments:

    FilterModuleContext     - our filter context
    StatusIndication        - the status being indicated

NOTE: called at <= DISPATCH_LEVEL

  FILTER driver may call NdisFIndicateStatus to generate a status indication to
  all higher layer modules.

--*/
{
    PMS_FILTER              pFilter = (PMS_FILTER)FilterModuleContext;
#if DBG
    BOOLEAN                  bFalse = FALSE;
#endif

    DEBUGP(DL_TRACE, "===>FilterStatus, IndicateStatus = %8x.\n", StatusIndication->StatusCode);


    //
    // The filter may do processing on the status indication here, including
    // intercepting and dropping it entirely.  However, the sample does nothing
    // with status indications except pass them up to the higher layer.  It is
    // more efficient to omit the FilterStatus handler entirely if it does
    // nothing, but it is included in this sample for illustrative purposes.
    //

#if DBG
    FILTER_ACQUIRE_LOCK(&pFilter->Lock, bFalse);
    ASSERT(pFilter->bIndicating == FALSE);
    pFilter->bIndicating = TRUE;
    FILTER_RELEASE_LOCK(&pFilter->Lock, bFalse);
#endif // DBG

    NdisFIndicateStatus(pFilter->FilterHandle, StatusIndication);

#if DBG
    FILTER_ACQUIRE_LOCK(&pFilter->Lock, bFalse);
    ASSERT(pFilter->bIndicating == TRUE);
    pFilter->bIndicating = FALSE;
    FILTER_RELEASE_LOCK(&pFilter->Lock, bFalse);
#endif // DBG

    DEBUGP(DL_TRACE, "<===FilterStatus.\n");

}

_Use_decl_annotations_
VOID
FilterDevicePnPEventNotify(
    NDIS_HANDLE             FilterModuleContext,
    PNET_DEVICE_PNP_EVENT   NetDevicePnPEvent
    )
/*++

Routine Description:

    Device PNP event handler

Arguments:

    FilterModuleContext         - our filter context
    NetDevicePnPEvent           - a Device PnP event

NOTE: called at PASSIVE_LEVEL

--*/
{
    PMS_FILTER             pFilter = (PMS_FILTER)FilterModuleContext;
    NDIS_DEVICE_PNP_EVENT  DevicePnPEvent = NetDevicePnPEvent->DevicePnPEvent;
#if DBG
    BOOLEAN                bFalse = FALSE;
#endif

    DEBUGP(DL_TRACE, "===>FilterDevicePnPEventNotify: NetPnPEvent = %p.\n", NetDevicePnPEvent);

    //
    // The filter may do processing on the event here, including intercepting
    // and dropping it entirely.  However, the sample does nothing with Device
    // PNP events, except pass them down to the next lower* layer.  It is more
    // efficient to omit the FilterDevicePnPEventNotify handler entirely if it
    // does nothing, but it is included in this sample for illustrative purposes.
    //
    // * Trivia: Device PNP events percolate DOWN the stack, instead of upwards
    // like status indications and Net PNP events.  So the next layer is the
    // LOWER layer.
    //

    switch (DevicePnPEvent)
    {

        case NdisDevicePnPEventQueryRemoved:
        case NdisDevicePnPEventRemoved:
        case NdisDevicePnPEventSurpriseRemoved:
        case NdisDevicePnPEventQueryStopped:
        case NdisDevicePnPEventStopped:
        case NdisDevicePnPEventPowerProfileChanged:
        case NdisDevicePnPEventFilterListChanged:

            break;

        default:
            DEBUGP(DL_ERROR, "FilterDevicePnPEventNotify: Invalid event.\n");
            FILTER_ASSERT(bFalse);

            break;
    }

    NdisFDevicePnPEventNotify(pFilter->FilterHandle, NetDevicePnPEvent);

    DEBUGP(DL_TRACE, "<===FilterDevicePnPEventNotify\n");

}

_Use_decl_annotations_
NDIS_STATUS
FilterNetPnPEvent(
    NDIS_HANDLE              FilterModuleContext,
    PNET_PNP_EVENT_NOTIFICATION NetPnPEventNotification
    )
/*++

Routine Description:

    Net PNP event handler

Arguments:

    FilterModuleContext         - our filter context
    NetPnPEventNotification     - a Net PnP event

NOTE: called at PASSIVE_LEVEL

--*/
{
    PMS_FILTER                pFilter = (PMS_FILTER)FilterModuleContext;
    NDIS_STATUS               Status = NDIS_STATUS_SUCCESS;

    //
    // The filter may do processing on the event here, including intercepting
    // and dropping it entirely.  However, the sample does nothing with Net PNP
    // events, except pass them up to the next higher layer.  It is more
    // efficient to omit the FilterNetPnPEvent handler entirely if it does
    // nothing, but it is included in this sample for illustrative purposes.
    //

    Status = NdisFNetPnPEvent(pFilter->FilterHandle, NetPnPEventNotification);

    return Status;
}

_Use_decl_annotations_
VOID
FilterSendNetBufferListsComplete(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    ULONG               SendCompleteFlags
    )
/*++

Routine Description:

    Send complete handler

    This routine is invoked whenever the lower layer is finished processing
    sent NET_BUFFER_LISTs.  If the filter does not need to be involved in the
    send path, you should remove this routine and the FilterSendNetBufferLists
    routine.  NDIS will pass along send packets on behalf of your filter more
    efficiently than the filter can.

Arguments:

    FilterModuleContext     - our filter context
    NetBufferLists          - a chain of NBLs that are being returned to you
    SendCompleteFlags       - flags (see documentation)

Return Value:

     NONE

--*/
{
    PMS_FILTER         pFilter = (PMS_FILTER)FilterModuleContext;
    ULONG              NumOfSendCompletes = 0;
    BOOLEAN            DispatchLevel;
    PNET_BUFFER_LIST   CurrNbl;

    DEBUGP(DL_TRACE, "===>SendNBLComplete, NetBufferList: %p.\n", NetBufferLists);


    //
    // If your filter injected any send packets into the datapath to be sent,
    // you must identify their NBLs here and remove them from the chain.  Do not
    // attempt to send-complete your NBLs up to the higher layer.
    //

    //
    // If your filter has modified any NBLs (or NBs, MDLs, etc) in your
    // FilterSendNetBufferLists handler, you must undo the modifications here.
    // In general, NBLs must be returned in the same condition in which you had
    // you received them.  (Exceptions: the NBLs can be re-ordered on the linked
    // list, and the scratch fields are don't-care).
    //

    if (pFilter->TrackSends)
    {
        CurrNbl = NetBufferLists;
        while (CurrNbl)
        {
            NumOfSendCompletes++;
            CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);

        }
        DispatchLevel = NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendCompleteFlags);
        FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);
        pFilter->OutstandingSends -= NumOfSendCompletes;
        FILTER_LOG_SEND_REF(2, pFilter, PrevNbl, pFilter->OutstandingSends);
        FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
    }

    // Send complete the NBLs.  If you removed any NBLs from the chain, make
    // sure the chain isn't empty (i.e., NetBufferLists!=NULL).

    // v1: pisahkan NBL hasil inject sendiri; jangan diteruskan ke protokol
    {
        PNET_BUFFER_LIST curr = NetBufferLists;
        PNET_BUFFER_LIST prev = NULL;
        PNET_BUFFER_LIST keep = NULL;

        while (curr != NULL)
        {
            PNET_BUFFER_LIST next = NET_BUFFER_LIST_NEXT_NBL(curr);
            if (NlInjectComplete(curr))
            {
                if (prev != NULL)
                {
                    NET_BUFFER_LIST_NEXT_NBL(prev) = next;
                }
            }
            else
            {
                if (keep == NULL)
                {
                    keep = curr;
                }
                prev = curr;
            }
            curr = next;
        }

        NetBufferLists = keep;
    }

    if (NetBufferLists != NULL)
    {
        NdisFSendNetBufferListsComplete(pFilter->FilterHandle, NetBufferLists, SendCompleteFlags);
    }

    DEBUGP(DL_TRACE, "<===SendNBLComplete.\n");
}


_Use_decl_annotations_
VOID
FilterSendNetBufferLists(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    NDIS_PORT_NUMBER    PortNumber,
    ULONG               SendFlags
    )
/*++

Routine Description:

    Send Net Buffer List handler
    This function is an optional function for filter drivers. If provided, NDIS
    will call this function to transmit a linked list of NetBuffers, described by a
    NetBufferList, over the network. If this handler is NULL, NDIS will skip calling
    this filter when sending a NetBufferList and will call the next lower
    driver in the stack.  A filter that doesn't provide a FilerSendNetBufferList
    handler can not originate a send on its own.

Arguments:

    FilterModuleContext     - our filter context area
    NetBufferLists          - a List of NetBufferLists to send
    PortNumber              - Port Number to which this send is targeted
    SendFlags               - specifies if the call is at DISPATCH_LEVEL

--*/
{
    PMS_FILTER          pFilter = (PMS_FILTER)FilterModuleContext;
    PNET_BUFFER_LIST    CurrNbl;
    BOOLEAN             DispatchLevel;
    BOOLEAN             bFalse = FALSE;

    DEBUGP(DL_TRACE, "===>SendNetBufferList: NBL = %p.\n", NetBufferLists);

    do
    {

       DispatchLevel = NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags);
#if DBG
        //
        // we should never get packets to send if we are not in running state
        //

        FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);
        //
        // If the filter is not in running state, fail the send
        //
        if (pFilter->State != FilterRunning)
        {
            FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);

            CurrNbl = NetBufferLists;
            while (CurrNbl)
            {
                NET_BUFFER_LIST_STATUS(CurrNbl) = NDIS_STATUS_PAUSED;
                CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
            }
            NdisFSendNetBufferListsComplete(pFilter->FilterHandle,
                        NetBufferLists,
                        DispatchLevel ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
            break;

        }
        FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
#endif
        if (pFilter->TrackSends)
        {
            FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);
            CurrNbl = NetBufferLists;
            while (CurrNbl)
            {
                pFilter->OutstandingSends++;
                FILTER_LOG_SEND_REF(1, pFilter, CurrNbl, pFilter->OutstandingSends);

                CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
            }
            FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
        }

        //
        // If necessary, queue the NetBufferLists in a local structure for later
        // processing.  However, do not queue them for "too long", or else the
        // system's performance may be degraded.  If you need to hold onto an
        // NBL for an unbounded amount of time, then allocate memory, perform a
        // deep copy, and complete the original NBL.
        //

        // v1: hitung byte TX (passthrough tetap penuh)
        {
            PNET_BUFFER CurrNb = NET_BUFFER_LIST_FIRST_NB(NetBufferLists);
            while (CurrNb)
            {
                InterlockedAdd64(&pFilter->NlTxBytes, (LONG64)NET_BUFFER_DATA_LENGTH(CurrNb));
                CurrNb = NET_BUFFER_NEXT_NB(CurrNb);
            }
        }

        // v1: capture TX saat mode TX aktif (salin; passthrough tetap penuh)
        if ((pFilter->NlMode & NL_MODE_TX) != 0)
        {
            UCHAR CaptureBuf[NL_FRAME_MAX];
            PNET_BUFFER CurrNb = NET_BUFFER_LIST_FIRST_NB(NetBufferLists);
            while (CurrNb != NULL)
            {
                ULONG nbLen = NET_BUFFER_DATA_LENGTH(CurrNb);
                if (nbLen > 0)
                {
                    PUCHAR data;
                    if (nbLen > NL_FRAME_MAX) { nbLen = NL_FRAME_MAX; }
                    data = (PUCHAR)NdisGetDataBuffer(CurrNb, nbLen, CaptureBuf, 1, 0);
                    if (data != NULL)
                    {
                        // v1: MAC adapter = src MAC frame TX pertama
                        if (pFilter->NlMac[0] == 0 && pFilter->NlMac[1] == 0 &&
                            pFilter->NlMac[2] == 0 && pFilter->NlMac[3] == 0 &&
                            pFilter->NlMac[4] == 0 && pFilter->NlMac[5] == 0 &&
                            nbLen >= 12)
                        {
                            NdisMoveMemory(pFilter->NlMac, data + 6, 6);
                        }
                        NlTxCapture(pFilter, data, nbLen);
                    }

                }
                CurrNb = NET_BUFFER_NEXT_NB(CurrNb);
            }
        }

        // v4: policy aksi netem per-NBL (EX aktif = Blok/Internal Delay sendiri)
        if (NlGate(NL_POLICY_EN_TX) != 0)
        {
            PNET_BUFFER_LIST bl, bNext, cbl, keepHead = NULL, keepTail = NULL;
            PNET_BUFFER_LIST dropHead = NULL, dropTail = NULL;
            UCHAR PolicyBuf[NL_FRAME_MAX];
            ULONG DropCount = 0;

            for (bl = NetBufferLists; bl != NULL; )
            {
                /* simpan next DULU: pointer Next kita tumpuk nanti */
                bNext = NET_BUFFER_LIST_NEXT_NBL(bl);
                {
                    PNET_BUFFER Pnbl;
                    BOOLEAN blocked = FALSE;
                    BOOLEAN delayed = FALSE;
                    ULONG RAction = NL_ACTION_ALLOW, RDelay = 0, RIdx = NL_RULE_NONE;

                    for (Pnbl = NET_BUFFER_LIST_FIRST_NB(bl); Pnbl != NULL; Pnbl = NET_BUFFER_NEXT_NB(Pnbl))
                    {
                        ULONG Plen = NET_BUFFER_DATA_LENGTH(Pnbl);
                        if (Plen == 0 || Plen > NL_FRAME_MAX)
                        {
                            continue; /* frame >1514 B tidak dinilai (passthrough) */
                        }
                        {
                            PUCHAR pdata = (PUCHAR)NdisGetDataBuffer(Pnbl, Plen, PolicyBuf, 1, 0);
                            if (pdata != NULL)
                            {
                                if (NlPolicyDecide(pdata, Plen, NL_POLICY_EN_TX, &RIdx, &RAction, &RDelay))
                                {
                                    blocked = TRUE;
                                    break;
                                }
                                if (RAction == NL_ACTION_DELAY)
                                {
                                    delayed = TRUE;
                                    break;
                                }
                            }
                        }
                    }

                    NET_BUFFER_LIST_NEXT_NBL(bl) = NULL;
                    if (blocked)
                    {
                        DropCount++;
                        if (dropTail != NULL) { NET_BUFFER_LIST_NEXT_NBL(dropTail) = bl; }
                        else { dropHead = bl; }
                        dropTail = bl;
                    }
                    else if (delayed)
                    {
                        /* tahan NBL besar lengkap; saat due di-forward apa adanya
                         * (kualitas keputusan tetap seperti aslinya) */
                        if (NlDelayAdd(pFilter, NL_DELAY_KIND_NBL, SendFlags, 0, (PVOID)bl, NULL, RDelay))
                        {
                            /* NBL tidak masuk chain — tidak di-send sekarang */
                        }
                        else
                        {
                            /* antrean penuh: tetap drop seperti blok netem
                             * (hits sudah naik di evaluator) */
                            InterlockedIncrement64(&NlBlocked);
                            InterlockedIncrement64(&NlBlockedTx);
                            DropCount++;
                            if (dropTail != NULL) { NET_BUFFER_LIST_NEXT_NBL(dropTail) = bl; }
                            else { dropHead = bl; }
                            dropTail = bl;
                        }
                    }
                    else
                    {
                        if (keepTail != NULL) { NET_BUFFER_LIST_NEXT_NBL(keepTail) = bl; }
                        else { keepHead = bl; }
                        keepTail = bl;
                    }
                }

                bl = bNext;
            }

            if (dropHead != NULL)
            {
                /* NBL yang di-complete sendiri tidak melewati
                 * FilterSendNetBufferListsComplete lagi, jadi hitungan
                 * OutstandingSends dikoreksi di sini, dan statusnya ditandai
                 * gagal (drop policy). */
                if (pFilter->TrackSends)
                {
                    FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);
                    for (cbl = dropHead; cbl != NULL; cbl = NET_BUFFER_LIST_NEXT_NBL(cbl))
                    {
                        pFilter->OutstandingSends--;
                        FILTER_LOG_SEND_REF(2, pFilter, cbl, pFilter->OutstandingSends);
                    }
                    FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
                }

                for (cbl = dropHead; cbl != NULL; cbl = NET_BUFFER_LIST_NEXT_NBL(cbl))
                {
                    NET_BUFFER_LIST_STATUS(cbl) = NDIS_STATUS_FAILURE;
                }

                NdisFSendNetBufferListsComplete(pFilter->FilterHandle,
                                                dropHead,
                                                DispatchLevel ?
                    NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
                (VOID)DropCount;
                DEBUGP(DL_TRACE, "<===SendNBL dropped (policy).\n");
            }

            NetBufferLists = keepHead;
        }

        if (NetBufferLists != NULL)
        {
            NdisFSendNetBufferLists(pFilter->FilterHandle, NetBufferLists, PortNumber, SendFlags);
        }


    }
    while (bFalse);

    DEBUGP(DL_TRACE, "<===SendNetBufferList. \n");
}

_Use_decl_annotations_
VOID
FilterReturnNetBufferLists(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    ULONG               ReturnFlags
    )
/*++

Routine Description:

    FilterReturnNetBufferLists handler.
    FilterReturnNetBufferLists is an optional function. If provided, NDIS calls
    FilterReturnNetBufferLists to return the ownership of one or more NetBufferLists
    and their embedded NetBuffers to the filter driver. If this handler is NULL, NDIS
    will skip calling this filter when returning NetBufferLists to the underlying
    miniport and will call the next lower driver in the stack. A filter that doesn't
    provide a FilterReturnNetBufferLists handler cannot originate a receive indication
    on its own.

Arguments:

    FilterInstanceContext       - our filter context area
    NetBufferLists              - a linked list of NetBufferLists that this
                                  filter driver indicated in a previous call to
                                  NdisFIndicateReceiveNetBufferLists
    ReturnFlags                 - flags specifying if the caller is at DISPATCH_LEVEL

--*/
{
    PMS_FILTER          pFilter = (PMS_FILTER)FilterModuleContext;
    PNET_BUFFER_LIST    CurrNbl = NetBufferLists;
    UINT                NumOfNetBufferLists = 0;
    BOOLEAN             DispatchLevel;
    ULONG               Ref;

    DEBUGP(DL_TRACE, "===>ReturnNetBufferLists, NetBufferLists is %p.\n", NetBufferLists);


    //
    // If your filter injected any receive packets into the datapath to be
    // received, you must identify their NBLs here and remove them from the
    // chain.  Do not attempt to receive-return your NBLs down to the lower
    // layer.
    //

    //
    // If your filter has modified any NBLs (or NBs, MDLs, etc) in your
    // FilterReceiveNetBufferLists handler, you must undo the modifications here.
    // In general, NBLs must be returned in the same condition in which you had
    // you received them.  (Exceptions: the NBLs can be re-ordered on the linked
    // list, and the scratch fields are don't-care).
    //

    if (pFilter->TrackReceives)
    {
        while (CurrNbl)
        {
            NumOfNetBufferLists ++;
            CurrNbl = NET_BUFFER_LIST_NEXT_NBL(CurrNbl);
        }
    }


    // Return the received NBLs.  If you removed any NBLs from the chain, make
    // sure the chain isn't empty (i.e., NetBufferLists!=NULL).

    NdisFReturnNetBufferLists(pFilter->FilterHandle, NetBufferLists, ReturnFlags);

    if (pFilter->TrackReceives)
    {
        DispatchLevel = NDIS_TEST_RETURN_AT_DISPATCH_LEVEL(ReturnFlags);
        FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);

        pFilter->OutstandingRcvs -= NumOfNetBufferLists;
        Ref = pFilter->OutstandingRcvs;
        FILTER_LOG_RCV_REF(3, pFilter, NetBufferLists, Ref);
        FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
    }


    DEBUGP(DL_TRACE, "<===ReturnNetBufferLists.\n");


}


_Use_decl_annotations_
VOID
FilterReceiveNetBufferLists(
    NDIS_HANDLE         FilterModuleContext,
    PNET_BUFFER_LIST    NetBufferLists,
    NDIS_PORT_NUMBER    PortNumber,
    ULONG               NumberOfNetBufferLists,
    ULONG               ReceiveFlags
    )
/*++

Routine Description:

    FilerReceiveNetBufferLists is an optional function for filter drivers.
    If provided, this function processes receive indications made by underlying
    NIC or lower level filter drivers. This function  can also be called as a
    result of loopback. If this handler is NULL, NDIS will skip calling this
    filter when processing a receive indication and will call the next higher
    driver in the stack. A filter that doesn't provide a
    FilterReceiveNetBufferLists handler cannot provide a
    FilterReturnNetBufferLists handler and cannot a initiate an original receive
    indication on its own.

Arguments:

    FilterModuleContext      - our filter context area.
    NetBufferLists           - a linked list of NetBufferLists
    PortNumber               - Port on which the receive is indicated
    ReceiveFlags             -

N.B.: It is important to check the ReceiveFlags in NDIS_TEST_RECEIVE_CANNOT_PEND.
    This controls whether the receive indication is an synchronous or
    asynchronous function call.

--*/
{

    PMS_FILTER          pFilter = (PMS_FILTER)FilterModuleContext;
    BOOLEAN             DispatchLevel;
    ULONG               Ref;
    BOOLEAN             bFalse = FALSE;
#if DBG
    ULONG               ReturnFlags;
#endif

    DEBUGP(DL_TRACE, "===>ReceiveNetBufferList: NetBufferLists = %p.\n", NetBufferLists);
    do
    {

        DispatchLevel = NDIS_TEST_RECEIVE_AT_DISPATCH_LEVEL(ReceiveFlags);
#if DBG
        FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);

        if (pFilter->State != FilterRunning)
        {
            FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);

            if (NDIS_TEST_RECEIVE_CAN_PEND(ReceiveFlags))
            {
                ReturnFlags = 0;
                if (NDIS_TEST_RECEIVE_AT_DISPATCH_LEVEL(ReceiveFlags))
                {
                    NDIS_SET_RETURN_FLAG(ReturnFlags, NDIS_RETURN_FLAGS_DISPATCH_LEVEL);
                }

                NdisFReturnNetBufferLists(pFilter->FilterHandle, NetBufferLists, ReturnFlags);
            }
            break;
        }
        FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
#endif

        ASSERT(NumberOfNetBufferLists >= 1);

        //
        // If you would like to drop a received packet, then you must carefully
        // modify the NBL chain as follows:
        //
        //     if NDIS_TEST_RECEIVE_CANNOT_PEND(ReceiveFlags):
        //         For each NBL that is NOT dropped, temporarily unlink it from
        //         the linked list, and indicate it up alone with
        //         NdisFIndicateReceiveNetBufferLists and the
        //         NDIS_RECEIVE_FLAGS_RESOURCES flag set.  Then immediately
        //         relink the NBL back into the chain.  When all NBLs have been
        //         indicated up, you may return from this function.
        //     otherwise (NDIS_TEST_RECEIVE_CANNOT_PEND is FALSE):
        //         Divide the linked list of NBLs into two chains: one chain
        //         of packets to drop, and everything else in another chain.
        //         Return the first chain with NdisFReturnNetBufferLists, and
        //         indicate up the rest with NdisFIndicateReceiveNetBufferLists.
        //
        // Note: on the receive path for Ethernet packets, one NBL will have
        // exactly one NB.  So (assuming you are receiving on Ethernet, or are
        // attached above Native WiFi) you do not need to worry about dropping
        // one NB, but trying to indicate up the remaining NBs on the same NBL.
        // In other words, if the first NB should be dropped, drop the whole NBL.
        //

        //
        // If you would like to modify a packet, and can do so quickly, you may
        // do it here.  However, make sure you save enough information to undo
        // your modification in the FilterReturnNetBufferLists handler.
        //

        //
        // If necessary, queue the NetBufferLists in a local structure for later
        // processing.  However, do not queue them for "too long", or else the
        // system's performance may be degraded.  If you need to hold onto an
        // NBL for an unbounded amount of time, then allocate memory, perform a
        // deep copy, and return the original NBL.
        //

        if (pFilter->TrackReceives)
        {
            FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);
            pFilter->OutstandingRcvs += NumberOfNetBufferLists;
            Ref = pFilter->OutstandingRcvs;

            FILTER_LOG_RCV_REF(1, pFilter, NetBufferLists, Ref);
            FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
        }

        // v1: hitung byte RX (passthrough tetap penuh)
        {
            PNET_BUFFER_LIST CurrList;
            PNET_BUFFER CurrNb;
            for (CurrList = NetBufferLists; CurrList; CurrList = NET_BUFFER_LIST_NEXT_NBL(CurrList))
            {
                for (CurrNb = NET_BUFFER_LIST_FIRST_NB(CurrList); CurrNb; CurrNb = NET_BUFFER_NEXT_NB(CurrNb))
                {
                    InterlockedAdd64(&pFilter->NlRxBytes, (LONG64)NET_BUFFER_DATA_LENGTH(CurrNb));
                }
            }
        }

        // v1: capture RX saat mode RX aktif (salin; passthrough tetap penuh)
        if ((pFilter->NlMode & NL_MODE_RX) != 0)
        {
            UCHAR CaptureBuf[NL_FRAME_MAX];
            PNET_BUFFER_LIST rbnbl;
            for (rbnbl = NetBufferLists; rbnbl != NULL; rbnbl = NET_BUFFER_LIST_NEXT_NBL(rbnbl))
            {
                PNET_BUFFER rnb;
                for (rnb = NET_BUFFER_LIST_FIRST_NB(rbnbl); rnb != NULL; rnb = NET_BUFFER_NEXT_NB(rnb))
                {
                    ULONG nbLen = NET_BUFFER_DATA_LENGTH(rnb);
                    if (nbLen > 0)
                    {
                        PUCHAR rdata;
                        if (nbLen > NL_FRAME_MAX) { nbLen = NL_FRAME_MAX; }
                        rdata = (PUCHAR)NdisGetDataBuffer(rnb, nbLen, CaptureBuf, 1, 0);
                        if (rdata != NULL)
                        {
                            NlRxCapture(pFilter, rdata, nbLen);
                        }
                    }
                }
            }
        }

        // v4: policy drop/delay RX (Enabled bit1). Frame RESOURCES tidak bisa
        // di-drop (wajib segera diindikasikan), hanya chain CAN_PEND dipisah.
        if (NlGate(NL_POLICY_EN_RX) != 0 &&
            NDIS_TEST_RECEIVE_CAN_PEND(ReceiveFlags))
        {
            UCHAR PolicyBuf[NL_FRAME_MAX];
            PNET_BUFFER_LIST rl, rKeep = NULL, rKeepTail = NULL;
            PNET_BUFFER_LIST rDrop = NULL, rDropTail = NULL;
            ULONG DroppedCount = 0;

            for (rl = NetBufferLists; rl != NULL; )
            {
                PNET_BUFFER_LIST rNext = NET_BUFFER_LIST_NEXT_NBL(rl);
                BOOLEAN rBlocked = FALSE;
                BOOLEAN rDelayed = FALSE;
                ULONG RAction = NL_ACTION_ALLOW, RDelay = 0, RIdx = NL_RULE_NONE;

                /* ethernet: 1 NBL berisi tepat 1 NB */
                {
                    PNET_BUFFER rb = NET_BUFFER_LIST_FIRST_NB(rl);
                    if (rb != NULL)
                    {
                        ULONG rblen = NET_BUFFER_DATA_LENGTH(rb);
                        if (rblen > 0)
                        {
                            PUCHAR rbdata;
                            if (rblen > NL_FRAME_MAX) { rblen = NL_FRAME_MAX; }
                            rbdata = (PUCHAR)NdisGetDataBuffer(rb, rblen, PolicyBuf, 1, 0);
                            if (rbdata != NULL)
                            {
                                if (NlPolicyDecide(rbdata, rblen, NL_POLICY_EN_RX, &RIdx, &RAction, &RDelay))
                                {
                                    rBlocked = TRUE;
                                }
                                else if (RAction == NL_ACTION_DELAY)
                                {
                                    /* hold RX: salin bytes dan KEMBALIKAN NBL asli ke
                                     * miniport sekarang juga; saat due di-inject ulang
                                     * lewat core indicate (tak menyimpan NBL punya media). */
                                    if (NlDelayAdd(NULL, NL_DELAY_KIND_RX, 0,
                                                   rblen, NULL, rbdata, RDelay))
                                    {
                                        rDelayed = TRUE;
                                    }
                                    else
                                    {
                                        /* antrean penuh = drop netem (hits sudah naik) */
                                        InterlockedIncrement64(&NlBlocked);
                                        InterlockedIncrement64(&NlBlockedRx);
                                        rBlocked = TRUE;
                                    }
                                }
                            }
                        }
                    }
                }

                NET_BUFFER_LIST_NEXT_NBL(rl) = NULL;
                if (rBlocked || rDelayed)
                {
                    /* drop dan delay sama-sama di-RETURN ke miniport bawah */
                    DroppedCount++;
                    if (rDropTail != NULL) { NET_BUFFER_LIST_NEXT_NBL(rDropTail) = rl; }
                    else { rDrop = rl; }
                    rDropTail = rl;
                }
                else
                {
                    if (rKeepTail != NULL) { NET_BUFFER_LIST_NEXT_NBL(rKeepTail) = rl; }
                    else { rKeep = rl; }
                    rKeepTail = rl;
                }

                rl = rNext;
            }

            if (DroppedCount > 0)
            {
                ULONG ReturnFlags2 = 0;

                if (NDIS_TEST_RECEIVE_AT_DISPATCH_LEVEL(ReceiveFlags))
                {
                    NDIS_SET_RETURN_FLAG(ReturnFlags2, NDIS_RETURN_FLAGS_DISPATCH_LEVEL);
                }

                /* koreksi OutstandingRcvs untuk frame yang di-drop (tidak
                 * akan diindikasikan lagi) — digerbang sama dengan
                 * tumpang-tindih di atas. */
                if (pFilter->TrackReceives)
                {
                    FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);
                    pFilter->OutstandingRcvs -= DroppedCount;
                    FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
                }

                NumberOfNetBufferLists -= DroppedCount;
                NetBufferLists = rKeep;

                NdisFReturnNetBufferLists(pFilter->FilterHandle, rDrop, ReturnFlags2);
                DEBUGP(DL_TRACE, "<===Dropped %u recv NBL (policy).\n", DroppedCount);
            }
            else
            {
                NetBufferLists = rKeep;
            }
        }

        if (NetBufferLists != NULL)
        {
            NdisFIndicateReceiveNetBufferLists(
                       pFilter->FilterHandle,
                       NetBufferLists,
                       PortNumber,
                       NumberOfNetBufferLists,
                       ReceiveFlags);
        }


        if (NDIS_TEST_RECEIVE_CANNOT_PEND(ReceiveFlags) &&
            pFilter->TrackReceives)
        {
            FILTER_ACQUIRE_LOCK(&pFilter->Lock, DispatchLevel);
            pFilter->OutstandingRcvs -= NumberOfNetBufferLists;
            Ref = pFilter->OutstandingRcvs;
            FILTER_LOG_RCV_REF(2, pFilter, NetBufferLists, Ref);
            FILTER_RELEASE_LOCK(&pFilter->Lock, DispatchLevel);
        }

    } while (bFalse);

    DEBUGP(DL_TRACE, "<===ReceiveNetBufferList: Flags = %8x.\n", ReceiveFlags);

}


_Use_decl_annotations_
VOID
FilterCancelSendNetBufferLists(
    NDIS_HANDLE             FilterModuleContext,
    PVOID                   CancelId
    )
/*++

Routine Description:

    This function cancels any NET_BUFFER_LISTs pended in the filter and then
    calls the NdisFCancelSendNetBufferLists to propagate the cancel operation.

    If your driver does not queue any send NBLs, you may omit this routine.
    NDIS will propagate the cancelation on your behalf more efficiently.

Arguments:

    FilterModuleContext      - our filter context area.
    CancelId                 - an identifier for all NBLs that should be dequeued

Return Value:

    None

*/
{
    PMS_FILTER  pFilter = (PMS_FILTER)FilterModuleContext;

    NdisFCancelSendNetBufferLists(pFilter->FilterHandle, CancelId);
}


_Use_decl_annotations_
NDIS_STATUS
FilterSetModuleOptions(
    NDIS_HANDLE             FilterModuleContext
    )
/*++

Routine Description:

    This function set the optional handlers for the filter

Arguments:

    FilterModuleContext: The FilterModuleContext given to NdisFSetAttributes

Return Value:

    NDIS_STATUS_SUCCESS
    NDIS_STATUS_RESOURCES
    NDIS_STATUS_FAILURE

--*/
{
   PMS_FILTER                               pFilter = (PMS_FILTER)FilterModuleContext;
   NDIS_FILTER_PARTIAL_CHARACTERISTICS      OptionalHandlers;
   NDIS_STATUS                              Status = NDIS_STATUS_SUCCESS;
   BOOLEAN                                  bFalse = FALSE;

   //
   // Demonstrate how to change send/receive handlers at runtime.
   //
   if (bFalse)
   {
       UINT      i;


       pFilter->CallsRestart++;

       i = pFilter->CallsRestart % 8;

       pFilter->TrackReceives = TRUE;
       pFilter->TrackSends = TRUE;

       NdisMoveMemory(&OptionalHandlers, &DefaultChars, sizeof(OptionalHandlers));
       OptionalHandlers.Header.Type = NDIS_OBJECT_TYPE_FILTER_PARTIAL_CHARACTERISTICS;
       OptionalHandlers.Header.Size = sizeof(OptionalHandlers);
       switch (i)
       {

            case 0:
                OptionalHandlers.ReceiveNetBufferListsHandler = NULL;
                pFilter->TrackReceives = FALSE;
                break;

            case 1:

                OptionalHandlers.ReturnNetBufferListsHandler = NULL;
                pFilter->TrackReceives = FALSE;
                break;

            case 2:
                OptionalHandlers.SendNetBufferListsHandler = NULL;
                pFilter->TrackSends = FALSE;
                break;

            case 3:
                OptionalHandlers.SendNetBufferListsCompleteHandler = NULL;
                pFilter->TrackSends = FALSE;
                break;

            case 4:
                OptionalHandlers.ReceiveNetBufferListsHandler = NULL;
                OptionalHandlers.ReturnNetBufferListsHandler = NULL;
                break;

            case 5:
                OptionalHandlers.SendNetBufferListsHandler = NULL;
                OptionalHandlers.SendNetBufferListsCompleteHandler = NULL;
                break;

            case 6:

                OptionalHandlers.ReceiveNetBufferListsHandler = NULL;
                OptionalHandlers.ReturnNetBufferListsHandler = NULL;
                OptionalHandlers.SendNetBufferListsHandler = NULL;
                OptionalHandlers.SendNetBufferListsCompleteHandler = NULL;
                break;

            case 7:
                break;
       }
       Status = NdisSetOptionalHandlers(pFilter->FilterHandle, (PNDIS_DRIVER_OPTIONAL_HANDLERS)&OptionalHandlers );
   }
   return Status;
}



_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
filterDoInternalRequest(
    _In_ PMS_FILTER                   FilterModuleContext,
    _In_ NDIS_REQUEST_TYPE            RequestType,
    _In_ NDIS_OID                     Oid,
    _Inout_updates_bytes_to_(InformationBufferLength, *pBytesProcessed)
         PVOID                        InformationBuffer,
    _In_ ULONG                        InformationBufferLength,
    _In_opt_ ULONG                    OutputBufferLength,
    _In_ ULONG                        MethodId,
    _Out_ PULONG                      pBytesProcessed
    )
/*++

Routine Description:

    Utility routine that forms and sends an NDIS_OID_REQUEST to the
    miniport, waits for it to complete, and returns status
    to the caller.

    NOTE: this assumes that the calling routine ensures validity
    of the filter handle until this returns.

Arguments:

    FilterModuleContext - pointer to our filter module context
    RequestType - NdisRequest[Set|Query|method]Information
    Oid - the object being set/queried
    InformationBuffer - data for the request
    InformationBufferLength - length of the above
    OutputBufferLength  - valid only for method request
    MethodId - valid only for method request
    pBytesProcessed - place to return bytes read/written

Return Value:

    Status of the set/query request

--*/
{
    FILTER_REQUEST              FilterRequest;
    PNDIS_OID_REQUEST           NdisRequest = &FilterRequest.Request;
    NDIS_STATUS                 Status;
    BOOLEAN                     bFalse;


    bFalse = FALSE;
    *pBytesProcessed = 0;
    NdisZeroMemory(NdisRequest, sizeof(NDIS_OID_REQUEST));

    NdisInitializeEvent(&FilterRequest.ReqEvent);

    NdisRequest->Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    NdisRequest->Header.Revision = NDIS_OID_REQUEST_REVISION_1;
    NdisRequest->Header.Size = sizeof(NDIS_OID_REQUEST);
    NdisRequest->RequestType = RequestType;

    switch (RequestType)
    {
        case NdisRequestQueryInformation:
             NdisRequest->DATA.QUERY_INFORMATION.Oid = Oid;
             NdisRequest->DATA.QUERY_INFORMATION.InformationBuffer =
                                    InformationBuffer;
             NdisRequest->DATA.QUERY_INFORMATION.InformationBufferLength =
                                    InformationBufferLength;
            break;

        case NdisRequestSetInformation:
             NdisRequest->DATA.SET_INFORMATION.Oid = Oid;
             NdisRequest->DATA.SET_INFORMATION.InformationBuffer =
                                    InformationBuffer;
             NdisRequest->DATA.SET_INFORMATION.InformationBufferLength =
                                    InformationBufferLength;
            break;

        case NdisRequestMethod:
             NdisRequest->DATA.METHOD_INFORMATION.Oid = Oid;
             NdisRequest->DATA.METHOD_INFORMATION.MethodId = MethodId;
             NdisRequest->DATA.METHOD_INFORMATION.InformationBuffer =
                                    InformationBuffer;
             NdisRequest->DATA.METHOD_INFORMATION.InputBufferLength =
                                    InformationBufferLength;
             NdisRequest->DATA.METHOD_INFORMATION.OutputBufferLength = OutputBufferLength;
             break;



        default:
            FILTER_ASSERT(bFalse);
            break;
    }

    NdisRequest->RequestId = (PVOID)FILTER_REQUEST_ID;

    Status = NdisFOidRequest(FilterModuleContext->FilterHandle,
                            NdisRequest);


    if (Status == NDIS_STATUS_PENDING)
    {

        NdisWaitEvent(&FilterRequest.ReqEvent, 0);
        Status = FilterRequest.Status;
    }


    if (Status == NDIS_STATUS_SUCCESS)
    {
        if (RequestType == NdisRequestSetInformation)
        {
            *pBytesProcessed = NdisRequest->DATA.SET_INFORMATION.BytesRead;
        }

        if (RequestType == NdisRequestQueryInformation)
        {
            *pBytesProcessed = NdisRequest->DATA.QUERY_INFORMATION.BytesWritten;
        }

        if (RequestType == NdisRequestMethod)
        {
            *pBytesProcessed = NdisRequest->DATA.METHOD_INFORMATION.BytesWritten;
        }

        //
        // The driver below should set the correct value to BytesWritten
        // or BytesRead. But now, we just truncate the value to InformationBufferLength
        //
        if (RequestType == NdisRequestMethod)
        {
            if (*pBytesProcessed > OutputBufferLength)
            {
                *pBytesProcessed = OutputBufferLength;
            }
        }
        else
        {

            if (*pBytesProcessed > InformationBufferLength)
            {
                *pBytesProcessed = InformationBufferLength;
            }
        }
    }


    return Status;
}

VOID
filterInternalRequestComplete(
    _In_ NDIS_HANDLE                  FilterModuleContext,
    _In_ PNDIS_OID_REQUEST            NdisRequest,
    _In_ NDIS_STATUS                  Status
    )
/*++

Routine Description:

    NDIS entry point indicating completion of a pended NDIS_OID_REQUEST.

Arguments:

    FilterModuleContext - pointer to filter module context
    NdisRequest - pointer to NDIS request
    Status - status of request completion

Return Value:

    None

--*/
{
    PFILTER_REQUEST              FilterRequest;


    UNREFERENCED_PARAMETER(FilterModuleContext);

    //
    //  Get at the request context.
    //
    FilterRequest = CONTAINING_RECORD(NdisRequest, FILTER_REQUEST, Request);

    //
    //  Save away the completion status.
    //
    FilterRequest->Status = Status;

    //
    //  Wake up the thread blocked for this request to complete.
    //
    NdisSetEvent(&FilterRequest->ReqEvent);
}
VOID
NlRingInit(
    IN PMS_FILTER pFilter
    )
{
    ULONG Bytes = NL_RING_ENTRIES * sizeof(NL_RING_ENTRY);

    pFilter->NlTxRing = (NL_RING_ENTRY*)NdisAllocateMemoryWithTagPriority(
                                FilterDriverHandle, Bytes, FILTER_ALLOC_TAG,
                                NormalPoolPriority);
    pFilter->NlRxRing = (NL_RING_ENTRY*)NdisAllocateMemoryWithTagPriority(
                                FilterDriverHandle, Bytes, FILTER_ALLOC_TAG,
                                NormalPoolPriority);

    if (pFilter->NlTxRing == NULL || pFilter->NlRxRing == NULL)
    {
        if (pFilter->NlTxRing != NULL)
        {
            NdisFreeMemory(pFilter->NlTxRing, Bytes, 0);
            pFilter->NlTxRing = NULL;
        }
        if (pFilter->NlRxRing != NULL)
        {
            NdisFreeMemory(pFilter->NlRxRing, Bytes, 0);
            pFilter->NlRxRing = NULL;
        }
        return;
    }

    NdisZeroMemory(pFilter->NlTxRing, Bytes);
    NdisZeroMemory(pFilter->NlRxRing, Bytes);

    KeInitializeSpinLock(&pFilter->NlTxRingLock);
    KeInitializeSpinLock(&pFilter->NlRxRingLock);

    pFilter->NlTxHead = pFilter->NlTxTail = 0;
    pFilter->NlRxHead = pFilter->NlRxTail = 0;
    pFilter->NlTxDrop = pFilter->NlRxDrop = 0;
    pFilter->NlCapTx = pFilter->NlCapRx = 0;
}

VOID
NlRingUninit(
    IN PMS_FILTER pFilter
    )
{
    ULONG Bytes = NL_RING_ENTRIES * sizeof(NL_RING_ENTRY);
    if (pFilter->NlTxRing != NULL)
    {
        NdisFreeMemory(pFilter->NlTxRing, Bytes, 0);
        pFilter->NlTxRing = NULL;
    }
    if (pFilter->NlRxRing != NULL)
    {
        NdisFreeMemory(pFilter->NlRxRing, Bytes, 0);
        pFilter->NlRxRing = NULL;
    }
}
BOOLEAN
NlRingCapture(
    IN NL_RING_ENTRY* Ring,
    IN PKSPIN_LOCK Lock,
    IN volatile LONG* Head,
    IN volatile LONG* Tail,
    IN volatile ULONG* Drop,
    IN PUCHAR Data,
    IN ULONG Len,
    IN ULONG Flags
    )
{
    KIRQL OldIrql;
    LONG slot;

    if (Ring == NULL || Len == 0)
    {
        return FALSE;
    }
    if (Len > NL_FRAME_MAX)
    {
        Len = NL_FRAME_MAX;
    }

    KeAcquireSpinLock(Lock, &OldIrql);

    slot = *Head;
    if ((ULONG)((slot - *Tail) & (NL_RING_ENTRIES - 1)) >= (NL_RING_ENTRIES - 1))
    {
        KeReleaseSpinLock(Lock, OldIrql);
        InterlockedIncrement((volatile LONG*)Drop);
        return FALSE;
    }

    Ring[slot].Len = Len;
    Ring[slot].Flags = Flags;
    NdisMoveMemory(Ring[slot].Data, Data, Len);
    *Head = (*Head + 1) & (NL_RING_ENTRIES - 1);

    KeReleaseSpinLock(Lock, OldIrql);

    return TRUE;
}
ULONG
NlRingDrain(
    IN NL_RING_ENTRY* Ring,
    IN PKSPIN_LOCK Lock,
    IN volatile LONG* Head,
    IN volatile LONG* Tail,
    OUT PNL_RING_ENTRY Out,
    IN ULONG Max
    )
{
    KIRQL OldIrql;
    ULONG count = 0;

    if (Ring == NULL || Out == NULL || Max == 0)
    {
        return 0;
    }

    KeAcquireSpinLock(Lock, &OldIrql);
    while (count < Max && *Tail != *Head)
    {
        Out[count] = Ring[*Tail];
        *Tail = (*Tail + 1) & (NL_RING_ENTRIES - 1);
        count++;
    }
    KeReleaseSpinLock(Lock, OldIrql);

    return count;
}

/* ----------------------------------------------------------------
 * v2: capture helper — target internal ring vs shared-memory ring
 * ---------------------------------------------------------------- */

static VOID
NlPushTx(
    IN PMS_FILTER pFilter,
    IN PUCHAR Data,
    IN ULONG Len
    )
/*++  v6 (M5): push ring TX tanpa mirror (shm bila mapped, ring internal).
     Dipanggil <=DISPATCH_LEVEL. --*/
{
    KIRQL OldIrql;
    LONG slot;

    if (pFilter->NlShmMapped)
    {
        KeAcquireSpinLock(&pFilter->NlTxRingLock, &OldIrql);
        if (pFilter->NlShmMapped)
        {
            PNL_RING_ENTRY ring = pFilter->NlShmTxRing;
            PNL_SHM_HDR hdr = pFilter->NlShmHdr;
            ULONG mask = (NL_RING_ENTRIES - 1);

            /* kepala milik kernel (tidak bisa ditulis app); tail app tetap
             * dimask — byte dari SHM tidak dipercaya untuk indeks. */
            slot = pFilter->NlShmTxHead & (LONG)mask;
            if ((ULONG)((slot - (hdr->TxTail & (LONG)mask)) & mask) >= mask)
            {
                InterlockedIncrement((volatile LONG *)&pFilter->NlTxDrop);
            }
            else
            {
                LONG tailMasked = (LONG)hdr->TxTail & (LONG)mask;
                BOOLEAN wasEmpty = (pFilter->NlShmTxHead == tailMasked);

                ring[slot].Len = Len;
                ring[slot].Flags = NL_RING_TX;
                NdisMoveMemory(ring[slot].Data, Data, Len);
                pFilter->NlShmTxHead = (LONG)((slot + 1) & mask);
                hdr->TxHead = pFilter->NlShmTxHead;
                InterlockedIncrement64(&pFilter->NlCapTx);
                hdr->CapTxMirror = (UINT64)pFilter->NlCapTx;
                hdr->TxBytesMirror = (UINT64)pFilter->NlTxBytes;
                /* v3 M2: event coalesced — sinyal hanya saat ring kering ->
                 * berisi (awal batch) atau tiap 64 entri baru, BUKAN per
                 * paket; TxWake menjumlah berapa sinyal terkirim. */
                if (pFilter->NlShmEvent != NULL &&
                    (wasEmpty ||
                     (((ULONG)((slot + 1) - tailMasked) & mask) & 63) == 0))
                {
                    InterlockedIncrement((volatile LONG *)&hdr->TxWake);
                    KeSetEvent(pFilter->NlShmEvent, IO_NO_INCREMENT, FALSE);
                }
            }
            KeReleaseSpinLock(&pFilter->NlTxRingLock, OldIrql);
            return;
        }
        KeReleaseSpinLock(&pFilter->NlTxRingLock, OldIrql);
    }

    if (NlRingCapture(pFilter->NlTxRing, &pFilter->NlTxRingLock,
                      &pFilter->NlTxHead, &pFilter->NlTxTail,
                      &pFilter->NlTxDrop, Data, Len, NL_RING_TX))
    {
        InterlockedIncrement64(&pFilter->NlCapTx);
    }
}

VOID
NlTxCapture(
    IN PMS_FILTER pFilter,
    IN PUCHAR Data,
    IN ULONG Len
    )
/*++  Push capture TX + v6 (M5): mirror ke ring adapter target bila --
     pasangan diatur lewat MIRROR ioctl. Ref target diambil semalam
     (FilterListLock) supaya target tak bisa terbebaskan di tengah push. --*/
{
    PMS_FILTER Dst;

    NlPushTx(pFilter, Data, Len);

    Dst = NlMirrorTake(pFilter, TRUE);
    if (Dst != NULL)
    {
        NlPushTx(Dst, Data, Len);
        filterDereferenceModule(Dst);
    }
}

static VOID
NlPushRx(
    IN PMS_FILTER pFilter,
    IN PUCHAR Data,
    IN ULONG Len
    )
/*++  v6 (M5): push ring RX tanpa mirror (shm bila mapped, ring internal). --*/
{
    KIRQL OldIrql;
    LONG slot;

    if (pFilter->NlShmMapped)
    {
        KeAcquireSpinLock(&pFilter->NlTxRingLock, &OldIrql);
        if (pFilter->NlShmMapped)
        {
            PNL_RING_ENTRY ring = pFilter->NlShmRxRing;
            PNL_SHM_HDR hdr = pFilter->NlShmHdr;
            ULONG mask = (NL_RING_ENTRIES - 1);

            slot = pFilter->NlShmRxHead & (LONG)mask;
            if ((ULONG)((slot - (hdr->RxTail & (LONG)mask)) & mask) >= mask)
            {
                InterlockedIncrement((volatile LONG *)&pFilter->NlRxDrop);
            }
            else
            {
                LONG tailMasked = (LONG)hdr->RxTail & (LONG)mask;
                BOOLEAN wasEmpty = (pFilter->NlShmRxHead == tailMasked);

                ring[slot].Len = Len;
                ring[slot].Flags = NL_RING_RX;
                NdisMoveMemory(ring[slot].Data, Data, Len);
                pFilter->NlShmRxHead = (LONG)((slot + 1) & mask);
                hdr->RxHead = pFilter->NlShmRxHead;
                InterlockedIncrement64(&pFilter->NlCapRx);
                hdr->CapRxMirror = (UINT64)pFilter->NlCapRx;
                hdr->RxBytesMirror = (UINT64)pFilter->NlRxBytes;
                /* v3 M2: event coalesced arah RX (sama seperti TX). */
                if (pFilter->NlShmEvent != NULL &&
                    (wasEmpty ||
                     (((ULONG)((slot + 1) - tailMasked) & mask) & 63) == 0))
                {
                    InterlockedIncrement((volatile LONG *)&hdr->RxWake);
                    KeSetEvent(pFilter->NlShmEvent, IO_NO_INCREMENT, FALSE);
                }
            }
            KeReleaseSpinLock(&pFilter->NlTxRingLock, OldIrql);
            return;
        }
        KeReleaseSpinLock(&pFilter->NlTxRingLock, OldIrql);
    }

    if (NlRingCapture(pFilter->NlRxRing, &pFilter->NlRxRingLock,
                      &pFilter->NlRxHead, &pFilter->NlRxTail,
                      &pFilter->NlRxDrop, Data, Len, NL_RING_RX))
    {
        InterlockedIncrement64(&pFilter->NlCapRx);
    }
}

VOID
NlRxCapture(
    IN PMS_FILTER pFilter,
    IN PUCHAR Data,
    IN ULONG Len
    )
/*++  Push capture RX + v6 (M5): mirror ke ring target bila diatur. --*/
{
    PMS_FILTER Dst;

    NlPushRx(pFilter, Data, Len);

    Dst = NlMirrorTake(pFilter, FALSE);
    if (Dst != NULL)
    {
        NlPushRx(Dst, Data, Len);
        filterDereferenceModule(Dst);
    }
}

/* ----------------------------------------------------------------
 * v2: flow cache keputusan policy (direct-mapped, generasi-invalid)
 * ---------------------------------------------------------------- */

VOID
NlFlowInit(
    VOID
    )
{
    KeInitializeSpinLock(&NlFlowLock);
    NlFlowTab = (PNL_FLOW_ENT)NdisAllocateMemoryWithTagPriority(
                    FilterDriverHandle,
                    NL_FLOW_ENTRIES * sizeof(NL_FLOW_ENT),
                    FILTER_ALLOC_TAG,
                    NormalPoolPriority);
    if (NlFlowTab != NULL)
    {
        NdisZeroMemory(NlFlowTab, NL_FLOW_ENTRIES * sizeof(NL_FLOW_ENT));
    }
    /* modal: NULL berarti cache tidak aktif (semua via rule walker) */
}

VOID
NlFlowUninit(
    VOID
    )
{
    if (NlFlowTab != NULL)
    {
        NdisFreeMemory(NlFlowTab, NL_FLOW_ENTRIES * sizeof(NL_FLOW_ENT), 0);
        NlFlowTab = NULL;
    }
}

static ULONG
NlFlowHash(
    IN PNL_FLOW_KEY Key
    )
{
    UINT32 h;

    h = Key->Sip;
    h ^= Key->Dip << 3;
    h ^= (UINT32)Key->Sp << 11;
    h ^= (UINT32)Key->Dp << 19;
    h ^= (UINT32)Key->Proto << 27;
    h ^= h >> 13;
    h *= 0x9E3779B9;
    h ^= h >> 16;
    return h & NL_FLOW_MASK;
}

BOOLEAN
NlFlowProbe(
    IN PNL_FLOW_KEY Key,
    IN LONG64 Gen,
    OUT PBOOLEAN VerdictOut,
    OUT PULONG RuleIdxOut
    )
/*++  Hit-only lookup. TRUE = verdict cache (VerdictOut), FALSE = miss.
     Gen dari snapshot policy frame ini: entri policy lain tidak dihormati. --*/
{
    PNL_FLOW_ENT ent;
    BOOLEAN hit = FALSE;
    KIRQL OldIrql;

    if (NlFlowTab == NULL)
    {
        return FALSE;
    }

    KeAcquireSpinLock(&NlFlowLock, &OldIrql);
    ent = &NlFlowTab[NlFlowHash(Key)];
    if (ent->Gen == Gen &&
        ent->Key.Sip == Key->Sip && ent->Key.Dip == Key->Dip &&
        ent->Key.Sp == Key->Sp && ent->Key.Dp == Key->Dp &&
        ent->Key.Proto == Key->Proto)
    {
        *VerdictOut = (ent->Blocked != 0);
        *RuleIdxOut = (ent->Rule >= 0) ? (ULONG)ent->Rule : NL_RULE_NONE;
        hit = TRUE;
        InterlockedIncrement64(&NlFlowHit);
    }
    else
    {
        InterlockedIncrement64(&NlFlowMiss);
        *RuleIdxOut = NL_RULE_NONE;
    }
    KeReleaseSpinLock(&NlFlowLock, OldIrql);

    return hit;
}

VOID
NlFlowStore(
    IN PNL_FLOW_KEY Key,
    IN BOOLEAN Blocked,
    IN ULONG RuleIdx,
    IN LONG64 Gen
    )
/*++  Simpan verdict untuk generasi panggilan ini — TIDAK dibaca ulang dari
     NlFlowGen. Bila policy berganti setelah snapshot frame, gen baru tak
     akan cocok dan entri ini otomatis basi (tidak ada verdict tua yang
     dipublikasikan memakai generasi baru). --*/
{
    PNL_FLOW_ENT ent;
    KIRQL OldIrql;

    if (NlFlowTab == NULL)
    {
        return;
    }

    KeAcquireSpinLock(&NlFlowLock, &OldIrql);
    ent = &NlFlowTab[NlFlowHash(Key)];
    ent->Key = *Key;
    ent->Blocked = Blocked ? 1 : 0;
    ent->Rule = (RuleIdx == NL_RULE_NONE) ? -1 : (LONG)RuleIdx;
    InterlockedExchange64(&ent->Gen, Gen);
    KeReleaseSpinLock(&NlFlowLock, OldIrql);
}

/* ----------------------------------------------------------------
 * v2: shared-memory ring per adapter
 * ---------------------------------------------------------------- */

BOOLEAN
NlShmPrepare(
    IN PVOID UserVa,
    IN ULONG Len,
    IN PNL_SHM_PREP Prep
    )
/*++  PASSIVE_LEVEL, TANPA kunci apa pun: alloc MDL, lock halaman user,
      lalu map ke VA kernel. Hasilnya dititipkan di Prep; instalasi ke
      instance dikerjakan NlShmInstall (dipisah agar API MDL tidak disentuh
      di IRQL tinggi di bawah spinlock). --*/
{
    PMDL Mdl;
    PVOID Kva;

    if (Prep == NULL || UserVa == NULL || Len < NL_SHM_BYTES ||
        ((ULONG_PTR)UserVa & (PAGE_SIZE - 1)) != 0)
    {
        return FALSE;
    }
    Prep->Mdl = NULL;
    Prep->Kva = NULL;

    Mdl = IoAllocateMdl(UserVa, Len, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        return FALSE;
    }

    __try
    {
        MmProbeAndLockPages(Mdl, UserMode, IoModifyAccess);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        IoFreeMdl(Mdl);
        return FALSE;
    }

    Kva = MmMapLockedPagesSpecifyCache(Mdl, KernelMode, MmCached,
                                       NULL, FALSE, NormalPagePriority);
    if (Kva == NULL)
    {
        MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
        return FALSE;
    }

    Prep->Mdl = Mdl;
    Prep->Kva = Kva;
    return TRUE;
}

VOID
NlShmPrepareCleanup(
    IN PNL_SHM_PREP Prep
    )
/*++  Lepas hasil Prepare yang tidak terpasang (unmap VA kernel, unlock
      halaman, free MDL). --*/
{
    if (Prep == NULL || Prep->Mdl == NULL)
    {
        return;
    }
    if (Prep->Kva != NULL)
    {
        MmUnmapLockedPages(Prep->Kva, Prep->Mdl);
    }
    MmUnlockPages(Prep->Mdl);
    IoFreeMdl(Prep->Mdl);
    Prep->Mdl = NULL;
    Prep->Kva = NULL;
}

BOOLEAN
NlShmInstall(
    IN PMS_FILTER pFilter,
    IN PNL_SHM_PREP Prep,
    IN PFILE_OBJECT OwnerFile
    )
/*++  Instalasi pointer SHM instance di bawah NlTxRingLock. Re-check
      NlShmMapped (double map ditolak) dan NlDetached (instance lepas list
      tidak boleh dipetakan). --*/
{
    PNL_SHM_HDR sh;
    PNL_RING_ENTRY txr, rxr;
    KIRQL OldIrql;

    if (Prep == NULL || Prep->Mdl == NULL || Prep->Kva == NULL)
    {
        return FALSE;
    }

    sh = (PNL_SHM_HDR)Prep->Kva;
    txr = (PNL_RING_ENTRY)((PUCHAR)Prep->Kva + NL_SHM_TXRING_OFF);
    rxr = txr + NL_RING_ENTRIES;

    sh->Magic = NL_SHM_MAGIC;
    sh->Version = NL_SHM_VERSION;
    sh->AdapterIndex = pFilter->NlIdx;
    sh->Flags = NL_RING_TX | NL_RING_RX;
    sh->TxHead = sh->TxTail = 0;
    sh->RxHead = sh->RxTail = 0;
    sh->TxBytesMirror = 0;
    sh->RxBytesMirror = 0;
    sh->CapTxMirror = 0;
    sh->CapRxMirror = 0;
    sh->TxWake = 0;
    sh->RxWake = 0;

    KeAcquireSpinLock(&pFilter->NlTxRingLock, &OldIrql);
    if (pFilter->NlShmMapped || pFilter->NlDetached)
    {
        KeReleaseSpinLock(&pFilter->NlTxRingLock, OldIrql);
        return FALSE;
    }
    pFilter->NlShmMdl = Prep->Mdl;
    pFilter->NlShmKva = Prep->Kva;
    pFilter->NlShmHdr = sh;
    pFilter->NlShmTxRing = txr;
    pFilter->NlShmRxRing = rxr;
    pFilter->NlShmOwnerFile = OwnerFile;
    /* kepala ring milik kernel direset; event TIDAK disentuh (milik
     * GET_EVENT; dilepas saat unmap). */
    pFilter->NlShmTxHead = 0;
    pFilter->NlShmRxHead = 0;
    KeMemoryBarrier();
    pFilter->NlShmMapped = TRUE;
    KeReleaseSpinLock(&pFilter->NlTxRingLock, OldIrql);

    return TRUE;
}

VOID
NlShmUnmap(
    IN PMS_FILTER pFilter
    )
/*++  Lepas mapping + event. Semua swap dilakukan di bawah ring lock
      (pembaca/capture memakai kunci yang sama); Mm/Deref di luar. --*/
{
    PMDL Mdl;
    PVOID Kva;
    PKEVENT ev;
    KIRQL OldIrql;

    KeAcquireSpinLock(&pFilter->NlTxRingLock, &OldIrql);
    Mdl = pFilter->NlShmMdl;
    Kva = pFilter->NlShmKva;
    ev = pFilter->NlShmEvent;
    pFilter->NlShmMapped = FALSE;
    KeMemoryBarrier();
    pFilter->NlShmMdl = NULL;
    pFilter->NlShmKva = NULL;
    pFilter->NlShmHdr = NULL;
    pFilter->NlShmTxRing = NULL;
    pFilter->NlShmRxRing = NULL;
    pFilter->NlShmOwnerFile = NULL;
    pFilter->NlShmEvent = NULL;
    pFilter->NlShmEventFile = NULL;
    KeReleaseSpinLock(&pFilter->NlTxRingLock, OldIrql);

    if (Mdl != NULL)
    {
        if (Kva != NULL)
        {
            MmUnmapLockedPages(Kva, Mdl);
        }
        MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
    }

    if (ev != NULL)
    {
        /* ref kernel dilepas; user handle milik app menopang objeknya
         * sampai app menutup handle (Ob refcount) */
        ObDereferenceObject(ev);
    }
}

VOID
NlShmUnmapAll(
    VOID
    )
/*++  Unload: lepas semua mapping pada semua instance. --*/
{
    PLIST_ENTRY Link;
    BOOLEAN bFalse = FALSE;

    FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
    for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
    {
        PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
        NlShmUnmap(pF);
    }
    FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
}

VOID
filterReferenceModule(
    struct _MS_FILTER *pFilter
    )
/*++  Tahan lifetime konteks filter untuk jalur IOCTL (PASSIVE). RefCounter
     dikelola di bawah FilterListLock seperti daftar module itu sendiri. --*/
{
    BOOLEAN bFalse = FALSE;

    FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
    InterlockedIncrement((volatile LONG *)&pFilter->RefCount);
    FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
}

VOID
filterDereferenceModule(
    struct _MS_FILTER *pFilter
    )
/*++  Lepas referensi filterReferenceModule; pemegang terakhir membebaskan
     konteks + kunci (detach hanya menurunkan miliknya). --*/
{
    PMS_FILTER pF = (PMS_FILTER)pFilter;
    BOOLEAN bFalse = FALSE;
    BOOLEAN doFree = FALSE;

    FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
    doFree = (InterlockedDecrement((volatile LONG *)&pF->RefCount) == 0);
    FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

    if (doFree)
    {
        FILTER_FREE_LOCK(&pF->Lock);
        FILTER_FREE_MEM(pF);
    }
}

typedef struct _NL_INJ_ENTRY {
    LIST_ENTRY              List;
    PNET_BUFFER_LIST        Nbl;
    PMDL                    Mdl;
    PUCHAR                  Buf;
} NL_INJ_ENTRY, *PNL_INJ_ENTRY;

static NDIS_HANDLE    NlInjectNblPool;
static LIST_ENTRY     NlInjList;
static KSPIN_LOCK     NlInjLock;

VOID
NlInjectInit(
    VOID
    )
{
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParams;

    NdisZeroMemory(&PoolParams, sizeof(PoolParams));
    PoolParams.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParams.Header.Size = sizeof(PoolParams);
    PoolParams.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    PoolParams.fAllocateNetBuffer = TRUE;
    PoolParams.ContextSize = 0;

    NlInjectNblPool = NdisAllocateNetBufferListPool(FilterDriverHandle, &PoolParams);
    KeInitializeSpinLock(&NlInjLock);
    InitializeListHead(&NlInjList);
}

VOID
NlInjectUninit(
    VOID
    )
{
    PLIST_ENTRY Link;
    PNL_INJ_ENTRY Ent;
    KIRQL OldIrql;

    KeAcquireSpinLock(&NlInjLock, &OldIrql);
    while (!IsListEmpty(&NlInjList))
    {
        Link = RemoveHeadList(&NlInjList);
        Ent = CONTAINING_RECORD(Link, NL_INJ_ENTRY, List);
        KeReleaseSpinLock(&NlInjLock, OldIrql);
        IoFreeMdl(Ent->Mdl);
        NdisFreeMemory(Ent->Buf, NL_FRAME_MAX, 0);
        NdisFreeNetBufferList(Ent->Nbl);
        NdisFreeMemory(Ent, sizeof(NL_INJ_ENTRY), 0);
        KeAcquireSpinLock(&NlInjLock, &OldIrql);
    }
    KeReleaseSpinLock(&NlInjLock, OldIrql);

    if (NlInjectNblPool != NULL)
    {
        NdisFreeNetBufferListPool(NlInjectNblPool);
        NlInjectNblPool = NULL;
    }
}
BOOLEAN
NlInjectComplete(
    IN PNET_BUFFER_LIST Nbl
    )
{
    KIRQL           OldIrql;
    PLIST_ENTRY     Link;
    PNL_INJ_ENTRY   Ent;

    KeAcquireSpinLock(&NlInjLock, &OldIrql);
    for (Link = NlInjList.Flink; Link != &NlInjList; Link = Link->Flink)
    {
        Ent = CONTAINING_RECORD(Link, NL_INJ_ENTRY, List);
        if (Ent->Nbl == Nbl)
        {
            RemoveEntryList(Link);
            KeReleaseSpinLock(&NlInjLock, OldIrql);
            IoFreeMdl(Ent->Mdl);
            NdisFreeMemory(Ent->Buf, NL_FRAME_MAX, 0);
            NdisFreeMemory(Ent, sizeof(NL_INJ_ENTRY), 0);
            return TRUE;
        }
    }
    KeReleaseSpinLock(&NlInjLock, OldIrql);
    return FALSE;
}
static PMS_FILTER
NlFilterByIdxLocked(
    IN ULONG Idx
    )
/*++  v6 (M5): instance urutannya sama dengan ENUM (Flink ke belakang).
     Dipanggil dengan FilterListLock SUDAH dipegang. --*/
{
    PLIST_ENTRY Link;
    PMS_FILTER pF;
    ULONG i;

    for (Link = FilterModuleList.Flink, i = 0;
         Link != &FilterModuleList;
         Link = Link->Flink, ++i)
    {
        pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
        if (i == Idx)
        {
            return pF;
        }
    }
    return NULL;
}

static PMS_FILTER
NlMirrorTake(
    IN PMS_FILTER pSrc,
    IN BOOLEAN Tx
    )
/*++  v6 (M5): resolve target mirror (TX=TRUE / RX=FALSE) dan ambil satu
     referensi (snapshot + ref di bawah FilterListLock); caller wajib
     filterDereferenceModule setelah selesai memakainya. Target yang sudah
     detach di-skip. --*/
{
    PMS_FILTER Dst;
    BOOLEAN bFalse = FALSE;

    FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
    Dst = Tx ? pSrc->NlMirrorTx : pSrc->NlMirrorRx;
    if (Dst != NULL)
    {
        if (Dst->NlDetached)
        {
            Dst = NULL;
        }
        else
        {
            InterlockedIncrement((volatile LONG *)&Dst->RefCount);
        }
    }
    FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
    return Dst;
}

#define NL_MIRROR_BATCH     32

static VOID
NlMirrorDetach(
    IN PMS_FILTER pFilter
    )
/*++  v6 (M5): lepaskan semua pasangan mirror yang menyentuh instance ini
     — sebagai Src (clear pointer sendiri) maupun sebagai Dst (cari di
     list). Ref dilepas sesudah lock. Dipanggil dari FilterDetach SEBELUM
     list-ref diturunkan, jadi ref >= 1 dan deref di sini tidak pernah
     membebaskan memori instance. --*/
{
    PLIST_ENTRY Link;
    PMS_FILTER  pOther;
    BOOLEAN     bFalse = FALSE;
    PMS_FILTER  Batch[NL_MIRROR_BATCH];
    ULONG       n, i;

    do
    {
        n = 0;
        FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);

        if (pFilter->NlMirrorTx != NULL && n < NL_MIRROR_BATCH)
        {
            Batch[n++] = pFilter->NlMirrorTx;
            pFilter->NlMirrorTx = NULL;
        }
        if (pFilter->NlMirrorRx != NULL && n < NL_MIRROR_BATCH)
        {
            Batch[n++] = pFilter->NlMirrorRx;
            pFilter->NlMirrorRx = NULL;
        }

        for (Link = FilterModuleList.Flink;
             Link != &FilterModuleList;
             Link = Link->Flink)
        {
            pOther = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
            if (pOther == pFilter)
            {
                continue;
            }
            if (pOther->NlMirrorTx == pFilter && n < NL_MIRROR_BATCH)
            {
                Batch[n++] = pFilter;
                pOther->NlMirrorTx = NULL;
            }
            if (pOther->NlMirrorRx == pFilter && n < NL_MIRROR_BATCH)
            {
                Batch[n++] = pFilter;
                pOther->NlMirrorRx = NULL;
            }
        }

        FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

        for (i = 0; i < n; i++)
        {
            filterDereferenceModule(Batch[i]);
        }
    } while (n == NL_MIRROR_BATCH);
}

VOID
NlMirrorSet(
    IN const PNL_MIRROR_CFG Src
    )
/*++  v6 (M5): pasang/cabut mirror per arah. Dst di-refer (RefCount) dan
     dilepas lewat NlMirrorDetach saat salah satu sisi detach. Flags=0 atau
     DstIndex==SrcIndex menonaktifkan pasangan. --*/
{
    PMS_FILTER pSrc, pDst, pOldTx, pOldRx;
    BOOLEAN bFalse = FALSE;

    if (Src == NULL)
    {
        return;
    }

    FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
    pSrc = NlFilterByIdxLocked(Src->SrcIndex);
    pDst = (Src->Flags == 0) ? NULL : NlFilterByIdxLocked(Src->DstIndex);

    if (pSrc != NULL && pDst != NULL && pSrc == pDst)
    {
        pDst = NULL;    /* mirror ke diri sendiri ditolak */
    }

    /* lepas pasangan lama (pointer diangkat duluan, ref dilepas sesudah
     * lock — filterDereferenceModule menaikkan kunci yang sama). */
    pOldTx = NULL;
    pOldRx = NULL;
    if (pSrc != NULL)
    {
        pOldTx = pSrc->NlMirrorTx;
        pSrc->NlMirrorTx = NULL;
        pOldRx = pSrc->NlMirrorRx;
        pSrc->NlMirrorRx = NULL;

        if (pDst != NULL && pDst->State == FilterRunning)
        {
            if ((Src->Flags & NL_MIRROR_TX) != 0)
            {
                InterlockedIncrement((volatile LONG *)&pDst->RefCount);
                pSrc->NlMirrorTx = pDst;
            }
            if ((Src->Flags & NL_MIRROR_RX) != 0)
            {
                InterlockedIncrement((volatile LONG *)&pDst->RefCount);
                pSrc->NlMirrorRx = pDst;
            }
        }
    }
    FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

    if (pOldTx != NULL)
    {
        filterDereferenceModule(pOldTx);
    }
    if (pOldRx != NULL)
    {
        filterDereferenceModule(pOldRx);
    }
}

static NDIS_STATUS
NlInjectTxCore(
    IN PUCHAR Data,
    IN ULONG Len,
    IN PMS_FILTER Target
    )
/*++  Core inject TX tanpa evaluasi policy; Target=NULL = adapter pertama
     yang jalan (replay frame delay DPC). Caller (NlInjectTo) pegang ref
     target supaya instance tidak hilang di tengah. --*/
{
    PNL_INJ_ENTRY   Ent;
    PMDL            Mdl;
    PUCHAR          Buf;
    PNET_BUFFER_LIST Nbl;
    PLIST_ENTRY     Link;
    PMS_FILTER      pFilter;
    KIRQL           OldIrql;
    BOOLEAN         Sent = FALSE;
    NDIS_STATUS     Status;

    if (Len == 0 || Len > NL_FRAME_MAX || NlInjectNblPool == NULL)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    Buf = (PUCHAR)NdisAllocateMemoryWithTagPriority(FilterDriverHandle,
                                                    NL_FRAME_MAX,
                                                    FILTER_ALLOC_TAG,
                                                    NormalPoolPriority);
    if (Buf == NULL)
    {
        return NDIS_STATUS_RESOURCES;
    }
    NdisZeroMemory(Buf, NL_FRAME_MAX);
    NdisMoveMemory(Buf, Data, Len);

    Mdl = IoAllocateMdl(Buf, NL_FRAME_MAX, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        NdisFreeMemory(Buf, NL_FRAME_MAX, 0);
        return NDIS_STATUS_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(Mdl);

    Ent = (PNL_INJ_ENTRY)NdisAllocateMemoryWithTagPriority(FilterDriverHandle,
                                                           sizeof(NL_INJ_ENTRY),
                                                           FILTER_ALLOC_TAG,
                                                           NormalPoolPriority);
    if (Ent == NULL)
    {
        IoFreeMdl(Mdl);
        NdisFreeMemory(Buf, NL_FRAME_MAX, 0);
        return NDIS_STATUS_RESOURCES;
    }

    Nbl = NdisAllocateNetBufferAndNetBufferList(NlInjectNblPool, 0, 0, Mdl, 0, Len);
    if (Nbl == NULL)
    {
        NdisFreeMemory(Ent, sizeof(NL_INJ_ENTRY), 0);
        IoFreeMdl(Mdl);
        NdisFreeMemory(Buf, NL_FRAME_MAX, 0);
        return NDIS_STATUS_RESOURCES;
    }
    Nbl->SourceHandle = FilterDriverHandle;

    Ent->Nbl = Nbl;
    Ent->Mdl = Mdl;
    Ent->Buf = Buf;

    KeAcquireSpinLock(&NlInjLock, &OldIrql);
    InsertHeadList(&NlInjList, &Ent->List);
    KeReleaseSpinLock(&NlInjLock, OldIrql);

    FILTER_ACQUIRE_LOCK(&FilterListLock, FALSE);
    if (Target != NULL)
    {
        /* REDIRECT: kirim ke adapter target persis (ref dipegang caller). */
        pFilter = Target;
        if (pFilter->State == FilterRunning)
        {
            InterlockedIncrement64(&NlInj);
            NdisFSendNetBufferLists(pFilter->FilterHandle,
                                    Nbl,
                                    NDIS_DEFAULT_PORT_NUMBER,
                                    NDIS_SEND_FLAGS_DISPATCH_LEVEL);
            Sent = TRUE;
        }
    }
    else
    {
        for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
        {
            pFilter = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
            if (pFilter->State == FilterRunning)
            {
                InterlockedIncrement64(&NlInj);
                NdisFSendNetBufferLists(pFilter->FilterHandle,
                                        Nbl,
                                        NDIS_DEFAULT_PORT_NUMBER,
                                        NDIS_SEND_FLAGS_DISPATCH_LEVEL);
                Sent = TRUE;
                break;
            }
        }
    }
    FILTER_RELEASE_LOCK(&FilterListLock, FALSE);

    if (!Sent)
    {
        /* dulu: NBL tertinggal di NlInjList yang tak pernah complete →
         * bocor; kini bebas sinkron dan dilaporkan. */
        KeAcquireSpinLock(&NlInjLock, &OldIrql);
        RemoveEntryList(&Ent->List);
        KeReleaseSpinLock(&NlInjLock, OldIrql);
        NdisFreeMemory(Ent, sizeof(NL_INJ_ENTRY), 0);
        NdisFreeNetBufferList(Nbl);
        IoFreeMdl(Mdl);
        NdisFreeMemory(Buf, NL_FRAME_MAX, 0);
        Status = NDIS_STATUS_INVALID_STATE;
    }
    else
    {
        Status = NDIS_STATUS_SUCCESS;
    }

    return Status;
}

NDIS_STATUS
NlInjectTx(
    IN PUCHAR Data,
    IN ULONG Len
    )
{
    UCHAR       Pbuf[NL_FRAME_MAX];
    ULONG       Action = NL_ACTION_ALLOW, DelayMs = 0, RuleIdx = NL_RULE_NONE;

    if (Len == 0 || Len > NL_FRAME_MAX || NlInjectNblPool == NULL)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    NdisZeroMemory(Pbuf, NL_FRAME_MAX);
    NdisMoveMemory(Pbuf, Data, Len);

    if (NlPolicyDecide(Pbuf, Len, NL_POLICY_EN_TX, &RuleIdx, &Action, &DelayMs))
    {
        InterlockedIncrement64(&NlInj);  /* counter injection tetap hitung */
        return NDIS_STATUS_SUCCESS;
    }
    if (Action == NL_ACTION_DELAY)
    {
        InterlockedIncrement64(&NlInj);
        if (NlDelayAdd(NULL, NL_DELAY_KIND_TX, 0, Len, NULL, Pbuf, DelayMs))
        {
            return NDIS_STATUS_SUCCESS;
        }
        /* antrean penuh: perlakukan seperti drop netem */
        InterlockedIncrement64(&NlBlocked);
        InterlockedIncrement64(&NlBlockedTx);
        return NDIS_STATUS_SUCCESS;
    }
    return NlInjectTxCore(Data, Len, NULL);
}

NDIS_STATUS
NlInjectTo(
    IN ULONG DstIndex,
    IN PUCHAR Data,
    IN ULONG Len
    )
/*++  v6 (M5): REDIRECT — inject TX ke adapter tertentu. Resolve target
     di bawah lock + refer; kirimpun setelah release lock. Target tidak
     ada → NDIS_STATUS_INVALID_PARAMETER. --*/
{
    PMS_FILTER  pTarget = NULL;
    ULONG       Tries;
    BOOLEAN     bFalse = FALSE;
    NDIS_STATUS Status;

    if (Len == 0 || Len > NL_FRAME_MAX || NlInjectNblPool == NULL)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    /* Beberapa percobaan singkat: target bisa sedang detach. */
    for (Tries = 0; Tries < 4; Tries++)
    {
        FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
        pTarget = NlFilterByIdxLocked(DstIndex);
        if (pTarget != NULL && pTarget->State == FilterRunning)
        {
            InterlockedIncrement((volatile LONG *)&pTarget->RefCount);
        }
        else
        {
            pTarget = NULL;
        }
        FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

        if (pTarget != NULL)
        {
            break;
        }
        KeStallExecutionProcessor(1000);    /* 1 ms per percobaan */
    }
    if (pTarget == NULL)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    Status = NlInjectTxCore(Data, Len, pTarget);
    filterDereferenceModule(pTarget);
    return Status;
}

static NDIS_STATUS
NlInjectRxCore(
    IN PUCHAR Data,
    IN ULONG Len
    )
/*++  Core inject RX tanpa evaluasi policy (replay frame delay DPC).
     indicate ke stack lewat NdisFIndicateReceiveNetBufferLists pada
     adapter RUNNING pertama; RESOURCES → konsumsi sinkron bebas. --*/
{
    PMDL             Mdl;
    PUCHAR           Buf;
    PNET_BUFFER_LIST Nbl;
    PLIST_ENTRY      Link;
    PMS_FILTER       pFilter;
    BOOLEAN          Sent = FALSE;
    NDIS_STATUS      Status = NDIS_STATUS_SUCCESS;

    if (Len == 0 || Len > NL_FRAME_MAX || NlInjectNblPool == NULL)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    Buf = (PUCHAR)NdisAllocateMemoryWithTagPriority(FilterDriverHandle,
                                                    NL_FRAME_MAX,
                                                    FILTER_ALLOC_TAG,
                                                    NormalPoolPriority);
    if (Buf == NULL)
    {
        return NDIS_STATUS_RESOURCES;
    }
    NdisZeroMemory(Buf, NL_FRAME_MAX);
    NdisMoveMemory(Buf, Data, Len);

    Mdl = IoAllocateMdl(Buf, NL_FRAME_MAX, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        NdisFreeMemory(Buf, NL_FRAME_MAX, 0);
        return NDIS_STATUS_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(Mdl);

    Nbl = NdisAllocateNetBufferAndNetBufferList(NlInjectNblPool, 0, 0, Mdl, 0, Len);
    if (Nbl == NULL)
    {
        IoFreeMdl(Mdl);
        NdisFreeMemory(Buf, NL_FRAME_MAX, 0);
        return NDIS_STATUS_RESOURCES;
    }
    Nbl->SourceHandle = FilterDriverHandle;

    FILTER_ACQUIRE_LOCK(&FilterListLock, FALSE);
    for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
    {
        pFilter = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
        if (pFilter->State == FilterRunning)
        {
            NdisFIndicateReceiveNetBufferLists(pFilter->FilterHandle,
                                               Nbl,
                                               NDIS_DEFAULT_PORT_NUMBER,
                                               1,
                                               NDIS_RECEIVE_FLAGS_RESOURCES |
                                               NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
            Sent = TRUE;
            break;
        }
    }
    FILTER_RELEASE_LOCK(&FilterListLock, FALSE);

    if (!Sent)
    {
        NdisFreeNetBufferList(Nbl);
        IoFreeMdl(Mdl);
        NdisFreeMemory(Buf, NL_FRAME_MAX, 0);
        Status = NDIS_STATUS_INVALID_STATE;
    }

    return Status;
}

NDIS_STATUS
NlInjectRx(
    IN PUCHAR Data,
    IN ULONG Len
    )
{
    UCHAR       Pbuf[NL_FRAME_MAX];
    ULONG       Action = NL_ACTION_ALLOW, DelayMs = 0, RuleIdx = NL_RULE_NONE;

    if (Len == 0 || Len > NL_FRAME_MAX || NlInjectNblPool == NULL)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    NdisZeroMemory(Pbuf, NL_FRAME_MAX);
    NdisMoveMemory(Pbuf, Data, Len);

    InterlockedIncrement64(&NlInj);

    /* Evaluasi policy RX dulu: frame yang diblokir tidak masuk stack
     * (increment counter hukum dijalankan tunggal di NlPolicyDecide). */
    if (NlPolicyDecide(Pbuf, Len, NL_POLICY_EN_RX, &RuleIdx, &Action, &DelayMs))
    {
        return NDIS_STATUS_SUCCESS;
    }
    if (Action == NL_ACTION_DELAY)
    {
        if (NlDelayAdd(NULL, NL_DELAY_KIND_RX, 0, Len, NULL, Pbuf, DelayMs))
        {
            return NDIS_STATUS_SUCCESS;
        }
        /* antrean penuh: perlakukan seperti drop netem */
        InterlockedIncrement64(&NlBlocked);
        InterlockedIncrement64(&NlBlockedRx);
        return NDIS_STATUS_SUCCESS;
    }
    return NlInjectRxCore(Data, Len);
}


ULONG
NlDrainAll(
    IN  PMS_FILTER pFilter,
    OUT PNL_RING_ENTRY Out,
    IN  ULONG Max
    )
/*++
    Drain ring TX lalu RX per-filter-instance ke buffer user, terbatas Max entri.
    Flags tiap entri sudah disimpan saat capture (NL_RING_TX/RX).
--*/
{
    ULONG n;
    ULONG total = 0;

    if (Out == NULL || Max == 0 || pFilter->NlTxRing == NULL)
    {
        return 0;
    }

    n = NlRingDrain(pFilter->NlTxRing, &pFilter->NlTxRingLock, &pFilter->NlTxHead, &pFilter->NlTxTail, Out, Max);
    total += n;

    if (Max > n && pFilter->NlRxRing != NULL)
    {
        total += NlRingDrain(pFilter->NlRxRing, &pFilter->NlRxRingLock, &pFilter->NlRxHead, &pFilter->NlRxTail,
                             Out + n, Max - n);
    }

    return total;
}
static BOOLEAN
NlIpRulesBlocked(
    IN PNL_POLICY Snap,
    IN ULONG  Sip,
    IN ULONG  Dip,
    IN USHORT SrcPort,
    IN USHORT DstPort,
    IN ULONG  Proto,
    IN PULONG RuleIdxOut
    )
/*++  Block IP-rule walker terhadap SNAPSHOT policy (bukan global), tanpa
     hitungan NlBlocked (increment tepat satu titik di NlPolicyEval).
     RuleIdxOut = index rule yang match, NL_RULE_NONE bila tidak ada. --*/
{
    PNL_POLICY_RULE r;
    ULONG i;

    for (i = 0; i < Snap->Count && i < NL_POLICY_MAX_RULES; i++)
    {
        r = &Snap->Rules[i];
        if (r->Proto != 0 && r->Proto != Proto) { continue; }
        if (r->Match & NL_MATCH_SRC)
        {
            if ((Sip & r->Mask) == (r->Ip & r->Mask))
            {
                if (SrcPort == 0 || (SrcPort >= r->PortMin && SrcPort <= r->PortMax))
                {
                    *RuleIdxOut = i;
                    return TRUE;
                }
            }
        }
        if (r->Match & NL_MATCH_DST)
        {
            if ((Dip & r->Mask) == (r->Ip & r->Mask))
            {
                if (DstPort == 0 || (DstPort >= r->PortMin && DstPort <= r->PortMax))
                {
                    *RuleIdxOut = i;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

static BOOLEAN
NlDmacBlocked(
    IN PNL_POLICY Snap,
    IN PUCHAR Data,
    IN PULONG RuleIdxOut
    )
/*++  Match dst MAC ethernet (Data[0..5]) terhadap rule NL_MATCH_DSTMAC
     pada snapshot. RuleIdxOut = index rule yang match. --*/
{
    ULONG i;
    PNL_POLICY_RULE r;

    for (i = 0; i < Snap->Count && i < NL_POLICY_MAX_RULES; i++)
    {
        r = &Snap->Rules[i];
        if ((r->Match & NL_MATCH_DSTMAC) == 0)
        {
            continue;
        }
        if (Data[0] == r->DstMac[0] && Data[1] == r->DstMac[1] &&
            Data[2] == r->DstMac[2] && Data[3] == r->DstMac[3] &&
            Data[4] == r->DstMac[4] && Data[5] == r->DstMac[5])
        {
            *RuleIdxOut = i;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
NlExMatch(
    IN PNL_POLICY_RULE_EX Rules,
    IN ULONG              Count,
    IN PUCHAR             Data,
    IN ULONG              off,
    IN ULONG              proto,
    IN ULONG              sip,
    IN ULONG              dip,
    IN USHORT             SrcPort,
    IN USHORT             DstPort,
    OUT PULONG            RuleIdxOut
    )
/*++  Walker tabel EX: first-match di urutan rule; dst-MAC dipaksa
     hanya pada rule yang mencantumkan bit MAC. TANPA flow cache —
     aksi netem bisa bergantung waktu/acak per frame. --*/
{
    ULONG i;

    UNREFERENCED_PARAMETER(off);

    for (i = 0; i < Count && i < NL_POLICY_MAX_RULES; i++)
    {
        PNL_POLICY_RULE_EX r = &Rules[i];
        PNL_POLICY_RULE p = &r->R;

        if (p->Match & NL_MATCH_DSTMAC)
        {
            if (Data[0] == p->DstMac[0] && Data[1] == p->DstMac[1] &&
                Data[2] == p->DstMac[2] && Data[3] == p->DstMac[3] &&
                Data[4] == p->DstMac[4] && Data[5] == p->DstMac[5])
            {
                *RuleIdxOut = i;
                return TRUE;
            }
        }
        if (p->Match & (NL_MATCH_SRC | NL_MATCH_DST))
        {
            if (p->Proto != 0 && p->Proto != proto)
            {
                continue;
            }
            if ((p->Match & NL_MATCH_SRC) &&
                (sip & p->Mask) == (p->Ip & p->Mask))
            {
                if (SrcPort == 0 || (SrcPort >= p->PortMin && SrcPort <= p->PortMax))
                {
                    *RuleIdxOut = i;
                    return TRUE;
                }
            }
            if ((p->Match & NL_MATCH_DST) &&
                (dip & p->Mask) == (p->Ip & p->Mask))
            {
                if (DstPort == 0 || (DstPort >= p->PortMin && DstPort <= p->PortMax))
                {
                    *RuleIdxOut = i;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

BOOLEAN
NlPolicyDecide(
    IN  PUCHAR Data,
    IN  ULONG  Len,
    IN  ULONG  Dir,
    OUT PULONG RuleIdxOut,
    OUT PULONG ActionOut,
    OUT PULONG DelayMsOut
    )
/*++  v4: evaluator gabungan (EX netem / tabel lama). Dipakai TX, RX,
      dan inject. Increment counter hukum SATU titik:
      - hits rule: BLOCK, DROP_PCT kena, RATE jelang-limit, DELAY dipilih;
      - NlBlocked(Tx|Rx): HANYA keputusan drop efektif (delay menggantung,
        tidak diblok-counter; kalau antrean delay penuh, call site
        menaikkan NlBlocked sendiri). Output keputusan:
        RuleIdxOut / ActionOut / DelayMsOut (DelayMsOut hanya untuk DELAY). --*/
{
    KIRQL OldIrql;
    BOOLEAN blocked = FALSE, portsValid = TRUE;
    USHORT srcPort, dstPort;
    ULONG ipHdrLen, proto, sip, dip, off;
    ULONG RuleIdx = NL_RULE_NONE;
    ULONG Action = NL_ACTION_ALLOW;
    ULONG DelayMs = 0;

    if (NlGate(Dir) == 0 || Len < 34)
    {
        *RuleIdxOut = RuleIdx;
        *ActionOut = Action;
        *DelayMsOut = DelayMs;
        return FALSE;
    }

    /* ----- v5 (M4): program VM (rule terkompilasi) — mengganti SEMUA tabel
     * saat aktif. Decode ret: ACTION | (Param << 8); hit program masuk
     * NlRuleHits[0]; DROP_PCT/RATE pinjam slot netem 15. --*/
    if (InterlockedCompareExchange(&NlVmActive, 0, 0) != 0)
    {
        ULONG VmRet;

        if (NlVmRun(Data, Len, &VmRet))
        {
            ULONG Act = VmRet & 0xFF;
            ULONG Param = VmRet >> 8;
            switch (Act)
            {
                case NL_ACTION_BLOCK:
                {
                    InterlockedIncrement64(&NlRuleHits[0]);
                    InterlockedIncrement64(&NlBlocked);
                    if (Dir == NL_POLICY_EN_RX)
                    {
                        InterlockedIncrement64(&NlBlockedRx);
                    }
                    else
                    {
                        InterlockedIncrement64(&NlBlockedTx);
                    }
                    blocked = TRUE;
                    Action = NL_ACTION_BLOCK;
                    break;
                }
                case NL_ACTION_DELAY:
                {
                    ULONG delay = (Param > 60000UL) ? 60000UL : Param;

                    if (delay != 0)
                    {
                        InterlockedIncrement64(&NlRuleHits[0]);
                        Action = NL_ACTION_DELAY;
                        DelayMs = delay;
                    }
                    break;
                }
                case NL_ACTION_DROP_PCT:
                {
                    ULONG s = NlRngSeed[NL_POLICY_MAX_RULES - 1];

                    s = s * 1103515245UL + 12345UL;
                    NlRngSeed[NL_POLICY_MAX_RULES - 1] = s;
                    if (Param > 0 && ((s >> 16) & 0xFFFF) % 100UL < Param)
                    {
                        InterlockedIncrement64(&NlRuleHits[0]);
                        InterlockedIncrement64(&NlBlocked);
                        if (Dir == NL_POLICY_EN_RX)
                        {
                            InterlockedIncrement64(&NlBlockedRx);
                        }
                        else
                        {
                            InterlockedIncrement64(&NlBlockedTx);
                        }
                        blocked = TRUE;
                        Action = NL_ACTION_BLOCK;
                    }
                    break;
                }
                case NL_ACTION_RATE:
                {
                    const ULONG Vs = NL_POLICY_MAX_RULES - 1;
                    LARGE_INTEGER Now, Freq;
                    ULONG64 rateBps, burstCap, elapsed;

                    rateBps = (ULONG64)Param * 128ULL;
                    if (Param == 0)
                    {
                        /* "0 = tanpa limit": melewati saja */
                        break;
                    }

                    /* burst = kapasitas 1 detik token */
                    burstCap = rateBps;

                    Now = KeQueryPerformanceCounter(&Freq);

                    elapsed = (ULONG64)Now.QuadPart - (ULONG64)NlRateLast[Vs].QuadPart;
                    /* QPC tick → token: tick * rateBPS / tick-per-detik */
                    if (Freq.QuadPart != 0)
                    {
                        elapsed = elapsed * rateBps / (ULONG64)Freq.QuadPart;
                    }
                    else
                    {
                        elapsed = 0;
                    }

                    NlRateTok[Vs] += elapsed;
                    if (NlRateTok[Vs] > burstCap)
                    {
                        NlRateTok[Vs] = burstCap;
                    }
                    NlRateLast[Vs].QuadPart = Now.QuadPart;

                    if (NlRateTok[Vs] >= Len)
                    {
                        NlRateTok[Vs] -= Len;
                    }
                    else
                    {
                        InterlockedIncrement64(&NlRuleHits[0]);
                        InterlockedIncrement64(&NlBlocked);
                        if (Dir == NL_POLICY_EN_RX)
                        {
                            InterlockedIncrement64(&NlBlockedRx);
                        }
                        else
                        {
                            InterlockedIncrement64(&NlBlockedTx);
                        }
                        blocked = TRUE;
                        Action = NL_ACTION_BLOCK;
                    }
                    break;
                }
                default:
                    /* NL_ACTION_ALLOW (0) + aksi tidak dikenal = passthrough */
                    break;
            }
        }
        *RuleIdxOut = RuleIdx;
        *ActionOut = Action;
        *DelayMsOut = DelayMs;
        return blocked;
    }

    if (NlExEnabled == 0)
    {
        /* ----- tabel policy lama (kompat; counter identik versi M1) ----- */
        NL_POLICY Snap;
        LONG64 Gen;

        /* snapshot konsisten: policy + generasi dibawa kunci yang sama */
        KeAcquireSpinLock(&NlFlowLock, &OldIrql);
        Snap = NlPolicy;
        Gen = NlFlowGen;
        KeReleaseSpinLock(&NlFlowLock, OldIrql);

        if (Snap.Enabled == 0 || ((Snap.Enabled & Dir) == 0))
        {
            *RuleIdxOut = RuleIdx;
            *ActionOut = Action;
            *DelayMsOut = DelayMs;
            return FALSE;
        }
        if (Data[12] != 0x08 || Data[13] != 0x00)
        {
            *RuleIdxOut = RuleIdx;
            *ActionOut = Action;
            *DelayMsOut = DelayMs;
            return FALSE; /* tabel lama hanya IPv4; non-IP hanya utk EX/VM */
        }

        off = 14;
        proto = Data[off + 9];
        ipHdrLen = (ULONG)(Data[off] & 0x0F) * 4;
        if (ipHdrLen < 20 || Len < off + ipHdrLen)
        {
            *RuleIdxOut = RuleIdx;
            *ActionOut = Action;
            *DelayMsOut = DelayMs;
            return FALSE;
        }
        sip = *(ULONG *)&Data[off + 12];
        dip = *(ULONG *)&Data[off + 16];
        srcPort = dstPort = 0;
        if (proto == 6 || proto == 17)
        {
            /* jangan baca port kalau truncasi: guard OOB + jangan cache flow
             * untuk frame berpotongan (port nol punya makna "tanpa port"). */
            if (Len >= off + ipHdrLen + 4)
            {
                srcPort = (USHORT)((USHORT)Data[off + ipHdrLen] << 8 | Data[off + ipHdrLen + 1]);
                dstPort = (USHORT)((USHORT)Data[off + ipHdrLen + 2] << 8 | Data[off + ipHdrLen + 3]);
            }
            else
            {
                portsValid = FALSE;
            }
        }

        if (NlDmacBlocked(&Snap, Data, &RuleIdx))
        {
            blocked = TRUE;
        }
        else if (NlFlowTab != NULL && (proto == 6UL || proto == 17UL) && portsValid)
        {
            NL_FLOW_KEY key;
            BOOLEAN verdict;

            key.Sip = sip;
            key.Dip = dip;
            key.Sp = srcPort;
            key.Dp = dstPort;
            key.Proto = (UCHAR)proto;
            key.Pad = 0;

            if (NlFlowProbe(&key, Gen, &verdict, &RuleIdx))
            {
                blocked = verdict;
            }
            else
            {
                verdict = NlIpRulesBlocked(&Snap, sip, dip, srcPort, dstPort, proto, &RuleIdx);
                NlFlowStore(&key, verdict, RuleIdx, Gen);
                blocked = verdict;
            }
        }
        else
        {
            /* proto bukan TCP/UDP, frame berpotongan, atau cache mati: IP-only */
            blocked = NlIpRulesBlocked(&Snap, sip, dip, srcPort, dstPort, proto, &RuleIdx);
        }

        if (blocked)
        {
            Action = NL_ACTION_BLOCK;
            if (RuleIdx < NL_POLICY_MAX_RULES)
            {
                InterlockedIncrement64(&NlRuleHits[RuleIdx]);
            }
            InterlockedIncrement64(&NlBlocked);
            if (Dir == NL_POLICY_EN_RX)
            {
                InterlockedIncrement64(&NlBlockedRx);
            }
            else
            {
                InterlockedIncrement64(&NlBlockedTx);
            }
        }
        *RuleIdxOut = RuleIdx;
        *ActionOut = Action;
        *DelayMsOut = DelayMs;
        return blocked;
    }

    /* ----- tabel EX (aksi netem) — parse header dulu KHUSUS non-IP bisa
     * match rule dst-MAC murni, jadi parse header dilakukan di dalam
     * blok match masing-masing. --*/
    {
        BOOLEAN matched = FALSE;

        off = 14;
        ipHdrLen = 0;
        proto = 0;
        sip = dip = 0;
        srcPort = dstPort = 0;

        KeAcquireSpinLock(&NlFlowLock, &OldIrql);
        if ((NlPolicyEx.Enabled & Dir) == 0)
        {
            KeReleaseSpinLock(&NlFlowLock, OldIrql);
            *RuleIdxOut = RuleIdx;
            *ActionOut = Action;
            *DelayMsOut = DelayMs;
            return FALSE;
        }

        /* dst-MAC bisa match tanpa header IP — cek dulu terhadap kandidat
         * sebelum parse IPv4 dipakai. Loop pertama: rule MAC; loop kedua
         * (bila ada header IPv4): IP-rule. */
        if (NlExMatch(NlPolicyEx.Rules, NlPolicyEx.Count, Data, off, 0, 0, 0, 0, 0, &RuleIdx) &&
            (NlPolicyEx.Rules[RuleIdx].R.Match & NL_MATCH_DSTMAC))
        {
            matched = TRUE;
        }

        if (!matched && Data[12] == 0x08 && Data[13] == 0x00)
        {
            proto = Data[off + 9];
            ipHdrLen = (ULONG)(Data[off] & 0x0F) * 4;
            if (ipHdrLen >= 20 && Len >= off + ipHdrLen)
            {
                sip = *(ULONG *)&Data[off + 12];
                dip = *(ULONG *)&Data[off + 16];
                if (proto == 6 || proto == 17)
                {
                    if (Len >= off + ipHdrLen + 4)
                    {
                        srcPort = (USHORT)((USHORT)Data[off + ipHdrLen] << 8 | Data[off + ipHdrLen + 1]);
                        dstPort = (USHORT)((USHORT)Data[off + ipHdrLen + 2] << 8 | Data[off + ipHdrLen + 3]);
                    }
                    else
                    {
                        portsValid = FALSE;
                    }
                }
                if (portsValid)
                {
                    if (NlExMatch(NlPolicyEx.Rules, NlPolicyEx.Count, Data, off, proto,
                                  sip, dip, srcPort, dstPort, &RuleIdx))
                    {
                        matched = TRUE;
                    }
                }
            }
        }

        if (matched && RuleIdx < NL_POLICY_MAX_RULES && RuleIdx < NlPolicyEx.Count)
        {
            PNL_POLICY_RULE_EX r = &NlPolicyEx.Rules[RuleIdx];

            switch (r->Action)
            {
                case NL_ACTION_BLOCK:
                {
                    InterlockedIncrement64(&NlRuleHits[RuleIdx]);
                    InterlockedIncrement64(&NlBlocked);
                    if (Dir == NL_POLICY_EN_RX)
                    {
                        InterlockedIncrement64(&NlBlockedRx);
                    }
                    else
                    {
                        InterlockedIncrement64(&NlBlockedTx);
                    }
                    blocked = TRUE;
                    Action = NL_ACTION_BLOCK;
                    break;
                }
                case NL_ACTION_DELAY:
                {
                    ULONG delay;
                    delay = r->DelayMs;
                    if (delay > 60000)
                    {
                        delay = 60000;
                    }
                    if (delay == 0)
                    {
                        /* delay tanpa durasi = passthrough saja */
                        break;
                    }
                    InterlockedIncrement64(&NlRuleHits[RuleIdx]);
                    Action = NL_ACTION_DELAY;
                    DelayMs = delay;
                    break;
                }
                case NL_ACTION_DROP_PCT:
                {
                    ULONG s = NlRngSeed[RuleIdx];

                    s = s * 1103515245UL + 12345UL;
                    NlRngSeed[RuleIdx] = s;
                    if (r->DropPct > 0 &&
                        ((s >> 16) & 0xFFFF) % 100UL < r->DropPct)
                    {
                        InterlockedIncrement64(&NlRuleHits[RuleIdx]);
                        InterlockedIncrement64(&NlBlocked);
                        if (Dir == NL_POLICY_EN_RX)
                        {
                            InterlockedIncrement64(&NlBlockedRx);
                        }
                        else
                        {
                            InterlockedIncrement64(&NlBlockedTx);
                        }
                        blocked = TRUE;
                        Action = NL_ACTION_BLOCK;
                    }
                    break;
                }
                case NL_ACTION_RATE:
                {
                    LARGE_INTEGER Now, Freq;
                    ULONG64 rateBps = (ULONG64)r->RateKbps * 128ULL; /* KBps→B/s */
                    ULONG64 burstCap, elapsed;

                    if (r->RateKbps == 0)
                    {
                        /* "0 = tanpa limit": melewati saja */
                        break;
                    }

                    /* burst = kapasitas 1 detik token */
                    burstCap = rateBps;

                    Now = KeQueryPerformanceCounter(&Freq);

                    elapsed = (ULONG64)Now.QuadPart - (ULONG64)NlRateLast[RuleIdx].QuadPart;
                    /* QPC tick → token: tick * rateBPS / tick-per-detik */
                    if (Freq.QuadPart != 0)
                    {
                        elapsed = elapsed * rateBps / (ULONG64)Freq.QuadPart;
                    }
                    else
                    {
                        elapsed = 0;
                    }

                    NlRateTok[RuleIdx] += elapsed;
                    if (NlRateTok[RuleIdx] > burstCap)
                    {
                        NlRateTok[RuleIdx] = burstCap;
                    }
                    NlRateLast[RuleIdx].QuadPart = Now.QuadPart;

                    if (NlRateTok[RuleIdx] >= Len)
                    {
                        NlRateTok[RuleIdx] -= Len;
                    }
                    else
                    {
                        /* jelang token: drop (bisa juga queue-out, tapi
                         * legacy filter tidak punya modus ini — drop standar) */
                        InterlockedIncrement64(&NlRuleHits[RuleIdx]);
                        InterlockedIncrement64(&NlBlocked);
                        if (Dir == NL_POLICY_EN_RX)
                        {
                            InterlockedIncrement64(&NlBlockedRx);
                        }
                        else
                        {
                            InterlockedIncrement64(&NlBlockedTx);
                        }
                        blocked = TRUE;
                        Action = NL_ACTION_BLOCK;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        KeReleaseSpinLock(&NlFlowLock, OldIrql);
    }

    *RuleIdxOut = RuleIdx;
    *ActionOut = Action;
    *DelayMsOut = DelayMs;
    return blocked;
}

BOOLEAN
NlPolicyEval(
    IN PUCHAR Data,
    IN ULONG  Len,
    IN ULONG  Dir
    )
/*++  Wrapper kompat: TRUE = frame drop (BLOCK/drop netem). --*/
{
    ULONG RuleIdx, Action, DelayMs;

    return NlPolicyDecide(Data, Len, Dir, &RuleIdx, &Action, &DelayMs);
}

VOID
NlPolicyExSet(
    IN const PNL_POLICY_EX Src
    )
/*++  Pasang tabel EX (aksi netem) di bawah NlFlowLock: reset RNG dan
     token bucket (burst=1 detik) per rule, naikkan generasi flow cache
     (enter EX = tabel lama tidak dipakai), lalu publish gate EX. --*/
{
    KIRQL OldIrql;
    ULONG Ex;
    ULONG i;

    if (Src == NULL)
    {
        return;
    }
    Ex = (Src->Count > 0 && Src->Count <= NL_POLICY_MAX_RULES) ? (Src->Enabled ? Src->Enabled : NL_POLICY_EN_TX) : 0;

    KeAcquireSpinLock(&NlFlowLock, &OldIrql);
    NdisZeroMemory(&NlPolicyEx, sizeof(NlPolicyEx));
    if (Ex != 0)
    {
        NdisMoveMemory(&NlPolicyEx, Src, sizeof(NlPolicyEx));
    }
    for (i = 0; i < NL_POLICY_MAX_RULES; i++)
    {
        NlRngSeed[i] = 0x586C8801UL ^ (i * 7919UL + (ULONG)NlFlowGen);
        NlRateTok[i] = (NlPolicyEx.Rules[i].Action == NL_ACTION_RATE &&
                        NlPolicyEx.Rules[i].RateKbps != 0)
                       ? (ULONG64)NlPolicyEx.Rules[i].RateKbps * 128ULL
                       : 0;
        NlRateLast[i].QuadPart = KeQueryPerformanceCounter(NULL).QuadPart;
        }
    /* flow cache basi: setelah ini (EX mati) verdict lama tidak lolos */
    InterlockedIncrement64(&NlFlowGen);
    KeReleaseSpinLock(&NlFlowLock, OldIrql);

    InterlockedExchange(&NlExEnabled, (LONG)Ex);
}

VOID
NlVmSet(
    IN const PNL_VM_PROG Src
    )
/*++  Publish program VM: Count=0 mematikan (kembali ke tabel EX/lama).
     Program di-copy oalah NlFlowLock agar evaluator baca atomik. --*/
{
    KIRQL OldIrql;
    ULONG En;

    if (Src == NULL)
    {
        return;
    }
    if (Src->Count > NL_VM_MAX_INSNS)
    {
        return;
    }
    En = (Src->Count > 0) ? (Src->Enabled ? Src->Enabled : NL_POLICY_EN_TX) : 0;

    KeAcquireSpinLock(&NlFlowLock, &OldIrql);
    NdisZeroMemory(&NlVm, sizeof(NlVm));
    if (En != 0)
    {
        NdisMoveMemory(&NlVm, Src, sizeof(NlVm));
    }
    /* kalau VM mati, flow cache boleh dirajang ulang; selalu generasi+ */
    InterlockedIncrement64(&NlFlowGen);
    KeReleaseSpinLock(&NlFlowLock, OldIrql);

    InterlockedExchange(&NlVmActive, (LONG)(En != 0));
    InterlockedExchange(&NlVmEn, (LONG)En);
}

static ULONG
NlVmLoad16(
    IN const PUCHAR Data,
    IN ULONG Len,
    IN ULONG Off
    )
/*++  Load 16-bit big-endian; OOB = 0. --*/
{
    if (Off + 1 >= Len)
    {
        return 0;
    }
    return ((ULONG)Data[Off] << 8) | Data[Off + 1];
}

static ULONG
NlVmLoad32(
    IN const PUCHAR Data,
    IN ULONG Len,
    IN ULONG Off
    )
/*++  Load 32-bit big-endian; OOB = 0. --*/
{
    if (Off + 3 >= Len)
    {
        return 0;
    }
    return ((ULONG)Data[Off] << 24) | ((ULONG)Data[Off + 1] << 16) |
           ((ULONG)Data[Off + 2] << 8) | (ULONG)Data[Off + 3];
}

static BOOLEAN
NlVmRun(
    IN const PUCHAR Data,
    IN ULONG Len,
    OUT PULONG RetOut
    )
/*++  Evaluator BPF-c klasik: register A/X, scratch M[16]. Program di-copy
     ke stack (snapshot di bawah NlFlowLock — tidak pernah membaca struct
     selagi NlVmSet menimpanya). Loads OOB = 0; pembagian nol = 0; lompatan
     keluar program = ALLOW. Return TRUE dgn *RetOut = ACTION | (Param << 8). --*/
{
    NL_VM_INSN Ctx[NL_VM_MAX_INSNS];
    KIRQL OldIrql;
    ULONG A = 0, X = 0;
    ULONG M[NL_VM_SCRATCH];
    ULONG Count, i, Code;
    int steps;

    *RetOut = NL_ACTION_ALLOW;
    if (InterlockedCompareExchange(&NlVmActive, 0, 0) == 0)
    {
        return FALSE;
    }
    if (Data == NULL)
    {
        return FALSE;
    }

    KeAcquireSpinLock(&NlFlowLock, &OldIrql);
    Count = NlVm.Count;
    if (Count != 0 && Count <= NL_VM_MAX_INSNS)
    {
        NdisMoveMemory(Ctx, NlVm.Insns, Count * sizeof(NL_VM_INSN));
    }
    KeReleaseSpinLock(&NlFlowLock, OldIrql);
    if (Count == 0)
    {
        return FALSE;
    }

    RtlZeroMemory(M, sizeof(M));

    for (i = 0, steps = 0; i < Count && steps++ < 100000; ++i)
    {
        Code = Ctx[i].Code;

        switch (Code & 0x07U)   /* class */
        {
            case NL_VM_CL_LD:
            case NL_VM_CL_LDX:
            {
                ULONG Val;
                BOOLEAN ToX = ((Code & 0x07U) == NL_VM_CL_LDX);

                if ((Code & NL_VM_MD_LEN) != 0)
                {
                    Val = Len;
                }
                else if ((Code & NL_VM_MD_MEM) == NL_VM_MD_MEM)
                {
                    if (Ctx[i].K >= NL_VM_SCRATCH)
                    {
                        *RetOut = NL_ACTION_ALLOW;
                        return TRUE;
                    }
                    Val = M[Ctx[i].K];
                }
                else if ((Code & NL_VM_MD_ABS) != 0)
                {
                    ULONG Off = Ctx[i].K;
                    if ((Code & NL_VM_MD_IND) != 0)
                    {
                        Off += X;
                    }
                    if ((Code & NL_VM_SZ_B) != 0)
                    {
                        Val = (Off < Len) ? (ULONG)Data[Off] : 0;
                    }
                    else if ((Code & NL_VM_SZ_H) != 0)
                    {
                        Val = NlVmLoad16(Data, Len, Off);
                    }
                    else
                    {
                        Val = NlVmLoad32(Data, Len, Off);
                    }
                }
                else    /* MD_IMM */
                {
                    Val = Ctx[i].K;
                }

                if (ToX)
                {
                    X = Val;
                }
                else
                {
                    A = Val;
                }
                break;
            }
            case NL_VM_CL_ST:
                if (Ctx[i].K < NL_VM_SCRATCH)
                {
                    M[Ctx[i].K] = A;
                }
                break;
            case NL_VM_CL_STX:
                if (Ctx[i].K < NL_VM_SCRATCH)
                {
                    M[Ctx[i].K] = X;
                }
                break;
            case NL_VM_CL_ALU:
            {
                ULONG Op = Code & 0xF8U;
                ULONG Src = ((Code & NL_VM_SRC_X) != 0) ? X : Ctx[i].K;

                switch (Op)
                {
                    case NL_VM_OP_ADD: A += Src; break;
                    case NL_VM_OP_SUB: A -= Src; break;
                    case NL_VM_OP_MUL: A *= Src; break;
                    case NL_VM_OP_DIV: A = (Src != 0) ? (A / Src) : 0; break;
                    case NL_VM_OP_OR:  A |= Src; break;
                    case NL_VM_OP_AND: A &= Src; break;
                    case NL_VM_OP_LSH: A <<= (Src & 31); break;
                    case NL_VM_OP_RSH: A >>= (Src & 31); break;
                    case NL_VM_OP_NEG: A = (ULONG)(-(LONG)A); break;
                    default: break;
                }
                break;
            }
            case NL_VM_CL_JMP:
            {
                ULONG Op = Code & 0xF8U;
                ULONG Src = ((Code & NL_VM_SRC_X) != 0) ? X : Ctx[i].K;
                BOOLEAN Take = FALSE;

                if (Op == NL_VM_JA)
                {
                    i += Ctx[i].Jt;
                }
                else switch (Op)
                {
                    case NL_VM_JEQ:  Take = (A == Src); break;
                    case NL_VM_JGT:  Take = (A > Src); break;
                    case NL_VM_JGE:  Take = (A >= Src); break;
                    case NL_VM_JSET: Take = ((A & Src) != 0); break;
                    default: *RetOut = NL_ACTION_ALLOW; return TRUE;
                }
                if (Op != NL_VM_JA)
                {
                    i += Take ? Ctx[i].Jt : Ctx[i].Jf;
                }
                if (i >= Count)
                {
                    /* jatuh keluar program: ALLOW */
                    return TRUE;
                }
                continue;
            }
            case NL_VM_CL_RET:
            {
                ULONG V = ((Code & NL_VM_RTK) != 0) ? Ctx[i].K : A;
                *RetOut = V;
                return TRUE;
            }
            case NL_VM_CL_MISC:
            default:
                if (Code == NL_VM_TAX)
                {
                    X = A;
                }
                else if (Code == NL_VM_TXA)
                {
                    A = X;
                }
                break;
        }
    }

    *RetOut = NL_ACTION_ALLOW;
    return TRUE;
}

VOID
NlPolicySet(
    IN PNL_POLICY Src
    )
/*++  Pasang policy baru di bawah NlFlowLock lalu naikkan generasi: pembaca
     lain (snapshot evaluator, probe, store) di kunci yang sama, jadi tidak
     ada verdict lama yang dikaitkan generasi baru dan sebaliknya. --*/
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&NlFlowLock, &OldIrql);
    NdisZeroMemory(&NlPolicy, sizeof(NlPolicy));
    if (Src->Count <= NL_POLICY_MAX_RULES)
    {
        NdisMoveMemory(&NlPolicy, Src, sizeof(NlPolicy));
    }
    /* semua entri flow cache jadi basi (gen mismatch). */
    InterlockedIncrement64(&NlFlowGen);
    KeReleaseSpinLock(&NlFlowLock, OldIrql);
}
