//
//    Copyright (C) Microsoft Corporation.  All rights reserved.
//
#ifndef __FILTERUSER_H__
#define __FILTERUSER_H__

//
// Kontrak kontrol user-mode <-> driver (v1)
//

#define _NDIS_CONTROL_CODE(request,method) \
            CTL_CODE(FILE_DEVICE_PHYSICAL_NETCARD, request, method, FILE_ANY_ACCESS)

#define IOCTL_FILTER_RESTART_ALL            _NDIS_CONTROL_CODE(0, METHOD_BUFFERED)
#define IOCTL_FILTER_RESTART_ONE_INSTANCE   _NDIS_CONTROL_CODE(1, METHOD_BUFFERED)
#define IOCTL_FILTER_ENUERATE_ALL_INSTANCES _NDIS_CONTROL_CODE(2, METHOD_BUFFERED)
#define IOCTL_FILTER_ENUMERATE_ALL_INSTANCES IOCTL_FILTER_ENUERATE_ALL_INSTANCES
#define IOCTL_FILTER_QUERY_ALL_STAT         _NDIS_CONTROL_CODE(3, METHOD_BUFFERED)
#define IOCTL_FILTER_CLEAR_ALL_STAT         _NDIS_CONTROL_CODE(4, METHOD_BUFFERED)
#define IOCTL_FILTER_SET_OID_VALUE          _NDIS_CONTROL_CODE(5, METHOD_BUFFERED)
#define IOCTL_FILTER_QUERY_OID_VALUE        _NDIS_CONTROL_CODE(6, METHOD_BUFFERED)
#define IOCTL_FILTER_CANCEL_REQUEST         _NDIS_CONTROL_CODE(7, METHOD_BUFFERED)
#define IOCTL_FILTER_READ_DRIVER_CONFIG     _NDIS_CONTROL_CODE(8, METHOD_BUFFERED)
#define IOCTL_FILTER_WRITE_DRIVER_CONFIG    _NDIS_CONTROL_CODE(9, METHOD_BUFFERED)
#define IOCTL_FILTER_READ_ADAPTER_CONFIG    _NDIS_CONTROL_CODE(10, METHOD_BUFFERED)
#define IOCTL_FILTER_WRITE_ADAPTER_CONFIG   _NDIS_CONTROL_CODE(11, METHOD_BUFFERED)
#define IOCTL_FILTER_READ_INSTANCE_CONFIG   _NDIS_CONTROL_CODE(12, METHOD_BUFFERED)
#define IOCTL_FILTER_WRITE_INSTANCE_CONFIG  _NDIS_CONTROL_CODE(13, METHOD_BUFFERED)


#define MAX_FILTER_INSTANCE_NAME_LENGTH     256
#define MAX_FILTER_CONFIG_KEYWORD_LENGTH    256
typedef struct _FILTER_DRIVER_ALL_STAT
{
    ULONG          AttachCount;
    ULONG          DetachCount;
    ULONG          ExternalRequestFailedCount;
    ULONG          ExternalRequestSuccessCount;
    ULONG          InternalRequestFailedCount;
} FILTER_DRIVER_ALL_STAT, * PFILTER_DRIVER_ALL_STAT;


typedef struct _FILTER_SET_OID
{
    WCHAR           InstanceName[MAX_FILTER_INSTANCE_NAME_LENGTH];
    ULONG           InstanceNameLength;
    NDIS_OID        Oid;
    NDIS_STATUS     Status;
    UCHAR           Data[sizeof(ULONG)];

}FILTER_SET_OID, *PFILTER_SET_OID;

typedef struct _FILTER_QUERY_OID
{
    WCHAR           InstanceName[MAX_FILTER_INSTANCE_NAME_LENGTH];
    ULONG           InstanceNameLength;
    NDIS_OID        Oid;
    NDIS_STATUS     Status;
    UCHAR           Data[sizeof(ULONG)];

}FILTER_QUERY_OID, *PFILTER_QUERY_OID;

typedef struct _FILTER_READ_CONFIG
{
    _Field_size_bytes_part_(MAX_FILTER_INSTANCE_NAME_LENGTH,InstanceNameLength)
    WCHAR                   InstanceName[MAX_FILTER_INSTANCE_NAME_LENGTH];
    ULONG                   InstanceNameLength;
    _Field_size_bytes_part_(MAX_FILTER_CONFIG_KEYWORD_LENGTH,KeywordLength)
    WCHAR                   Keyword[MAX_FILTER_CONFIG_KEYWORD_LENGTH];
    ULONG                   KeywordLength;
    NDIS_PARAMETER_TYPE     ParameterType;
    NDIS_STATUS             Status;
    UCHAR                   Data[sizeof(ULONG)];
}FILTER_READ_CONFIG, *PFILTER_READ_CONFIG;

typedef struct _FILTER_WRITE_CONFIG
{
    _Field_size_bytes_part_(MAX_FILTER_INSTANCE_NAME_LENGTH,InstanceNameLength)
    WCHAR                   InstanceName[MAX_FILTER_INSTANCE_NAME_LENGTH];
    ULONG                   InstanceNameLength;
    _Field_size_bytes_part_(MAX_FILTER_CONFIG_KEYWORD_LENGTH,KeywordLength)
    WCHAR                   Keyword[MAX_FILTER_CONFIG_KEYWORD_LENGTH];
    ULONG                   KeywordLength;
    NDIS_PARAMETER_TYPE     ParameterType;
    NDIS_STATUS             Status;
    UCHAR                   Data[sizeof(ULONG)];
}FILTER_WRITE_CONFIG, *PFILTER_WRITE_CONFIG;


/* ----------------------------------------------------------------
 * Blok kontrak kontrol v1 - device 0x8900
 * CTL_CODE: dev<<16 | acc<<14 | fn<<2 | met
 * ---------------------------------------------------------------- */
#define NL_DEV_TYPE   0x8900
#define NL_CODE(fn)   ((NL_DEV_TYPE << 16) | (FILE_ANY_ACCESS << 14) | ((fn) << 2) | METHOD_BUFFERED)

#define NL_IOCTL_VERSION    NL_CODE(0x800)
#define NL_IOCTL_ENUM_ADPT  NL_CODE(0x801)
#define NL_IOCTL_SET_MODE   NL_CODE(0x802)
#define NL_IOCTL_STATS      NL_CODE(0x803)
#define NL_IOCTL_READ_RING  NL_CODE(0x804)
#define NL_IOCTL_INJECT     NL_CODE(0x805)
#define NL_IOCTL_CLEAR      NL_CODE(0x806)
#define NL_IOCTL_POLICY     NL_CODE(0x807)

#define NL_ABI_MAGIC   0x31504C4Eu   /* 'NLP1' */
#define NL_ABI_VERSION ((1UL << 16) | 0UL)

#define NL_FLAG_RUNNING   0x1
#define NL_MODE_RX        0x1
#define NL_MODE_TX        0x2

typedef struct _NL_VERSION {
    ULONG   Magic;
    ULONG   Version;
} NL_VERSION, *PNL_VERSION;

#define NL_IFNAME_MAX 260
#define NL_MAX_ADAPTERS 64

typedef struct _NL_ADAPTER {
    ULONG   Index;
    ULONG   Medium;
    ULONG   Flags;         /* NL_FLAG_RUNNING */
    ULONG   Reserved;
    UCHAR   Mac[6];
    UCHAR   Pad2[2];
    WCHAR   IfName[NL_IFNAME_MAX]; /* UTF-16, null-terminated */
} NL_ADAPTER, *PNL_ADAPTER;

typedef struct _NL_MODE {
    ULONG   AdapterIndex;
    ULONG   Flags;         /* NL_MODE_RX | NL_MODE_TX */
} NL_MODE, *PNL_MODE;


/* ----------------------------------------------------------------
 * Ring capture/inject v1
 * ---------------------------------------------------------------- */
#define NL_FRAME_MAX        1514
#define NL_RING_ENTRIES     1024
#define NL_RING_TX          0x1
#define NL_RING_RX          0x2

typedef struct _NL_RING_ENTRY {
    ULONG   Len;            /* byte efektif di Data */
    ULONG   Flags;          /* NL_RING_TX dari TX path, NL_RING_RX dari RX path */
    UCHAR   Data[NL_FRAME_MAX];
} NL_RING_ENTRY, *PNL_RING_ENTRY;

typedef struct _NL_INJECT {
    ULONG   Len;
    ULONG   Reserved;
    UCHAR   Data[NL_FRAME_MAX];
} NL_INJECT, *PNL_INJECT;

/* ----------------------------------------------------------------
 * Policy block TX v1
 * ---------------------------------------------------------------- */
#define NL_POLICY_MAX_RULES 16
#define NL_PROTO_ANY        0
#define NL_PROTO_TCP        6
#define NL_PROTO_UDP        17
#define NL_MATCH_SRC        0x1
#define NL_MATCH_DST        0x2
#define NL_MATCH_DSTMAC     0x4

typedef struct _NL_POLICY_RULE {
    ULONG   Proto;      /* 6/17/0=any */
    ULONG   Match;      /* NL_MATCH_* (bitwise) */
    ULONG   Ip;         /* network byte order */
    ULONG   Mask;
    USHORT  PortMin;
    USHORT  PortMax;
    UCHAR   DstMac[6];
} NL_POLICY_RULE, *PNL_POLICY_RULE;

typedef struct _NL_POLICY {
    ULONG   Count;      /* <= NL_POLICY_MAX_RULES */
    ULONG   Enabled;    /* global: atur drop TX */
    NL_POLICY_RULE Rules[NL_POLICY_MAX_RULES];
} NL_POLICY, *PNL_POLICY;
typedef struct _NL_STATS {
    ULONG64 RxTotal;
    ULONG64 TxTotal;
    ULONG64 RxCaptured;
    ULONG64 TxCaptured;
    ULONG64 Injected;
    ULONG64 Dropped;
    ULONG   ActiveUnits;
    ULONG   AbiVersion;
} NL_STATS, *PNL_STATS;

#endif //__FILTERUSER_H__

