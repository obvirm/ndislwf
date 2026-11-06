# ndislwf — NDIS 6 lightweight packet filter

An NDIS 6.30 lightweight filter (LWF) driver with a device control plane
(`\\.\ndislwf`), capture/inject rings, per-rule rule engine (block / delay /
drop% / rate), a classic BPF-style mini VM, adapter-to-adapter mirroring, and
a user-mode shim for legacy `ndisapi.dll` callers.

> **Intended use & legal notice**
> This is a network test and lab tool: traffic capture, reproduction and
> impairment on machines **you own or are authorized to test**. Using it to
> intercept, modify, or disrupt traffic that you have no authorization over
> is not an intended use and may be unlawful in your jurisdiction. The
> software is provided "as is", without warranty of any kind (see LICENSE);
> you install and run it at your own risk. No telemetry, no phoning home,
> nothing leaves the machine except network frames the filter itself handles.

## Features

- Capture on both directions (RX and TX) per minute-bound adapters, via an
  in-kernel ring or zero-copy shared memory + signaled events
- Inject raw frames into the Windows stack (RX) or directly to any bound
  adapter (TX)
- Result table: 16 first-match rules — direction, protocol (any/icmp/tcp/udp),
  IP prefix, port range or destination MAC — with actions
  ALLOW / BLOCK / DELAY (0–60 s, kernel DPC timer queue) / DROP (percentage) /
  RATE (token bucket, kbps)
- Classic-cBPF-like packet filter VM (64 registers, A/X + 16 scratch,
  LD/LDX/ST/STX/ALU/JMP/RET) that can return any action, including netem ones
- Adapter → adapter mirroring (copies all captured frames of a source
  adapter into the ring of a target adapter), plus redirect by TX-injection
  to a specific adapter
- Per-rule hit counters, per-direction queue stats, delay queue stats
  (global and per rule), debug state snapshot ioctl for observability
- `nlctl` CLI for all of the above without writing C
- `ndisapi.dll` compatibility shim so legacy user-mode tools run
  without recompilation

**Status:** production-internal. Data path and rules engine are exercised by
end-to-end tests (full = 66/66, 10-minute chaos runs under Driver Verifier,
break, ioctl fuzzing) — but see "Build & install a signed driver" below:
there is **no** prebuilt signed binary in this repository.

## Restrictions

```
src/ndislwf/      kernel filter driver (C, WDK/MSVC, x64 Release)
src/nlbridge/     user-mode C-ABI control plane (nlbridge.dll)
src/shim/         ndisapi.dll compat shim (C++)
src/installer/    install.ps1 / uninstall.ps1 + build artifacts
tests/            nlctl CLI + stress harness + user-mode build helper
docs/ID.md        the full development log/rootmap (indonesian)
```

`tests/guest/` is a local VM-test harness and intentionally not tracked.

## Build (Windows x64)

Option A — standard WDK (recommended for outsiders):

```
1. Install Visual Studio 2022 + Windows Driver Kit 10 (matching your SDK)
2. msbuild src\ndislwf\ndislwf.vcxproj /p:Configuration=Release /p:Platform=x64
```

Artifacts land in `x64\Release\ndislwf\`: `NDISLWF.sys`, `ndislwf.inf`,
`ndislwf.cat`.

Option B — Enterprise WDK (what the author uses to build all major artifacts
in one command, no full SDK install):

```
set EWDK_ROOT=<your EWDK folder>     (default F:\BuildEnv)
tests\build_um.cmd <cl arguments>
```

`build_um.cmd` sets INCLUDE/LIB from the EWDK toolchain and then requests
`cl` verbatim, so both user-mode helpers (`nlbridge`, shim, `nlctl` and the
stress harness) build out of the same command. Example:

```
tests\build_um.cmd /nologo /W4 /O2 /MT /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS
  tests\nlctl.c /Fe:tests\nlctl.exe /Fo:tests\nlctl.obj
  /I src\nlbridge src\nlbridge\nlbridge.lib kernel32.lib advapi32.lib
```

`nlbridge.dll` itself:

```
tests\build_um.cmd /nologo /W4 /O2 /MT /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS
  /DNLBRIDGE_EXPORTS /LD src\nlbridge\nlbridge.cpp
  /Fe:src\nlbridge\nlbridge.dll /Fo:src\nlbridge\nlbridge.obj
```

Build the DLL directly to its final filename — do NOT rename a `/Fe:out`
afterwards: the PE import table of `nlctl.exe` carries the DLL name at link
time, and a renamed DLL fails to load silently (0xC0000135).

## Build & install a *signed* driver

`.sys` kernel drivers are rejected by Windows unless the catalog is signed.
This repo ships **test-signed** artifacts only, which load when:
```
bcdedit /set testsigning on      (then reboot)
```

For self-signing your own build (publishing the driver is not the same as
signing it):

```
1. Use "SignTool" with a self-signed certificate you trust:
   signtool sign /fd sha256 /a /ph /v ndislwf.cat
2. install that trusted root into the target machine:
   certmgr /add mycert.cer /s root
3. bcdedit /set testsigning on + reboot (still needed for anything not
   production-signed by Microsoft)
```

Microsoft production signing (attestation via Hardware Dev Center) is only
needed if you distribute prebuilt binaries to machines with Secure Boot on;
it requires an EV code-signing certificate (~$250-400/year). For a
source-first project, self-sign inmedia is the documented route.

## Usage — `nlctl`

```
nlctl show                                  kernel state + stats + hits + delay + adapters
nlctl enum | nlctl stats [idx]
nlctl mode <idx> <0|rx|tx|txrx>
nlctl block <dir> [proto] [ip[/p]] [port] [mac]      dir = tx|rx|txrx, proto = any|icmp|tcp|udp
nlctl delay tx udp 9998 500                 (ms)   | nlctl drop txrx udp 9997 50 (%)
nlctl rate tx udp 9996 256                  (kbps) | nlctl allow <...>
nlctl dlstat                                delay pending/fired + per-rule `rule[i] fired=`
nlctl clear                                 (wipes EX/POLICY/VM, stats + delay counters) | reset (clear + mode 0)
```

Rule commands overwrite the whole EX table with a single rule (no merge);
`ip[/p]` takes a /0-/32 prefix, `port` is N or N-M, `mac` switches the match
to destination MAC. Exit codes: 0 ok, 1 runtime error, 2 bad arguments.
A rule needs at least one match field besides the protocol (proto-only is
rejected). `nlctl load <file>` installs a multi-line ruleset — one rule per
line, same format (action value at end of line for delay/drop/rate) into the
EX table (16 rules, first-match, `#` comments); a bad line stops the run
with exit 1 and a line number.

`nlctl cap <idx> <tx|rx|txrx> <out.bin> [sec=5]` records ring frames into a
dump file (magic `NCLC`, 16-B header + one fixed 1524-B record per frame)
then switches capture mode back off. `nlctl send <dump.bin> [rx | to <idx>]`
replays a dump: each frame's direction follows its record flags
(TX → `INJECT_TO`, RX → `INJECT_RX`); `rx` routes everything into the
TCP/IP stack, `to <idx>` targets one adapter. `nlctl mirror <src> <dst>
<tx|rx|txrx|off>` toggles a mirror pair: every frame captured on the source
adapter is copied into the target adapter's ring (same direction).
`nlctl pcap <dump.bin>` converts NCLC → classic pcap (LE, Ethernet); no
driver needed. `nlctl vm <off|file>` installs/removes a mini-BPF program
from text: one instruction per line (`#` comments, `:label` markers),
`ldw/ldh/ldb/ldxw/ldxh/ldxb` + `abs|ind|imm <k>`, `ldw len`, `ld*w mem <n>`,
`st/stx <n>`, `tax/txa`, `alu add|sub|mul|div|or|and|lsh|rsh|neg <k|x>`,
`jmp eq|gt|ge|set <k|x> <t1> <t2>` / `jmp a <tgt>`, `ret a|allow|block|
delay <ms>|drop <pct>|rate <kbps>`, `enabled tx|rx|txrx`. An active program
overrides all tables (EX and v3 policy); `vm off` falls back to the EX
table. `nlctl replay <pcap> rx|to <idx>` replays a classic pcap (including
Wireshark exports) directly to the driver.

## shim — legacy `ndisapi.dll` compatibility

`src/shim/ndisapi.cpp` builds into `ndisapi.dll`. Legacy tools calling
`ndisapi.dll` run on this project without recompilation:

```
GetTcpipBoundAdaptersInfo -> nl_enum          (\DEVICE\nl<i>, handle=i+1)
SetAdapterMode/GetAdapterMode -> nl_set_mode  (TUNNEL treated as LISTEN)
ReadPacket/ReadPackets     -> nl_read_ring   (batch max 256)
SendPacketToAdapter        -> nl_inject_to   (adapter direction)
SendPacketToMstcp          -> nl_inject_rx
```

Packed structures are pinned by static_assert: `INTERMEDIATE_BUFFER` 1566 B,
`ETH_REQUEST` 16 B, `ETH_M_REQUEST` 24 B, `TCP_AdapterList` 8836 B,
`ADAPTER_MODE` 12 B. Limitation: TUNNEL mode does not hold the original
packet at the adapter (dual-direction capture/inject, does not block the
flow); `GetAdapterMode` is a stub. Verification probe:
`tests/shimprobe.c` (dll load → enum → listen mode → UDP TX → ReadPackets
20 → send adapter + mstcp; PASS 13/13 inside the reference VM).

## ioctl contract

All METHOD_BUFFERED, FILE_ANY_ACCESS, device type 0x8900:

```
0x89002000 VERSION
0x89002004 ENUM_ADPT    (out: [NL_ADAPTER x n], 544 B/entry)
0x89002008 SET_MODE     (in: NL_MODE = idx+flags; flag 1=rx 2=tx)
0x8900200C STATS        (out: NL_STATS = 56 B, full counters)
0x89002010 READ_RING    (out: [u32 count][NL_RING_ENTRY x count], 1524 B/entry)
0x89002014 INJECT       (in:  NL_INJECT = len+reserved+data[1514])
0x89002018 CLEAR        (clear capture stats — and now also delay queue/counters)
0x8900201C POLICY       (in:  NL_POLICY = count+enabled+rules[16]; enabled: bit0 TX bit1 RX)
0x89002020 MAP_RING     (in: NL_MAP_RING = idx+len+UserVa[page-aligned, >= NL_SHM_BYTES]; STATUS_DEVICE_BUSY when already mapped)
0x89002024 UNMAP_RING   (in: u32 adapter index; STATUS_ACCESS_DENIED when owned by another FILE_OBJECT)
0x89002028 GET_EVENT    (in: u32 idx; out: NL_EVENT = handle; SynchronizationEvent kernel->app; re-GET swaps the active handle)
0x8900202C INJECT_RX    (in:  NL_INJECT; frame is indicated into the RX path; rejected if RX policy blocks)
0x89002030 RULEHITS     (out: NL_RULEHITS = hits[16] u64 per rule + block_tx + block_rx)
0x89002034 POLICY_EX    (in: NL_POLICY_EX = 712 B; count=0 → back to legacy POLICY)
0x89002038 DELAYSTAT    (out: NL_DELAY_STAT = pending + fired u64)
0x8900203C PROGRAM      (in: NL_VM_PROG = 520 B; count=0 → back to EX/POLICY tables)
0x89002040 MIRROR       (in: NL_MIRROR_CFG = src+dst+flags; bit0 TX bit1 RX; 0 = off)
0x89002044 INJECT_TO    (in: NL_INJECT_TO = dst+len+data[1514]; TX inject to a specific adapter)
0x89002050 DEBUG        (out: NL_DEBUG = 48 B; snapshot of the evaluation state: flow gen + active-table flags)
0x89002054 DELAYSTAT_EX (out: NL_DELAY_STAT_EX = 144 B; pending/fired + fired_rule[16])
```

Full Struct definitions: `src/ndislwf/filteruser.h` (kernel side) and
`src/nlbridge/nlbridge.h` (user-mode C).

### Zero-copy shared-memory ring

User buffer (page-aligned, `NL_SHM_BYTES` = 128 + 2x1024x1524 =
3,117,184 B; 1524-B slots after the head-pad align 4):

```
[NL_SHM_HDR 128 B][TX ring 1024 slots][RX ring 1024 slots]
slot = NL_RING_ENTRY (Len/Flags/Data) — same layout as READ_RING.
```

Kernel writes TxHead/RxHead, app reads TxTail/RxTail. Full slots count as a
drop. While mapped, capture stops copying into the internal ring; READ_RING
still drains the leftovers of the internal ring. The mapping is released
automatically when the app FILE_OBJECT is closed (IRP_MJ_CLOSE), on adapter
detach, or during unload.

Coalesced events: the kernel signals when the ring changes empty→nonempty
OR every 64 new entries — not per packet. TxWake/RxWake counters in the
header accumulate signals per direction; a caught-up consumer generates
1 wake/packet (wasEmpty every burst), a lagging consumer around
1 wake/batch (packets/wake ≈ 60 measured during a 4-socket bench).

### Flow cache

Per-5-tuple TCP/UDP IPv4 policy decisions are cached (16384 entries,
direct-mapped) and auto-invalidated on every new POLICY (generation).

### Policy evaluation (v3)

`NlPolicyEval(Data, Len, Dir)` walks a frame in the direction Dir:
dst-MAC rule → flow cache → IP-rule walker. A matching rule drops the frame
and bumps `NlRuleHits[i]` + `NlBlocked(Tx|Rx)` — all from one code path.
`nl_inject_rx()` evaluates the RX policy before indicating, so blocked
frames never reach the TCP/IP stack.

### Per-rule netem (v4)

`POLICY_EX` installs the EX table: each rule carries an action
(ALLOW/BLOCK/DELAY/DROP_PCT/RATE), delay ms, drop %, rate kbps. With EX
active, all frames evaluate first-match WITHOUT the flow cache (actions can
be random or time-dependent); `count=0` switches back to the v3 table.
DELAY holds frames in a kernel queue (DPC timer, max 4096 items, due
≤ 60 s) and re-plays them when mature — TX is forwarded as-is, RX is
re-indicated; queue status is read via DELAYSTAT / DELAYSTAT_EX. DROP_PCT
uses a per-rule RNG; RATE is a per-rule token bucket (burst = 1 s of rate,
QPC refill).

### Classic mini-BPF VM (v5)

`PROGRAM` installs a VM program (`NL_VM_PROG`, max 64 insns, A/X registers,
M[16] scratch, classic BPF opcodes: LD/LDX/ST/STX/ALU/JMP/RET/MISC with
W/H/B sizes and IMM/ABS/IND/MEM/LEN modes). An active program replaces all
tables; `count=0` returns evaluation to the tables. RET insn = `ACTION |
(Param << 8)` — so any netem action (BLOCK/DELAY/DROP_PCT/RATE) can be built
in-machine; DELAY param is ms, DROP_PCT is %, RATE is kbps. Program hits are
recorded in `NlRuleHits[0]` (slot 0); the program can match any field via
ABS/IND byte offsets into the frame (MAC, IP header, ports). The `enabled`
bitmask (NL_POLICY_EN_TX/RX) steers which directions it evaluates.

### Mirror & redirect (v6)

`MIRROR` copies frames between adapters: frames captured on the source
adapter (direction TX and/or RX, per NL_MIRROR_* flags) are also written to
the target adapter's ring (`NlPushTx/NlPushRx` of the target), including
target counters/events. Pairs are held per-instance
(`NlMirrorTx/NlMirrorRx`), the target is reference-counted and released
automatically when either side detaches (`NlMirrorDetach`). `flags=0` or
`DstIndex==SrcIndex` removes pairs. `INJECT_TO` sends a frame to a specific
adapter (redirect — the target instance is referenced for the send).

### Hardening (M6)

- **Debug ioctl** `0x89002050` (`NL_DEBUG`, `nl_debug_state()`): snapshot of
  evaluation state — FlowGen (u64) + active-table flags/counts (policy
  en/cnt, ex en/cnt, vm a/en/cnt), mirror pair count, inject/delay queue
  lengths. The stress harness prints `[dbg]` during policy tests.
- **ioctl fuzz**: `nl_ioctl_raw()` (nlbridge, raw DeviceIoControl without
  app-side validation) + nlstress `fuzz <sec>` mode — 3 threads (`fzRaw`
  random garbage-but-typed raw ioctls, `fzVal` random valid state,
  `fzSend` 2-socket UDP load); bisect kinds `fuzzraw/fuzz3/fuzz5/fuzzval`,
  `nl_reset_all()` in cleanup. 6 fuzz→policy amplification cycles PASS.
- **Driver Verifier** (standard + NDIS/WIFI flags `0x2209bb`) on
  `ndislwf.sys` caught 2 real bugs — 0xC4/0x62 leak-on-unload
  (`NlInjectComplete` leaked one NBL per injection; 8182/2950 allocations)
  and `NlIdx` = 0xFFFFFFFF on first attach (counter init -1). After fixes:
  tFull **66/66 PASS**, 10-minute chaos BSOD-0 PASS, clean
  uninstall/reinstall. Minidump x64 parser included:
  `tests/parsesmall.mdmp.ps1` (BugCheckCode @0x38, P1–P4 @0x40-0x58).

## Credits

- The kernel driver started from Microsoft's `ndislwf` filter sample
  (Windows WDK samples, MIT license); the same attribution is preserved
  upstream in the individual source-file headers.

## License

BSD 3-Clause — see `LICENSE`. Contributions are welcome: open an issue or
send a PR. CI is skipped until the first outside contributor arrives.
