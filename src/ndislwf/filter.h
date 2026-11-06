/*++

Copyright (c) Microsoft Corporation

Module Name:

    Filter.h

Abstract:

    This module contains all prototypes and macros for filter code.

Notes:

--*/
#ifndef _FILT_H
#define _FILT_H

#pragma warning(disable:28930) // Unused assignment of pointer, by design in samples
#pragma warning(disable:28931) // Unused assignment of variable, by design in samples

// TODO: Customize these to hint at your component for memory leak tracking.
// These should be treated like a pooltag.
#define FILTER_REQUEST_ID          'QWPN'
#define FILTER_ALLOC_TAG           'FLtN'
#define FILTER_TAG                 'NLtW'

// TODO: Specify which version of the NDIS contract you will use here.
// In many cases, 6.0 is the best choice.  You only need to select a later
// version if you need a feature that is not available in 6.0.
//
// Legal values include:
//    6.0  Available starting with Windows Vista RTM
//    6.1  Available starting with Windows Vista SP1 / Windows Server 2008
//    6.20 Available starting with Windows 7 / Windows Server 2008 R2
//    6.30 Available starting with Windows 8 / Windows Server "8"
// Or, just use NDIS_FILTER_MAJOR_VERSION / NDIS_FILTER_MINOR_VERSION
// to pick up whatever version is defined by your build system
// (for example, "-DNDIS630").
#define FILTER_MAJOR_NDIS_VERSION   NDIS_FILTER_MAJOR_VERSION
#define FILTER_MINOR_NDIS_VERSION   NDIS_FILTER_MINOR_VERSION


//
// Global variables
//
extern NDIS_HANDLE         FilterDriverHandle; // NDIS handle for filter driver
extern NDIS_HANDLE         FilterDriverObject;
extern NDIS_HANDLE         NdisFilterDeviceHandle;
extern PDEVICE_OBJECT      NdisDeviceObject;

extern FILTER_LOCK         FilterListLock;
extern LIST_ENTRY          FilterModuleList;

struct _MS_FILTER;
VOID        NlRingInit(struct _MS_FILTER *pFilter);
BOOLEAN     NlPolicyEval(PUCHAR Data, ULONG Len, ULONG Dir);
BOOLEAN     NlPolicyDecide(PUCHAR Data, ULONG Len, ULONG Dir,
                            PULONG RuleIdxOut, PULONG ActionOut, PULONG DelayMsOut);
VOID        NlPolicyExSet(const PNL_POLICY_EX Src);
/* v5 (M4): prog BPF klasik; Count=0 kembali ke tabel EX/lama */
VOID        NlVmSet(const PNL_VM_PROG Src);
/* v6 (M5): mirror TX/RX ke adapter target + inject arah TX ke target */
VOID        NlMirrorSet(const PNL_MIRROR_CFG Src);
struct _MS_FILTER *NlFilterByIdxLocked(IN ULONG Idx);
NDIS_STATUS NlInjectTo(ULONG DstIndex, PUCHAR Data, ULONG Len);
VOID        NlDelayInit(VOID);
VOID        NlDelayDrain(struct _MS_FILTER *pFilter);
VOID        NlDelayUnload(VOID);
VOID        NlTxCapture(struct _MS_FILTER *pFilter, PUCHAR Data, ULONG Len);
VOID        NlRxCapture(struct _MS_FILTER *pFilter, PUCHAR Data, ULONG Len);
VOID        NlPolicySet(const PNL_POLICY Src);
/* v6/M6 (debug): snapshot state evaluasi */
VOID        NlDebugDump(PNL_DEBUG Out);
VOID        NlRingUninit(struct _MS_FILTER *pFilter);
VOID        NlInjectInit(VOID);
VOID        NlInjectUninit(VOID);
BOOLEAN     NlInjectComplete(PNET_BUFFER_LIST Nbl);
NDIS_STATUS NlInjectTx(PUCHAR Data, ULONG Len);
NDIS_STATUS NlInjectRx(PUCHAR Data, ULONG Len);
VOID        NlFlowInit(VOID);
VOID        NlFlowUninit(VOID);
/* v2: shared-memory ring.
 * NlShmPrepare dipanggil PASSIVE tanpa kunci apa pun (lock halaman user
 * + map kernel); NlShmInstall memasang hasilnya di bawah ring lock, dan
 * NlShmPrepareCleanup merapikan prep yang gagal dipasang. */
typedef struct _NL_SHM_PREP
{
    PMDL    Mdl;
    PVOID   Kva;
} NL_SHM_PREP, *PNL_SHM_PREP;

BOOLEAN     NlShmPrepare(PVOID UserVa, ULONG Len, PNL_SHM_PREP Prep);
BOOLEAN     NlShmInstall(struct _MS_FILTER *pFilter, PNL_SHM_PREP Prep, PFILE_OBJECT OwnerFile);
VOID        NlShmPrepareCleanup(PNL_SHM_PREP Prep);
VOID        NlShmUnmap(struct _MS_FILTER *pFilter);
VOID        NlShmUnmapAll(void);
VOID        filterReferenceModule(struct _MS_FILTER *pFilter);
VOID        filterDereferenceModule(struct _MS_FILTER *pFilter);
ULONG       NlDrainAll(struct _MS_FILTER *pFilter, PNL_RING_ENTRY Out, ULONG Max);
BOOLEAN     NlRingCapture(NL_RING_ENTRY* Ring,
                          PKSPIN_LOCK Lock,
                          volatile LONG* Head,
                          volatile LONG* Tail,
                          volatile ULONG* Drop,
                          PUCHAR Data,
                          ULONG Len,
                          ULONG Flags);
ULONG       NlRingDrain(NL_RING_ENTRY* Ring,
                         PKSPIN_LOCK Lock,
                         volatile LONG* Head,
                         volatile LONG* Tail,
                         PNL_RING_ENTRY Out,
                         ULONG Max);

extern volatile LONG64     NlInj;
extern volatile LONG64     NlBlocked;
/* v3: block per arah + hit tertinggal per rule (index sesuai rule policy) */
extern volatile LONG64     NlBlockedTx;
extern volatile LONG64     NlBlockedRx;
extern volatile LONG64     NlRuleHits[NL_POLICY_MAX_RULES];
/* v4: item yang sudah matang keluar antrean delay netem */
extern volatile LONG64     NlDelayFired;
/* v4: item yang masih menunggu due di antrean delay */
extern volatile LONG64     NlDelayCount;

extern LONG                NlInstanceCounter;




#if NDISLWF
#define FILTER_FRIENDLY_NAME        L"NDIS LightWeight Filter"
// TODO: Customize this to match the GUID in the INF
#define FILTER_UNIQUE_NAME          L"{6c5cde28-e3eb-419a-bed4-02c6451371fd}" //unique name, quid name
// TODO: Customize this to match the service name in the INF
#define FILTER_SERVICE_NAME         L"NDISLWF"
//
// The filter needs to handle IOCTLs
//
#define LINKNAME_STRING             L"\\DosDevices\\NDISLWF"
#define NTDEVICE_STRING             L"\\Device\\NDISLWF"
#endif


#if 0   /* varian sample lama (NDISLWF1) dinonaktifkan — kita pakai blok atas */
#define FILTER_FRIENDLY_NAME        L"NDIS LightWeight Filter"
#define FILTER_UNIQUE_NAME          L"{5cbf81be-5055-47cd-9055-a76b2b4e3697}" //unique name, quid name
#define FILTER_SERVICE_NAME         L"NDISLWF"
//
// The filter needs to handle IOCTRLs
//
#define LINKNAME_STRING             L"\\DosDevices\\NDISLWF"
#define NTDEVICE_STRING             L"\\Device\\NDISLWF"
#endif

#if NDISMON
#define FILTER_FRIENDLY_NAME        L"NDIS Sample Monitor LightWeight Filter"
#define FILTER_UNIQUE_NAME          L"{5cbf81bf-5055-47cd-9055-a76b2b4e3697}" //unique name, quid name
#define FILTER_SERVICE_NAME         L"NDISMON"
//
// The filter needs to handle IOCTRLs
//
#define LINKNAME_STRING             L"\\DosDevices\\NDISMON"
#define NTDEVICE_STRING             L"\\Device\\NDISMON"
#endif

#if NDISMON1
#define FILTER_FRIENDLY_NAME        L"NDIS Sample Monitor 1 LightWeight Filter"
#define FILTER_UNIQUE_NAME          L"{5cbf81c0-5055-47cd-9055-a76b2b4e3697}" //unique name, quid name
#define FILTER_SERVICE_NAME         L"NDISMON1"
//
// The filter needs to handle IOCTRLs
//
#define LINKNAME_STRING             L"\\DosDevices\\NDISMON1"
#define NTDEVICE_STRING             L"\\Device\\NDISMON1"
#endif


//
// Types and macros to manipulate packet queue
//
typedef struct _QUEUE_ENTRY
{
    struct _QUEUE_ENTRY * Next;
}QUEUE_ENTRY, *PQUEUE_ENTRY;

typedef struct _QUEUE_HEADER
{
    PQUEUE_ENTRY     Head;
    PQUEUE_ENTRY     Tail;
} QUEUE_HEADER, *PQUEUE_HEADER;


#if TRACK_RECEIVES
UINT         filterLogReceiveRefIndex = 0;
ULONG_PTR    filterLogReceiveRef[0x10000];
#endif

#if TRACK_SENDS
UINT         filterLogSendRefIndex = 0;
ULONG_PTR    filterLogSendRef[0x10000];
#endif

#if TRACK_RECEIVES
#define   FILTER_LOG_RCV_REF(_O, _Instance, _NetBufferList, _Ref)    \
    {\
        filterLogReceiveRef[filterLogReceiveRefIndex++] = (ULONG_PTR)(_O); \
        filterLogReceiveRef[filterLogReceiveRefIndex++] = (ULONG_PTR)(_Instance); \
        filterLogReceiveRef[filterLogReceiveRefIndex++] = (ULONG_PTR)(_NetBufferList); \
        filterLogReceiveRef[filterLogReceiveRefIndex++] = (ULONG_PTR)(_Ref); \
        if (filterLogReceiveRefIndex >= (0x10000 - 5))                    \
        {                                                              \
            filterLogReceiveRefIndex = 0;                                 \
        }                                                              \
    }
#else
#define   FILTER_LOG_RCV_REF(_O, _Instance, _NetBufferList, _Ref)
#endif

#if TRACK_SENDS
#define   FILTER_LOG_SEND_REF(_O, _Instance, _NetBufferList, _Ref)    \
    {\
        filterLogSendRef[filterLogSendRefIndex++] = (ULONG_PTR)(_O); \
        filterLogSendRef[filterLogSendRefIndex++] = (ULONG_PTR)(_Instance); \
        filterLogSendRef[filterLogSendRefIndex++] = (ULONG_PTR)(_NetBufferList); \
        filterLogSendRef[filterLogSendRefIndex++] = (ULONG_PTR)(_Ref); \
        if (filterLogSendRefIndex >= (0x10000 - 5))                    \
        {                                                              \
            filterLogSendRefIndex = 0;                                 \
        }                                                              \
    }

#else
#define   FILTER_LOG_SEND_REF(_O, _Instance, _NetBufferList, _Ref)
#endif


//
// DEBUG related macros.
//
#if DBG
#define FILTER_ALLOC_MEM(_NdisHandle, _Size)    \
    filterAuditAllocMem(                        \
            _NdisHandle,                        \
           _Size,                               \
           __FILENUMBER,                        \
           __LINE__);

#define FILTER_FREE_MEM(_pMem)                  \
    filterAuditFreeMem(_pMem);

#else
#define FILTER_ALLOC_MEM(_NdisHandle, _Size)     \
    NdisAllocateMemoryWithTagPriority(_NdisHandle, _Size, FILTER_ALLOC_TAG, LowPoolPriority)

#define FILTER_FREE_MEM(_pMem)    NdisFreeMemory(_pMem, 0, 0)

#endif //DBG

#if DBG_SPIN_LOCK
#define FILTER_INIT_LOCK(_pLock)                          \
    filterAllocateSpinLock(_pLock, __FILENUMBER, __LINE__)

#define FILTER_FREE_LOCK(_pLock)       filterFreeSpinLock(_pLock)


#define FILTER_ACQUIRE_LOCK(_pLock, DispatchLevel)  \
    filterAcquireSpinLock(_pLock, __FILENUMBER, __LINE__, DisaptchLevel)

#define FILTER_RELEASE_LOCK(_pLock, DispatchLevel)      \
    filterReleaseSpinLock(_pLock, __FILENUMBER, __LINE__, DispatchLevel)

#else
#define FILTER_INIT_LOCK(_pLock)      NdisAllocateSpinLock(_pLock)

#define FILTER_FREE_LOCK(_pLock)      NdisFreeSpinLock(_pLock)

#define FILTER_ACQUIRE_LOCK(_pLock, DispatchLevel)              \
    {                                                           \
        if (DispatchLevel)                                      \
        {                                                       \
            NdisDprAcquireSpinLock(_pLock);                     \
        }                                                       \
        else                                                    \
        {                                                       \
            NdisAcquireSpinLock(_pLock);                        \
        }                                                       \
    }

#define FILTER_RELEASE_LOCK(_pLock, DispatchLevel)              \
    {                                                           \
        if (DispatchLevel)                                      \
        {                                                       \
            NdisDprReleaseSpinLock(_pLock);                     \
        }                                                       \
        else                                                    \
        {                                                       \
            NdisReleaseSpinLock(_pLock);                        \
        }                                                       \
    }
#endif //DBG_SPIN_LOCK


#define NET_BUFFER_LIST_LINK_TO_ENTRY(_pNBL)    ((PQUEUE_ENTRY)(NET_BUFFER_LIST_NEXT_NBL(_pNBL)))
#define ENTRY_TO_NET_BUFFER_LIST(_pEnt)         (CONTAINING_RECORD((_pEnt), NET_BUFFER_LIST, Next))

#define InitializeQueueHeader(_QueueHeader)             \
{                                                       \
    (_QueueHeader)->Head = (_QueueHeader)->Tail = NULL; \
}

//
// Macros for queue operations
//
#define IsQueueEmpty(_QueueHeader)      ((_QueueHeader)->Head == NULL)

#define RemoveHeadQueue(_QueueHeader)                   \
    (_QueueHeader)->Head;                               \
    {                                                   \
        PQUEUE_ENTRY pNext;                             \
        ASSERT((_QueueHeader)->Head);                   \
        pNext = (_QueueHeader)->Head->Next;             \
        (_QueueHeader)->Head = pNext;                   \
        if (pNext == NULL)                              \
            (_QueueHeader)->Tail = NULL;                \
    }

#define InsertHeadQueue(_QueueHeader, _QueueEntry)                  \
    {                                                               \
        ((PQUEUE_ENTRY)(_QueueEntry))->Next = (_QueueHeader)->Head; \
        (_QueueHeader)->Head = (PQUEUE_ENTRY)(_QueueEntry);         \
        if ((_QueueHeader)->Tail == NULL)                           \
            (_QueueHeader)->Tail = (PQUEUE_ENTRY)(_QueueEntry);     \
    }

#define InsertTailQueue(_QueueHeader, _QueueEntry)                      \
    {                                                                   \
        ((PQUEUE_ENTRY)(_QueueEntry))->Next = NULL;                     \
        if ((_QueueHeader)->Tail)                                       \
            (_QueueHeader)->Tail->Next = (PQUEUE_ENTRY)(_QueueEntry);   \
        else                                                            \
            (_QueueHeader)->Head = (PQUEUE_ENTRY)(_QueueEntry);         \
        (_QueueHeader)->Tail = (PQUEUE_ENTRY)(_QueueEntry);             \
    }


//
// Enum of filter's states
// Filter can only be in one state at one time
//
typedef enum _FILTER_STATE
{
    FilterStateUnspecified,
    FilterInitialized,
    FilterPausing,
    FilterPaused,
    FilterRunning,
    FilterRestarting,
    FilterDetaching
} FILTER_STATE;


typedef struct _FILTER_REQUEST
{
    NDIS_OID_REQUEST       Request;
    NDIS_EVENT             ReqEvent;
    NDIS_STATUS            Status;
} FILTER_REQUEST, *PFILTER_REQUEST;

//
// Define the filter struct
//
typedef struct _MS_FILTER
{
    LIST_ENTRY                     FilterModuleLink;
    //Reference to this filter
    ULONG                           RefCount;

    NDIS_HANDLE                     FilterHandle;
    NDIS_STRING                     FilterModuleName;
    NDIS_STRING                     MiniportFriendlyName;
    NDIS_STRING                     MiniportName;
    NET_IFINDEX                     MiniportIfIndex;

    NDIS_STATUS                     Status;
    NDIS_EVENT                      Event;
    ULONG                           BackFillSize;
    FILTER_LOCK                     Lock;    // Lock for protection of state and outstanding sends and recvs

    FILTER_STATE                    State;   // Which state the filter is in
    ULONG                           OutstandingSends;
    ULONG                           OutstandingRequest;
    ULONG                           OutstandingRcvs;
    FILTER_LOCK                     SendLock;
    FILTER_LOCK                     RcvLock;
    QUEUE_HEADER                    SendNBLQueue;
    QUEUE_HEADER                    RcvNBLQueue;


    NDIS_STRING                     FilterName;
    ULONG                           CallsRestart;
    BOOLEAN                         TrackReceives;
    BOOLEAN                         TrackSends;
#if DBG
    BOOLEAN                         bIndicating;
#endif

    PNDIS_OID_REQUEST               PendingOidRequest;

    /* v1: state ringan per-filter-instance (ada per adapter).
     * counter_increment via InterlockedAdd64 — IRQL dpc-safe.
     * mode flags: NL_MODE_RX|NL_MODE_TX (SET_MODE dari user-mode) */
    volatile LONG64                 NlRxBytes;
    volatile LONG64                 NlTxBytes;
    volatile UINT32                 NlMode;
    volatile UINT32                 NlIdx;    /* ordinal adapter stabil sesi */
    UINT32                          NlMedium; /* NdisMedium802_3 = 0 */
    UINT8                           NlMac[6];
    UINT8                           NlMacPad[2];

    /* v1: ring capture per-adapter (dialokasi saat attach, dibebaskan saat detach).
     * 1024 entri x 1524 B = 1 562 624 B per arah; dua arah = 3 128 448 B/adapter */
    PNL_RING_ENTRY                  NlTxRing;
    PNL_RING_ENTRY                  NlRxRing;
    volatile LONG                   NlTxHead;
    volatile LONG                   NlTxTail;
    volatile LONG                   NlRxHead;
    volatile LONG                   NlRxTail;
    volatile ULONG                  NlTxDrop;
    volatile ULONG                  NlRxDrop;
    volatile LONG64                 NlCapTx;
    volatile LONG64                 NlCapRx;
    KSPIN_LOCK                      NlTxRingLock;
    KSPIN_LOCK                      NlRxRingLock;

    /* v2: shared-memory ring (zero-copy). Jika NlShmMapped, capture TX/RX
     * menulis langsung ke buffer usermode; ring internal menunggu dengan
     * menganggur. Semua akses pointer target dijaga NlTxRingLock/NlRxRingLock. */
    BOOLEAN                         NlShmMapped;
    BOOLEAN                         NlDetached;   /* TRUE bila sudah lepas list */
    PMDL                            NlShmMdl;
    PVOID                           NlShmKva;
    PNL_SHM_HDR                     NlShmHdr;
    PNL_RING_ENTRY                  NlShmTxRing;
    PNL_RING_ENTRY                  NlShmRxRing;
    PKEVENT                         NlShmEvent;   /* SignalEvent saat ada capture shm */
    PFILE_OBJECT                    NlShmEventFile;   /* pemilik handle GET_EVENT */
    PFILE_OBJECT                    NlShmOwnerFile;
    /* kepala ring SHM milik kernel (app hanya menulis tail) */
    volatile LONG                   NlShmTxHead;
    volatile LONG                   NlShmRxHead;

    /* v6 (M5): mirror per arah — target instance menerima copy frame
     * setelah push ring sendiri; hidup oleh NlMirrorSet/MIRROR ioctl.
     * Referensi lifetime via filterReferenceModule (dipegang target). */
    struct _MS_FILTER*              NlMirrorTx;
    struct _MS_FILTER*              NlMirrorRx;

}MS_FILTER, * PMS_FILTER;


typedef struct _FILTER_DEVICE_EXTENSION
{
    ULONG            Signature;
    NDIS_HANDLE      Handle;
} FILTER_DEVICE_EXTENSION, *PFILTER_DEVICE_EXTENSION;


#define FILTER_READY_TO_PAUSE(_Filter)      \
    ((_Filter)->State == FilterPausing)

//
// The driver should maintain a list of NDIS filter handles
//
typedef struct _FL_NDIS_FILTER_LIST
{
    LIST_ENTRY              Link;
    NDIS_HANDLE             ContextHandle;
    NDIS_STRING             FilterInstanceName;
} FL_NDIS_FILTER_LIST, *PFL_NDIS_FILTER_LIST;

//
// The context inside a cloned request
//
typedef struct _NDIS_OID_REQUEST *FILTER_REQUEST_CONTEXT,**PFILTER_REQUEST_CONTEXT;


//
// function prototypes
//
DRIVER_INITIALIZE DriverEntry;

FILTER_SET_OPTIONS FilterRegisterOptions;

FILTER_ATTACH FilterAttach;

FILTER_DETACH FilterDetach;

DRIVER_UNLOAD FilterUnload;

FILTER_RESTART FilterRestart;

FILTER_PAUSE FilterPause;

FILTER_OID_REQUEST FilterOidRequest;

FILTER_CANCEL_OID_REQUEST FilterCancelOidRequest;

FILTER_STATUS FilterStatus;

FILTER_DEVICE_PNP_EVENT_NOTIFY FilterDevicePnPEventNotify;

FILTER_NET_PNP_EVENT FilterNetPnPEvent;

FILTER_OID_REQUEST_COMPLETE FilterOidRequestComplete;

FILTER_SEND_NET_BUFFER_LISTS FilterSendNetBufferLists;

FILTER_RETURN_NET_BUFFER_LISTS FilterReturnNetBufferLists;

FILTER_SEND_NET_BUFFER_LISTS_COMPLETE FilterSendNetBufferListsComplete;

FILTER_RECEIVE_NET_BUFFER_LISTS FilterReceiveNetBufferLists;

FILTER_CANCEL_SEND_NET_BUFFER_LISTS FilterCancelSendNetBufferLists;

FILTER_SET_MODULE_OPTIONS FilterSetModuleOptions;


_IRQL_requires_max_(PASSIVE_LEVEL)
NDIS_STATUS
FilterRegisterDevice(
    VOID
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
FilterDeregisterDevice(
    VOID
    );

DRIVER_DISPATCH FilterDispatch;

DRIVER_DISPATCH FilterDeviceIoControl;

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
    );

VOID
filterInternalRequestComplete(
    _In_ NDIS_HANDLE                  FilterModuleContext,
    _In_ PNDIS_OID_REQUEST            NdisRequest,
    _In_ NDIS_STATUS                  Status
    );


#endif  //_FILT_H


