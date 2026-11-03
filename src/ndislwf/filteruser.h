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
#define VB_DEV_TYPE   0x8900
#define VB_CODE(fn)   ((VB_DEV_TYPE << 16) | (FILE_ANY_ACCESS << 14) | ((fn) << 2) | METHOD_BUFFERED)

#define VB_IOCTL_VERSION    VB_CODE(0x800)
#define VB_IOCTL_ENUM_ADPT  VB_CODE(0x801)
#define VB_IOCTL_SET_MODE   VB_CODE(0x802)
#define VB_IOCTL_STATS      VB_CODE(0x803)
#define VB_IOCTL_READ_RING  VB_CODE(0x804)
#define VB_IOCTL_INJECT     VB_CODE(0x805)
#define VB_IOCTL_CLEAR      VB_CODE(0x806)
#define VB_IOCTL_POLICY     VB_CODE(0x807)

#define VB_ABI_MAGIC   0x31504256u   /* 'VBP1' */
#define VB_ABI_VERSION ((1UL << 16) | 0UL)

#define VB_FLAG_RUNNING   0x1
#define VB_MODE_RX        0x1
#define VB_MODE_TX        0x2

typedef struct _VB_VERSION {
    ULONG   Magic;
    ULONG   Version;
} VB_VERSION, *PVB_VERSION;

#define VB_IFNAME_MAX 260
#define VB_MAX_ADAPTERS 64

typedef struct _VB_ADAPTER {
    ULONG   Index;
    ULONG   Medium;
    ULONG   Flags;         /* VB_FLAG_RUNNING */
    ULONG   Reserved;
    UCHAR   Mac[6];
    UCHAR   Pad2[2];
    WCHAR   IfName[VB_IFNAME_MAX]; /* UTF-16, null-terminated */
} VB_ADAPTER, *PVB_ADAPTER;

typedef struct _VB_MODE {
    ULONG   AdapterIndex;
    ULONG   Flags;         /* VB_MODE_RX | VB_MODE_TX */
} VB_MODE, *PVB_MODE;


/* ----------------------------------------------------------------
 * Ring capture/inject v1
 * ---------------------------------------------------------------- */
#define VB_FRAME_MAX        1514
#define VB_RING_ENTRIES     1024
#define VB_RING_TX          0x1
#define VB_RING_RX          0x2

typedef struct _VB_RING_ENTRY {
    ULONG   Len;            /* byte efektif di Data */
    ULONG   Flags;          /* VB_RING_TX dari TX path, VB_RING_RX dari RX path */
    UCHAR   Data[VB_FRAME_MAX];
} VB_RING_ENTRY, *PVB_RING_ENTRY;

typedef struct _VB_INJECT {
    ULONG   Len;
    ULONG   Reserved;
    UCHAR   Data[VB_FRAME_MAX];
} VB_INJECT, *PVB_INJECT;

/* ----------------------------------------------------------------
 * Policy block TX v1
 * ---------------------------------------------------------------- */
#define VB_POLICY_MAX_RULES 16
#define VB_PROTO_ANY        0
#define VB_PROTO_TCP        6
#define VB_PROTO_UDP        17
#define VB_MATCH_SRC        0x1
#define VB_MATCH_DST        0x2

typedef struct _VB_POLICY_RULE {
    ULONG   Proto;      /* 6/17/0=any */
    ULONG   Match;      /* VB_MATCH_SRC | VB_MATCH_DST (bitwise) */
    ULONG   Ip;         /* network byte order */
    ULONG   Mask;
    USHORT  PortMin;
    USHORT  PortMax;
} VB_POLICY_RULE, *PVB_POLICY_RULE;

typedef struct _VB_POLICY {
    ULONG   Count;      /* <= VB_POLICY_MAX_RULES */
    ULONG   Enabled;    /* global: atur drop TX */
    VB_POLICY_RULE Rules[VB_POLICY_MAX_RULES];
} VB_POLICY, *PVB_POLICY;
typedef struct _VB_STATS {
    ULONG64 RxTotal;
    ULONG64 TxTotal;
    ULONG64 RxCaptured;
    ULONG64 TxCaptured;
    ULONG64 Injected;
    ULONG64 Dropped;
    ULONG   ActiveUnits;
    ULONG   AbiVersion;
} VB_STATS, *PVB_STATS;

#endif //__FILTERUSER_H__

