# ndislwf -- NDIS 6 LightWeight Filter

Driver filter passthrough NDIS 6.30 dengan kontrol device (`\\.\ndislwf`),
ring capture/inject, dan kebijakan blok TX. Menyatu dengan CLI Python/Rust
di proyek `l2split` (client gateway).

## Struktur

```
src/ndislwf/      driver kernel (C, WDK/MSVC, x64 Release)
src/vbbridge/     transport user-mode C ABI (vbbridge.dll)
src/installer/    install.ps1 / uninstall.ps1 + artifact build
src/vbclient      Rust CLI (ada di repo terpisah, E:\project\l2split)
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

## Kontrol (contoh CLI vpnbypass)

```
vpnbypass driver                 enum adapter + MAC
vpnbypass stats                  statistik passthrough/capture/inject
vpnbypass mode --rx --tx         aktifkan capture RX+TX adapter 0
vpnbypass ring --n 8 --hex       drain ring + hexdump
vpnbypass ringd --secs 10        drain ring tiap 500 ms selama 10 dtk
vpnbypass inject arp_req.bin     kirim frame mentah via adapter
vpnbypass policy --on --dst 10.0.0.1 --port 53 --tcp   blok TX
vpnbypass policy --off           matikan kebijakan
```

## Kontrak ioctl

Semua METHOD_BUFFERED, access FILE_ANY_ACCESS, device 0x8900:

```
0x89002000 VERSION
0x89002004 ENUM_ADPT    (out: [VB_ADAPTER x n], 544 B per entri)
0x89002008 SET_MODE     (in: VB_MODE = idx+flags; flag 1=rx 2=tx)
0x8900200C STATS        (out: VB_STATS = 56 B, capture penuh)
0x89002010 READ_RING    (out: [u32 count][VB_RING_ENTRY x count], 1524 B/entri)
0x89002014 INJECT       (in:  VB_INJECT = len+reserved+data[1514])
0x89002018 CLEAR        (reset counter capture)
0x8900201C POLICY       (in:  VB_POLICY = count+enabled+rules[16])
```

Definisi struct lengkap: `src/ndislwf/filteruser.h` (kernel) dan
`src/vbbridge/vbbridge.h` (user-mode C) + `src/vbclient.rs` (Rust).
