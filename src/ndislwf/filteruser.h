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
/* v2: ring shared-memory zero-copy */
#define NL_IOCTL_MAP_RING   NL_CODE(0x808)
#define NL_IOCTL_UNMAP_RING NL_CODE(0x809)
#define NL_IOCTL_GET_EVENT  NL_CODE(0x80a)
/* v3 (M1): inject RX (replay) + observability per-rule */
#define NL_IOCTL_INJECT_RX  NL_CODE(0x80b)
#define NL_IOCTL_RULEHITS   NL_CODE(0x80c)
/* v4 (M3): tabel aksi netem per rule */
#define NL_IOCTL_POLICY_EX  NL_CODE(0x80d)

/* v4 (M3): observability antrean delay (netem) */
#define NL_IOCTL_DELAYSTAT  NL_CODE(0x80e)

/* v5 (M4): VM mini BPF klasik — rule terkompilasi */
#define NL_IOCTL_PROGRAM    NL_CODE(0x80f)

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
#define NL_POLICY_EN_TX     0x1     /* bitmask Enabled: drop TX */
#define NL_POLICY_EN_RX     0x2     /* bit1: drop RX */

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
    ULONG   Enabled;    /* bitmask: bit0 = drop TX, bit1 = drop RX (v2; 1 tetap berarti TX-only) */
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

/* v3: hit per rule (index = urutan rule di policy) + block per arah.
 * Hits[i] di-increment tepat oleh kernel saat rule i yang match memblok. */
typedef struct _NL_RULEHITS {
    ULONG64 Hits[NL_POLICY_MAX_RULES];
    ULONG64 BlockTx;
    ULONG64 BlockRx;
} NL_RULEHITS, *PNL_RULEHITS;

/* v4 (M3): aksi netem per rule. Tabel EX terpisah dari tabel lama 456 B
 * (yang tidak boleh berubah); ketika EX aktif (Count > 0), evaluasi frame
 * SELURUHNYA lewat EX dan tabel lama tidak dipakai; Count = 0 mengembalikan
 * kasus ke tabel lama. */
#define NL_ACTION_ALLOW     0
#define NL_ACTION_BLOCK     1
#define NL_ACTION_DELAY     2
#define NL_ACTION_DROP_PCT  3
#define NL_ACTION_RATE      4

typedef struct _NL_POLICY_RULE_EX {
    NL_POLICY_RULE R;       /* 28 B tamplate rule v1 */
    ULONG   Action;         /* NL_ACTION_* */
    ULONG   DelayMs;        /* NL_ACTION_DELAY */
    ULONG   DropPct;        /* 0..100 (NL_ACTION_DROP_PCT) */
    ULONG   RateKbps;       /* NL_ACTION_RATE (0 = tanpa limit) */
} NL_POLICY_RULE_EX, *PNL_POLICY_RULE_EX;

typedef struct _NL_POLICY_EX {
    ULONG   Count;              /* <= NL_POLICY_MAX_RULES; 0 = nonaktif */
    ULONG   Enabled;            /* bitmask NL_POLICY_EN_* */
    NL_POLICY_RULE_EX Rules[NL_POLICY_MAX_RULES];
} NL_POLICY_EX, *PNL_POLICY_EX;

/* v4: status antrean delay — Pending = item yang masih menunggu due,
 * Fired = total item yang sudah matang (di-forward/indicate ulang). */
typedef struct _NL_DELAY_STAT {
    ULONG64 Pending;
    ULONG64 Fired;
} NL_DELAY_STAT, *PNL_DELAY_STAT;

/* ----------------------------------------------------------------
 * v5 (M4): VM mini BPF klasik
 * ----------------------------------------------------------------
 * Bytecode BPF-c klasik (lsb/ldh/ldw/ldi/ldm, jmp jeq/jgt/jge/jset,
 * alu, ret, tax/txa) seperti cBPF/seccomp: register A, X, 16 scratch
 * slot M[]. Aksi keluar RET: nilai 32-bit = ACTION | (Param << 8),
 * ACTION = NL_ACTION_*; Param: DELAY ms, DROP_PCT %, RATE kbps.
 * Ketika program aktif, tabel EX/policy lama TIDAK dievaluasi;
 * Count = 0 mematikan VM dan mengembalikan ke tabel.
 * Hit program masuk NlRuleHits[0] (slot rule-0 dipinjam VM). */
#define NL_VM_MAX_INSNS     64

/* classes */
#define NL_VM_CL_LD         0x00
#define NL_VM_CL_LDX        0x01
#define NL_VM_CL_ST         0x02
#define NL_VM_CL_STX        0x03
#define NL_VM_CL_ALU        0x04
#define NL_VM_CL_JMP        0x05
#define NL_VM_CL_RET        0x06
#define NL_VM_CL_MISC       0x07

/* ld/ldx sizes (W = 0) */
#define NL_VM_SZ_H          0x08    /* 16-bit */
#define NL_VM_SZ_B          0x10    /* 8-bit */

/* mode ENCODING klasik: LD/SZ IMM = mode 0; ABS=0x20; IND=0x40;
 * MEM=0x60; LEN=0x80 */
#define NL_VM_MD_IMM        0x00
#define NL_VM_MD_ABS        0x20
#define NL_VM_MD_IND        0x40
#define NL_VM_MD_MEM        0x60
#define NL_VM_MD_LEN        0x80

/* alu/jmp ops (Code & 0xF0) + Jmp/A source X bit (Code & 0x08) */
#define NL_VM_OP_ADD        0x00
#define NL_VM_OP_SUB        0x10
#define NL_VM_OP_MUL        0x20
#define NL_VM_OP_DIV        0x30
#define NL_VM_OP_OR         0x40
#define NL_VM_OP_AND        0x50
#define NL_VM_OP_LSH        0x60
#define NL_VM_OP_RSH        0x70
#define NL_VM_OP_NEG        0x80
#define NL_VM_JA            0x00
#define NL_VM_JEQ           0x10
#define NL_VM_JGT           0x20
#define NL_VM_JGE           0x30
#define NL_VM_JSET          0x40
#define NL_VM_SRC_X         0x08

/* ret */
#define NL_VM_RTA           0x00    /* ret: keluar = A */
#define NL_VM_RTK           0x10    /* ret: keluar = K */

/* misc */
#define NL_VM_TAX           0x00    /* X = A */
#define NL_VM_TXA           0x80    /* A = X */

#define NL_VM_SCRATCH       16

typedef struct _NL_VM_INSN {
    USHORT  Code;
    UCHAR   Jt;         /* offset true (relatif ins[i+1]) */
    UCHAR   Jf;         /* offset false */
    ULONG   K;
} NL_VM_INSN, *PNL_VM_INSN;

typedef struct _NL_VM_PROG {
    ULONG       Count;          /* <= NL_VM_MAX_INSNS; 0 = nonaktif */
    ULONG       Enabled;        /* bitmask NL_POLICY_EN_* */
    NL_VM_INSN  Insns[NL_VM_MAX_INSNS];
} NL_VM_PROG, *PNL_VM_PROG;

/* ----------------------------------------------------------------
 * v2: ring shared-memory zero-copy per adapter
 * ----------------------------------------------------------------
 * Layout buffer usermode (harus disediakan caller NL_IOCTL_MAP_RING,
 * page-aligned, ukuran >= NL_SHM_SIZE):
 *   [NL_SHM_HDR 128 B][TX ring][RX ring]
 *   slot ring = NL_RING_ENTRY (Len/Flags/Data) — layout identik
 *   dengan READ_RING, jadi app decode sama.
 * Kernel menulis HEAD slot; app menulis TAIL slot setelah diproses.
 * Selama buffer ter-mapped, capture kernel menulis langsung ke
 * memori app (zero-copy); READ_RING hanya aktif di mode internal.
 */
#define NL_SHM_MAGIC            0x31534C4e  /* 'NLPs' reversed; unik v2 */
#define NL_SHM_VERSION          0x1
#define NL_SHM_HDR_BYTES        128
#define NL_SHM_TXRING_OFF       NL_SHM_HDR_BYTES
#define NL_SHM_BYTES            (NL_SHM_TXRING_OFF + (2 * (NL_RING_ENTRIES * sizeof(NL_RING_ENTRY))))

typedef struct _NL_SHM_HDR {
    ULONG           Magic;              /* NL_SHM_MAGIC */
    ULONG           Version;            /* NL_SHM_VERSION */
    ULONG           AdapterIndex;       /* NlIdx adapter tujuan */
    ULONG           Flags;              /* cek validitas arah (NL_RING_TX|RX) */
    volatile LONG   TxHead;             /* kernel menulis */
    volatile LONG   TxTail;             /* app menulis */
    volatile LONG   RxHead;             /* kernel menulis */
    volatile LONG   RxTail;             /* app menulis */
    ULONG           Reserved[4];        /* pad kepala ring ke 64 B */
    volatile ULONG64 TxBytesMirror;     /* mirror stats (kernel) */
    volatile ULONG64 RxBytesMirror;
    volatile ULONG64 CapTxMirror;
    volatile ULONG64 CapRxMirror;
    /* v3 M2: penghitung kejadian wake (event dikirim koales — bukan per
     * paket); struktur di bawah nulla jadi pad sampai 128 B kepala. */
    volatile ULONG  TxWake;             /* sinyal event arah TX */
    volatile ULONG  RxWake;             /* sinyal event arah RX */
    volatile ULONG  WakePad[2];
} NL_SHM_HDR, *PNL_SHM_HDR;

typedef struct _NL_MAP_RING {
    ULONG   AdapterIndex;
    ULONG   Len;                        /* ukuran buffer user (>= NL_SHM_BYTES) */
    PVOID   UserVa;                     /* page-aligned user buffer */
} NL_MAP_RING, *PNL_MAP_RING;

typedef struct _NL_EVENT {
    ULONG64 Handle;                     /* user handle event (SynchronizationEvent) */
} NL_EVENT, *PNL_EVENT;

#endif //__FILTERUSER_H__

