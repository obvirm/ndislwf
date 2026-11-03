/* vbbridge.h -- kontrak C ABI v1 vbbridge.dll <-> driver ndislwf.sys.
 *
 * Sumber kebenaran: struct ioctl di src/ndislwf/filteruser.h pada repo yang
 * sama. Semua struct fixed-size; layout little-endian, natural alignment.
 * Semua ioctl METHOD_BUFFERED, FILE_ANY_ACCESS, device type 0x8900:
 *   code = (0x8900 << 16) | (fn << 2)
 */
#ifndef VBBRIDGE_H
#define VBBRIDGE_H

#include <stdint.h>

#ifdef VBBRIDGE_EXPORTS
#define VB_API __declspec(dllexport)
#else
#define VB_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Codes: sama persis dengan konstanta kernel (value terpasang). */
#define VB_IOC_VERSION    0x89002000u  /* fn 0x800 */
#define VB_IOC_ENUM_ADPT  0x89002004u  /* fn 0x801 */
#define VB_IOC_SET_MODE   0x89002008u  /* fn 0x802 */
#define VB_IOC_STATS      0x8900200Cu  /* fn 0x803 */
#define VB_IOC_READ_RING  0x89002010u  /* fn 0x804 */
#define VB_IOC_INJECT     0x89002014u  /* fn 0x805 */
#define VB_IOC_CLEAR      0x89002018u  /* fn 0x806 */

#define VB_ABI_MAGIC      0x31504256u  /* 'VBP1' */
#define VB_ABI_VERSION    0x00010000u  /* v1.0 */

#define VB_FLAG_RUNNING   0x1u
#define VB_MODE_RX        0x1u
#define VB_MODE_TX        0x2u
#define VB_RING_TX        0x1u
#define VB_RING_RX        0x2u

#define VB_IFNAME_MAX     260
#define VB_MAX_ADAPTERS   64
#define VB_FRAME_MAX      1514
#define VB_RING_ENTRIES   1024

/* 544 byte: enum adapter */
typedef struct vb_adapter {
    uint32_t index;                 /* ordinal ENUM, sedap SET_MODE */
    uint32_t medium;                /* NdisMedium* */
    uint32_t flags;                 /* bit0 running */
    uint32_t reserved;
    uint8_t  mac[6];
    uint8_t  pad2[2];
    uint16_t ifname[VB_IFNAME_MAX]; /* UTF-16 null-terminated */
} vb_adapter_t;

/* 8 byte */
typedef struct vb_mode {
    uint32_t adapter_index;
    uint32_t flags;
} vb_mode_t;

/* 56 byte */
typedef struct vb_stats {
    uint64_t rx_total;
    uint64_t tx_total;
    uint64_t rx_captured;
    uint64_t tx_captured;
    uint64_t injected;
    uint64_t dropped;
    uint32_t active_units;
    uint32_t abi_ver;
} vb_stats_t;

/* 1524 byte: satu frame dari ring */
typedef struct vb_ring_entry {
    uint32_t len;
    uint32_t flags;                 /* VB_RING_TX / VB_RING_RX */
    uint8_t  data[VB_FRAME_MAX];
} vb_ring_entry_t;

/* 1524 byte: output INJECT */
typedef struct vb_inject {
    uint32_t len;
    uint32_t reserved;
    uint8_t  data[VB_FRAME_MAX];
} vb_inject_t;

/* --- API ------------------------------------------------------------------ */

/* Buka \\. \ndislwf. 0 = sukses; selain itu last error Win32. */
VB_API int  vb_open(void);
VB_API void vb_close(void);

/* 0 = sukses. */
VB_API int  vb_version(uint32_t* magic, uint32_t* ver);

/* Enum: isi out[0..*count); max <= VB_MAX_ADAPTERS. 0 = sukses. */
VB_API int  vb_enum(vb_adapter_t* out, int max, int* count);

/* Set mode capture per adapter. 0 = sukses. */
VB_API int  vb_set_mode(uint32_t adapter_index, uint32_t flags);

/* Statistik penuh. 0 = sukses. */
VB_API int  vb_stats(vb_stats_t* out);

/* Satu panggilan READ_RING: out bisa max entri; count efektif di *count.
 * 0 = sukses (jika count = 0, ring kosong). */
VB_API int  vb_read_ring(vb_ring_entry_t* out, int max, int* count);

/* Drain seluruh ring; summary di luar. 0 = sukses. */
VB_API int  vb_drain_ring(int* tx, int* rx, uint64_t* bytes);

/* Kirim frame mentah (panjang 1..VB_FRAME_MAX). 0 = sukses. */
VB_API int  vb_inject(const void* data, uint32_t len);

VB_API int  vb_clear(void);

#ifdef __cplusplus
}
#endif
#endif /* VBBRIDGE_H */
