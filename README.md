# ndislwf -- NDIS 6 LightWeight Filter

Driver filter passthrough NDIS 6.30 dengan kontrol device (`\\.\ndislwf`),
ring capture/inject, dan kebijakan blok TX.

## Struktur

```
src/ndislwf/      driver kernel (C, WDK/MSVC, x64 Release)
src/nlbridge/     transport user-mode C ABI (nlbridge.dll)
src/shim/         ndisapi.dll shim kompat legacy filter (C++)
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
Transport user-mode C ABI tersedia di `src/nlbridge` (nlbridge.dll, 23 fungsi).

### Shim ndisapi.dll (kompat legacy filter)

`src/shim/ndisapi.cpp` dibangun menjadi `ndisapi.dll` — nama API helper
user-mode legacy filter (driver legacy filter sendiri adalah `nllegacy.sys`).
Tool lama yang memanggil `ndisapi.dll` jalan tanpa recompilasi di atas
nlbridge:

```
GetTcpipBoundAdaptersInfo -> nl_enum        (\DEVICE\nl<i>, handle=i+1)
SetAdapterMode/GetAdapterMode -> nl_set_mode (TUNNEL diperlakukan LISTEN)
ReadPacket/ReadPackets    -> nl_read_ring   (batch maks 256)
SendPacketToAdapter       -> nl_inject_to   (arah adapter)
SendPacketToMstcp         -> nl_inject_rx
```

Struct packed dikunci static_assert: `INTERMEDIATE_BUFFER` 1566 B,
`ETH_REQUEST` 16 B, `ETH_M_REQUEST` 24 B, `TCP_AdapterList` 8836 B,
`ADAPTER_MODE` 12 B. Keterbatasan: mode TUNNEL tidak menahan paket asli di
adapter (capture/inject dua arah, tidak memblokir aliran), `GetAdapterMode`
adalah stub. Build:
```
tests\build_um.cmd /nologo /W4 /O2 /MT /LD /DWIN32 /D_WINDOWS \
  src\shim\ndisapi.cpp /Fe:src\shim\ndisapi.dll /Fo:src\shim\ndisapi.obj \
  /I src\nlbridge src\nlbridge\nlbridge.lib kernel32.lib user32.lib advapi32.lib
```
Probe verifikasi: `tests\shimprobe.c` (load dll → enum → mode listen →
UDP TX → ReadPackets 20 → send adapter + mstcp; PASS 13/13 di guest).

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
0x89002034 POLICY_EX    (in: NL_POLICY_EX = 712 B; count=0 → kembali ke POLICY lama)
0x89002038 DELAYSTAT    (out: NL_DELAY_STAT = pending + fired u64)
0x8900203C PROGRAM      (in: NL_VM_PROG = 520 B; count=0 → kembali ke tabel EX/POLICY)
0x89002040 MIRROR       (in: NL_MIRROR_CFG = src+dst+flags; bit0 TX bit1 RX; 0 = matikan semua)
0x89002044 INJECT_TO    (in: NL_INJECT_TO = dst+len+data[1514]; TX inject ke adapter tertentu)
0x89002050 DEBUG        (out: NL_DEBUG = 48 B; snapshot state evaluasi: flow gen + flag tabel aktif)
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

Event coalesced (M2): kernel menyinyal event saat ring berubah
empty→nonempty ATAU tiap 64 entri baru — bukan per paket. Counter
`TxWake/RxWake` di header menghitung sinyal per arah; consumer instan
menghasilkan 1 wake/paket (wasEmpty tiap burst), consumer tertinggal
menghasilkan 1 wake/batch (paket/wake ≈ 60 saat bench 4 socket drift).

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

### Netem per rule (v4)

`POLICY_EX` memasang tabel EX: tiap rule punya aksi
(`ALLOW/BLOCK/DELAY/DROP_PCT/RATE`), delay ms, drop %, dan rate Kbps.
Saat tabel EX aktif, seluruh frame dievaluasi first-match TANPA flow
cache (aksi bisa acak/waktu-bergantung); `count=0` mengembalikan ke
tabel v3. Aksi DELAY menahan frame di antrean kernel (DPC timer,
max 4096 item, due maks 60 s) lalu mainkan ulang saat matang — TX
dilewatkan apa adanya, RX di-indicate ulang; status antrean dibaca
via `DELAYSTAT` (pending/fired). DROP_PCT pakai RNG per rule; RATE
adalah token bucket per rule (burst = 1 s rate, refill QPC).

### VM mini BPF klasik (v5)

`PROGRAM` memasang program VM (`NL_VM_PROG`, maks 64 insn, register A/X,
scratch M[16], bytecode kelas BPF klasik: LD/LDX/ST/STX/ALU/JMP/RET/MISC dgn
size W/H/B, mode IMM/ABS/IND/MEM/LEN). Program aktif menggantikan SEMUA tabel
(EX dan policy v3); `count=0` mengembalikan evalasi ke tabel. Ret insn =
`ACTION | (Param << 8)` — jadi semua aksi netem (BLOCK/DELAY/DROP_PCT/RATE)
bisa dibangun dari satu program; DELAY param = ms, DROP_PCT = %, RATE = kbps.
Hit program tercatat di `NlRuleHits[0]` (slot 0); program match field apa pun
via ABS/IND offset byte frame (MAC, IP header, port). `enabled` bitmask
(NL_POLICY_EN_TX/RX) menentukan arah yang dievaluasi program.

### Mirror adapter→adapter + redirect (v6/M5)

`MIRROR` memasang salinan frame antar adapter: frame yang dicapture pada
adapter sumber (arah TX dan/atau RX, per `NL_MIRROR_*` flag) juga ditulis ke
ring target (`NlPushTx/NlPushRx` target) lengkap dgn counter/event target.
Pasangan disimpan per-instance (`NlMirrorTx/NlMirrorRx`), target di-refer
(RefCount) dan dilepas otomatis saat salah satu sisi detach
(`NlMirrorDetach`). `flags=0` atau `DstIndex==SrcIndex` mematikan pasangan.
`INJECT_TO` mengirim frame ke adapter tertentu (redirect — instance target
di-refer selama send).

### Hardening (v6/M6)

- **Debug ioctl** `0x89002050` (`NL_IOCTL_DEBUG`, nlbridge `nl_debug_state()`):
  snapshot state evaluasi — `FlowGen` (u64) + flag/jumlah tabel aktif
  (`policy en/cnt`, `ex en/cnt`, `vm a/en/cnt`), jumlah pasangan mirror, panjang
  antrean inject/delay. nlstress menampilkan `[dbg]` saat pengujian policy.
- **Fuzz ioctl**: `nl_ioctl_raw()` (nlbridge, gateway DeviceIoControl mentah
  tanpa validasi) + mode `fuzz <detik>` nlstress — 3 thread (`fzRaw` raw ioctl
  garbage-but-typed, `fzVal` state valid acak, `fzSend` beban UDP 2 soket);
  bisect `fuzzraw/fuzz3/fuzz5/fuzzval`, `nl_reset_all()` di cleanup.
- **Driver Verifier** (standard + NDIS/WIFI, flag `0x2209bb`) pada
  `ndislwf.sys`: menangkap 2 bug nyata — 0xC4/0x62 leak-on-unload
  (`NlInjectComplete` tidak membebaskan NBL; Arg4 8182/2950 alokasi) dan
  `NlIdx` = 0xFFFFFFFF saat attach pertama (counter init -1; kini NlIdx =
  ordinal list). Pasca fix: tFull **66/66 PASS** + chaos 10 menit BSOD-0 PASS
  + uninstall/reinstall bersih. Parser minidump x64 ringan:
  `tests/parsesmall.mdmp.ps1` (BugCheckCode @0x38, P1..P4 @0x40-0x58).
