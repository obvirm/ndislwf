/* nlbridge.h -- kontrak C ABI v1 nlbridge.dll <-> driver ndislwf.sys.
 *
 * Sumber kebenaran: struct ioctl di src/ndislwf/filteruser.h pada repo yang
 * sama. Semua struct fixed-size; layout little-endian, natural alignment.
 * Semua ioctl METHOD_BUFFERED, FILE_ANY_ACCESS, device type 0x8900:
 *   code = (0x8900 << 16) | (fn << 2)
 */
#ifndef NLBRIDGE_H
#define NLBRIDGE_H

#include <stdint.h>

#ifdef NLBRIDGE_EXPORTS
#define NL_API __declspec(dllexport)
#else
#define NL_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Codes: sama persis dengan konstanta kernel (value terpasang). */
#define NL_IOC_VERSION    0x89002000u  /* fn 0x800 */
#define NL_IOC_ENUM_ADPT  0x89002004u  /* fn 0x801 */
#define NL_IOC_SET_MODE   0x89002008u  /* fn 0x802 */
#define NL_IOC_STATS      0x8900200Cu  /* fn 0x803 */
#define NL_IOC_READ_RING  0x89002010u  /* fn 0x804 */
#define NL_IOC_INJECT     0x89002014u  /* fn 0x805 */
#define NL_IOC_CLEAR      0x89002018u  /* fn 0x806 */
#define NL_IOC_POLICY     0x8900201Cu  /* fn 0x807 (v2) */
#define NL_IOC_MAP_RING   0x89002020u  /* fn 0x808 (v2) */
#define NL_IOC_UNMAP_RING 0x89002024u  /* fn 0x809 (v2) */
#define NL_IOC_GET_EVENT  0x89002028u  /* fn 0x80a (v2) */
#define NL_IOC_INJECT_RX  0x8900202Cu  /* fn 0x80b (v3) */
#define NL_IOC_RULEHITS   0x89002030u  /* fn 0x80c (v3) */
#define NL_IOC_POLICY_EX  0x89002034u  /* fn 0x80d (v4/M3) */
#define NL_IOC_DELAYSTAT  0x89002038u  /* fn 0x80e (v4/M3) */
#define NL_IOC_PROGRAM    0x8900203Cu  /* fn 0x80f (v5/M4) */
#define NL_IOC_MIRROR     0x89002040u  /* fn 0x810 (v6/M5) */
#define NL_IOC_INJECT_TO  0x89002044u  /* fn 0x811 (v6/M5) */

#define NL_ABI_MAGIC      0x31504C4Eu  /* 'NLP1' */
#define NL_ABI_VERSION    0x00010000u  /* v1.0 */

#define NL_FLAG_RUNNING   0x1u
#define NL_MODE_RX        0x1u
#define NL_MODE_TX        0x2u
#define NL_RING_TX        0x1u
#define NL_RING_RX        0x2u

#define NL_IFNAME_MAX     260
#define NL_MAX_ADAPTERS   64
#define NL_FRAME_MAX      1514
#define NL_RING_ENTRIES   1024

/* 544 byte: enum adapter */
typedef struct nl_adapter {
    uint32_t index;                 /* ordinal ENUM, sedap SET_MODE */
    uint32_t medium;                /* NdisMedium* */
    uint32_t flags;                 /* bit0 running */
    uint32_t reserved;
    uint8_t  mac[6];
    uint8_t  pad2[2];
    uint16_t ifname[NL_IFNAME_MAX]; /* UTF-16 null-terminated */
} nl_adapter_t;

/* 8 byte */
typedef struct nl_mode {
    uint32_t adapter_index;
    uint32_t flags;
} nl_mode_t;

/* 56 byte */
typedef struct nl_stats {
    uint64_t rx_total;
    uint64_t tx_total;
    uint64_t rx_captured;
    uint64_t tx_captured;
    uint64_t injected;
    uint64_t dropped;
    uint32_t active_units;
    uint32_t abi_ver;
} nl_stats_t;

/* 1524 byte: satu frame dari ring */
typedef struct nl_ring_entry {
    uint32_t len;
    uint32_t flags;                 /* NL_RING_TX / NL_RING_RX */
    uint8_t  data[NL_FRAME_MAX];
} nl_ring_entry_t;

/* 1524 byte: output INJECT */
typedef struct nl_inject {
    uint32_t len;
    uint32_t reserved;
    uint8_t  data[NL_FRAME_MAX];
} nl_inject_t;

/* --- v2 ------------------------------------------------------------------- */

#define NL_POLICY_MAX_RULES 16u
#define NL_PROTO_ANY        0u
#define NL_PROTO_TCP        6u
#define NL_PROTO_UDP        17u
#define NL_MATCH_SRC        0x1u
#define NL_MATCH_DST        0x2u
#define NL_MATCH_DSTMAC     0x4u
#define NL_POLICY_EN_TX     0x1u        /* bitmask enabled: bit0 drop TX */
#define NL_POLICY_EN_RX     0x2u        /* bit1 drop RX */
#define NL_SHM_MAGIC        0x31534C4Eu  /* 'NLS1' */
#define NL_SHM_VERSION      1u
#define NL_SHM_HDR_BYTES    128u
#define NL_SHM_TXRING_OFF   NL_SHM_HDR_BYTES
#define NL_SHM_BYTES        (NL_SHM_TXRING_OFF + (2u * (NL_RING_ENTRIES * 1524u)))

/* 28 byte: satu rule policy */
typedef struct nl_policy_rule {
    uint32_t proto;                 /* NL_PROTO_* */
    uint32_t match;                 /* NL_MATCH_* (bitwise) */
    uint32_t ip;                    /* network byte order */
    uint32_t mask;
    uint16_t port_min;
    uint16_t port_max;
    uint8_t  dst_mac[6];
} nl_policy_rule_t;

/* v4 (M3): aksi netem per rule — tabel EX. Saat EX aktif (count > 0),
 * evaluasi frame SELURUHNYA lewat EX; tabel policy lama tidak dipakai.
 * count = 0 mematikan EX dan mengembalikan ke tabel policy lama. */
#define NL_ACTION_ALLOW     0u
#define NL_ACTION_BLOCK     1u
#define NL_ACTION_DELAY     2u
#define NL_ACTION_DROP_PCT  3u
#define NL_ACTION_RATE      4u
#define NL_DELAY_MAX_MS     60000u

/* 44 byte: rule EX (28 B rule v1 + 4 aksi) */
typedef struct nl_policy_rule_ex {
    nl_policy_rule_t r;
    uint32_t action;                /* NL_ACTION_* */
    uint32_t delay_ms;              /* NL_ACTION_DELAY (0 = passthrough) */
    uint32_t drop_pct;              /* 0..100 (NL_ACTION_DROP_PCT) */
    uint32_t rate_kbps;             /* NL_ACTION_RATE (0 = tanpa limit) */
} nl_policy_rule_ex_t;

/* 712 byte: tabel EX */
typedef struct nl_policy_ex {
    uint32_t count;                 /* <= NL_POLICY_MAX_RULES; 0 = nonaktif */
    uint32_t enabled;               /* bitmask NL_POLICY_EN_* */
    nl_policy_rule_ex_t rules[NL_POLICY_MAX_RULES];
} nl_policy_ex_t;

/* 16 byte: status antrean delay netem (v4) */
typedef struct nl_delay_stat {
    uint64_t pending;               /* masih menunggu due */
    uint64_t fired;                 /* sudah matang keluar antrean */
} nl_delay_stat_t;

/* --- v5: VM mini BPF klasik (M4) ------------------------------------------ */

#define NL_VM_MAX_INSNS    64u
#define NL_VM_SCRATCH      16u

/* classes */
#define NL_VM_CL_LD        0x00u
#define NL_VM_CL_LDX       0x01u
#define NL_VM_CL_ST        0x02u
#define NL_VM_CL_STX       0x03u
#define NL_VM_CL_ALU       0x04u
#define NL_VM_CL_JMP       0x05u
#define NL_VM_CL_RET       0x06u
#define NL_VM_CL_MISC      0x07u

/* sizes (W genap 0) */
#define NL_VM_SZ_H         0x08u
#define NL_VM_SZ_B         0x10u

/* modes: IMM=0, ABS 0x20, IND 0x40, MEM 0x60, LEN 0x80 */
#define NL_VM_MD_IMM       0x00u
#define NL_VM_MD_ABS       0x20u
#define NL_VM_MD_IND       0x40u
#define NL_VM_MD_MEM       0x60u
#define NL_VM_MD_LEN       0x80u

/* alu/jmp ops */
#define NL_VM_OP_ADD       0x00u
#define NL_VM_OP_SUB       0x10u
#define NL_VM_OP_MUL       0x20u
#define NL_VM_OP_DIV       0x30u
#define NL_VM_OP_OR        0x40u
#define NL_VM_OP_AND       0x50u
#define NL_VM_OP_LSH       0x60u
#define NL_VM_OP_RSH       0x70u
#define NL_VM_OP_NEG       0x80u
#define NL_VM_JA           0x00u
#define NL_VM_JEQ          0x10u
#define NL_VM_JGT          0x20u
#define NL_VM_JGE          0x30u
#define NL_VM_JSET         0x40u
#define NL_VM_SRC_X        0x08u

/* ret + misc */
#define NL_VM_RTA          0x00u
#define NL_VM_RTK          0x10u
#define NL_VM_TAX          0x00u
#define NL_VM_TXA          0x80u

/* 8 byte */
typedef struct nl_vm_insn {
    uint16_t code;
    uint8_t  jt;                    /* offset true (relatif insn[i+1]) */
    uint8_t  jf;
    uint32_t k;
} nl_vm_insn_t;

/* 520 byte: prog ketika Count > 0, 0 = nonaktif (kembali ke tabel) */
typedef struct nl_vm_prog {
    uint32_t count;
    uint32_t enabled;               /* bitmask NL_POLICY_EN_* */
    nl_vm_insn_t insns[NL_VM_MAX_INSNS];
} nl_vm_prog_t;

/* 456 byte: set policy global */
typedef struct nl_policy {
    uint32_t count;                 /* <= NL_POLICY_MAX_RULES */
    uint32_t enabled;               /* bitmask NL_POLICY_EN_* */
    nl_policy_rule_t rules[NL_POLICY_MAX_RULES];
} nl_policy_t;

/* 144 byte: hit per rule (v3); rules[i] match saat memblok kernel +1 */
typedef struct nl_rule_hits {
    uint64_t hits[NL_POLICY_MAX_RULES];
    uint64_t block_tx;
    uint64_t block_rx;
} nl_rule_hits_t;

/* 96 byte struct (band header 128 byte): kepala SHM; head kernel, tail app.
 * TxWake/RxWake (v3 M2): jumlah sinyal event coalesced per arah. */
typedef struct nl_shm_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t adapter_index;
    uint32_t flags;
    volatile int32_t tx_head;       /* kernel menulis */
    volatile int32_t tx_tail;       /* app menulis */
    volatile int32_t rx_head;       /* kernel menulis */
    volatile int32_t rx_tail;       /* app menulis */
    uint32_t reserved[4];
    volatile uint64_t tx_bytes_mirror;
    volatile uint64_t rx_bytes_mirror;
    volatile uint64_t cap_tx_mirror;
    volatile uint64_t cap_rx_mirror;
    volatile uint32_t tx_wake;      /* sinyal event TX (coalesced) */
    volatile uint32_t rx_wake;      /* sinyal event RX (coalesced) */
    uint32_t wake_pad[2];
} nl_shm_hdr_t;

typedef struct nl_map_ring {
    uint32_t adapter_index;
    uint32_t len;                   /* >= NL_SHM_BYTES */
    void*    user_va;               /* page-aligned buffer milik caller */
} nl_map_ring_t;

typedef struct nl_event {
    uint64_t handle;                /* user handle (SynchronizationEvent) */
} nl_event_t;

/* --- v6 (M5): MIRROR + REDIRECT -------------------------------------------- */

#define NL_MIRROR_TX   0x1u            /* mirror arah TX */
#define NL_MIRROR_RX   0x2u            /* mirror arah RX */

typedef struct nl_mirror_cfg {
    uint32_t src_index;                 /* adapter sumber */
    uint32_t dst_index;                 /* adapter target */
    uint32_t flags;                     /* NL_MIRROR_* | 0 = matikan semua */
} nl_mirror_cfg_t;

typedef struct nl_inject_to {
    uint32_t dst_index;                 /* adapter target TX inject */
    uint32_t len;                       /* frame 1..NL_FRAME_MAX */
    uint8_t  data[NL_FRAME_MAX];
} nl_inject_to_t;

/* --- API ------------------------------------------------------------------ */

/* Buka \\. \ndislwf. 0 = sukses; selain itu last error Win32. */
NL_API int  nl_open(void);
NL_API void nl_close(void);

/* 0 = sukses. */
NL_API int  nl_version(uint32_t* magic, uint32_t* ver);

/* Enum: isi out[0..*count); max <= NL_MAX_ADAPTERS. 0 = sukses. */
NL_API int  nl_enum(nl_adapter_t* out, int max, int* count);

/* Set mode capture per adapter. 0 = sukses. */
NL_API int  nl_set_mode(uint32_t adapter_index, uint32_t flags);

/* Statistik penuh. 0 = sukses. */
NL_API int  nl_stats(const uint32_t* adapter, nl_stats_t* out);

/* Satu panggilan READ_RING: out bisa max entri; count efektif di *count.
 * 0 = sukses (jika count = 0, ring kosong). */
NL_API int  nl_read_ring(const uint32_t* adapter, nl_ring_entry_t* out, int max, int* count);

/* Drain seluruh ring; summary di luar. 0 = sukses. */
NL_API int  nl_drain_ring(int* tx, int* rx, uint64_t* bytes);

/* Kirim frame mentah (panjang 1..NL_FRAME_MAX). 0 = sukses. */
NL_API int  nl_inject(const void* data, uint32_t len);

NL_API int  nl_clear(void);

/* --- API v2 --------------------------------------------------------------- */

/* Set policy global (block TX/RX). 0 = sukses. */
NL_API int  nl_policy_set(const nl_policy_t* policy);

/* v4: set tabel aksi netem per rule. 0 = sukses. */
NL_API int  nl_policy_ex_set(const nl_policy_ex_t* ex);

/* v4: baca status antrean delay netem. 0 = sukses. */
NL_API int  nl_delay_stat(nl_delay_stat_t* out);

/* v5: pasang program VM (rule terkompilasi); count=0 mematikan. 0 = sukses. */
NL_API int  nl_vm_set(const nl_vm_prog_t* prog);

/* v6: pasang/cabut mirror (flags=0 = matikan semua arah). 0 = sukses. */
NL_API int  nl_mirror_set(const nl_mirror_cfg_t* cfg);

/* v6: inject TX ke adapter tertentu (REDIRECT). 0 = sukses. */
NL_API int  nl_inject_to(uint32_t dst_index, const void* data, uint32_t len);

/* Suntik frame ke jalur RX (replay, v3). 0 = sukses. */
NL_API int  nl_inject_rx(const void* data, uint32_t len);

/* Hit per rule + counter block per arah (v3). 0 = sukses. */
NL_API int  nl_rule_hits(nl_rule_hits_t* out);

/* Map buffer SHM untuk capture zero-copy. virtual_address harus page-aligned
 * (aloc di nl_shm_alloc) dan len >= NL_SHM_BYTES. Buffer milik caller;
 * mapping lepas otomatis saat handle device ditutup atau nl_shm_unmap. */
NL_API void* nl_shm_alloc(void);
NL_API int   nl_shm_map(uint32_t adapter_index, void* virtual_address, uint32_t len);
NL_API int   nl_shm_unmap(uint32_t adapter_index);

/* Ambil event kernel->app (SynchronizationEvent) untuk adapter; hasilnya
 * HANDLE valid di proses caller (pindahkan ke HANDLE, WaitForSingleObject).
 * Panggilan ulang mengganti event yang berlaku; bila mapping dilepas/map
 * ulang, ambil event lagi (yang lama basi). */
NL_API int   nl_shm_event(uint32_t adapter_index, uint64_t* handle);

#ifdef __cplusplus
}
#endif
#endif /* NLBRIDGE_H */
