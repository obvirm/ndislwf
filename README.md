# ndislwf -- NDIS 6 LightWeight Filter

Driver filter passthrough NDIS 6.30 dengan kontrol device (`\\.\ndislwf`),
ring capture/inject, dan kebijakan blok TX.

## Struktur

```
src/ndislwf/      driver kernel (C, WDK/MSVC, x64 Release)
src/nlbridge/     transport user-mode C ABI (nlbridge.dll)
src/installer/    install.ps1 / uninstall.ps1 + artifact build
```

## Build (Windows, EWDK 10.0.26100)

```
F:\BuildEnv\SetupBuildEnv.cmd
msbuild src\ndislwf\ndislwf.vcxproj /p:Configuration=Release /p:Platform=x64
```

Artefak: `x64\Release\ndislwf\NDISLWF.sys|ndislwf.inf|ndislwf.cat`.

## Instalasi di VM target

Jalankan `install.ps1` (administrator) di folder artefak; `uninstall.ps1`
untuk pembongkaran. Windows harus dalam mode test-signing untuk katalog
test-signed:
```
bcdedit /set testsigning on   (boot ulang)
```

## Kontrol

Buka device `\\.\ndislwf` dan kirim ioctl sesuai bagian kontrak ioctl di bawah.
Transport user-mode C ABI tersedia di `src/nlbridge` (nlbridge.dll, 14 fungsi).

## Kontrak ioctl

Semua METHOD_BUFFERED, access FILE_ANY_ACCESS, device 0x8900:

```
0x89002000 VERSION
0x89002004 ENUM_ADPT    (out: [NL_ADAPTER x n], 544 B per entri)
0x89002008 SET_MODE     (in: NL_MODE = idx+flags; flag 1=rx 2=tx)
0x8900200C STATS        (out: NL_STATS = 56 B, capture penuh)
0x89002010 READ_RING    (out: [u32 count][NL_RING_ENTRY x count], 1524 B/entri)
0x89002014 INJECT       (in:  NL_INJECT = len+reserved+data[1514])
0x89002018 CLEAR        (reset counter capture)
0x8900201C POLICY       (in:  NL_POLICY = count+enabled+rules[16]); Enabled bitmask: bit0 TX, bit1 RX
0x89002020 MAP_RING     (in:  NL_MAP_RING = idx+len+UserVa[page-aligned, >= NL_SHM_BYTES]); STATUS_DEVICE_BUSY bila sudah ter-map
0x89002024 UNMAP_RING   (in:  u32 adapter index; STATUS_ACCESS_DENIED bila milik file handle lain)
0x89002028 GET_EVENT    (in: u32 idx; out: NL_EVENT = handle; SynchronizationEvent kernel->app; GET ulang mengganti handle aktif)
0x8900202C INJECT_RX    (in:  NL_INJECT; frame di-indicate ke RX stack; ditolak bila policy RX memblok)
0x89002030 RULEHITS     (out: NL_RULEHITS = hits[16] u64 per rule + block_tx + block_rx)
```

### Ring shared-memory (zero-copy)

Buffer user (page-aligned, `NL_SHM_BYTES` = 128 + 2x1024x1524 =
3 117 184 B; slot 1524 B setelah tail-pad align 4) berisi:

```
[NL_SHM_HDR 128 B][TX ring 1024 slot][RX ring 1024 slot]
slot = NL_RING_ENTRY (Len/Flags/Data) — layout sama dengan READ_RING.
```

Kernel menulis `TxHead/RxHead`, app menulis `TxTail/RxTail`. Slot penuh
dihitung drop. Saat mapped, capture tak lagi menyalin ke ring internal;
`READ_RING` tetap menyalurkan sisa entri ring internal. Mapping dilepas
otomatis saat `FILE_OBJECT` app ditutup (IRP_MJ_CLOSE), adapter detach,
atau unload.

### Flow cache

Keputusan policy per-5-tuple TCP/UDP IPv4 di-cache (16384 entri,
direct-mapped) dan di-invalidate otomatis pada tiap `POLICY` baru
(generasi).

Definisi struct lengkap: `src/ndislwf/filteruser.h` (kernel) dan
`src/nlbridge/nlbridge.h` (user-mode C).

### Evaluasi policy (v3)

`NlPolicyEval(Data, Len, Dir)` mengevaluasi frame IPv4 untuk arah Dir
(`NL_POLICY_EN_TX` / `NL_POLICY_EN_RX`): dst-MAC rule → flow cache →
IP-rule walker. Rule yang match memblok frame dan menaikkan
`NlRuleHits[i]` + counter `NlBlocked(Tx|Rx)` — increment di satu titik.
`nl_inject_rx()` meng-evaluasi policy RX sebelum frame di-indicate, jadi
frame yang diblokir tidak sampai ke TCP/IP stack.
