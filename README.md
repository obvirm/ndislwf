# ndislwf

NDIS 6 lightweight filter (LWF) passthrough driver with a control device.
Adapted from Microsoft's `network/ndis/filter` ndislwf sample (MIT).

## What it is

- Pure NDIS LWF: registers via `NdisFRegisterFilterDriver`, clones/passes
  through TX and RX without touching packets.
- Per-instance RX/TX byte counters (interlocked) and a stable per-attach
  adapter index.
- Control device `\Device\NDISLWF` (DOS name `\\.\ndislwf`) exposing a small
  ioctl contract, defined in `src/ndislwf/filteruser.h`:
  - `VB_IOCTL_VERSION`   — magic + ABI version
  - `VB_IOCTL_ENUM_ADPT` — enumerate attached filter instances
  - `VB_IOCTL_SET_MODE`  — set per-instance mode
  - `VB_IOCTL_STATS`     — aggregate counters
  - `VB_IOCTL_CLEAR`     — reset counters

It is NOT a bridge, not a protocol hook, and has no virtual adapter.

## Layout

- `src/ndislwf/` — C driver source (`ndislwf.vcxproj`)

## Build

With EWDK (or WDK) 26100 command prompt:

```
msbuild src\ndislwf\ndislwf.vcxproj /p:Configuration=Debug /p:Platform=x64
```

Outputs `NDISLWF.sys` + `ndislwf.inf` + `ndislwf.cat` (test-signed in Debug).

## Install (test machine, elevated)

```
bcdedit /set testsigning on   (reboot once)
pnputil /add-driver ndislwf.inf /install
netcfg -v -l ndislwf.inf -c s -i MS_NdisLwf
sc query NDISLWF
```

`netcfg` stage may not copy the binary to `C:\Windows\System32\DRIVERS\ndislwf.sys`;
if `sc start NDISLWF` reports exit code 2, copy `NDISLWF.sys` there manually.

## Status

- Builds clean on EWDK 26100.6584 (x64 Debug).
- Load-tested on Windows 10 x64 VM: service `NDISLWF` starts, `RUNNING`.
- Filter attach verification still in progress (docs/TESTPLAN.md, F1).

## License

GPL-2.0 — see `LICENSE`. Based on the Microsoft ndislwf sample, which is
licensed MIT; that sample is © Microsoft.
