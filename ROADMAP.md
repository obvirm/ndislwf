# ROADMAP — nlwf: filosofi pengganti legacy filter

> Prinsip: **kernel = dataplane tipis; kecerdasan & kebijakan tinggal di user-space.**
> legacy filter = rule statis ber-field-tetap + satu paket per call. Kami tidak meniru bentuknya,
> kami menggantinya: rule terkompilasi (bytecode), batch zero-copy, aksi netem dinamis,
> state per-flow, observability per-rule, dan replay/inject dua arah.

## Milestone (urut dari paling mudah; tanda ✅ = tuntas)

### M1 ✅ Blok RX + counter hit per rule + inject RX (replay)
- [x] `NL_POLICY_EN_RX` benar-benar dievaluasi di path RX (`NlPolicyEval(..., NL_POLICY_EN_RX)`) — dulu bit-nya hanya TX.
- [x] Counter hit per rule (`NlRuleHits[16]`, u64) — ioctl baru `NL_IOCTL_RULEHITS`.
- [x] `NL_IOCTL_INJECT_RX`: inject frame masuk jalur RX (`NdisFIndicateReceiveNetBufferLists`) — alat replay/test yang legacy filter tidak punya; sekaligus membuat test policy RX deterministik.
- [x] Counter `NlBlockedRx` terpisah; keduanya (TX+RX) masuk stats.Dropped.

### M2 ✅ Batch drain async (event coalesced)
- [x] Kernel: event hanya sinyal saat empty→nonempty (awal batch) ATAU tiap 64 entri baru — bukan per paket (filter.c NlTxCapture/NlRxCapture).
- [x] Counter wake per arah di shm header: `TxWake/RxWake` (u32; header 128-B pad band, 80→96 B).
- [x] nlstress `bench`: fase A drain READ_RING (copy, ≤32/ioctl) vs fase B drain zero-copy SHM + drift konsumen sengaja agar backlog tampak.
- [x] Hasil guest (4 thread socket stack-TX): SHM drain 35–38k pps vs READ_RING 13–22k pps (`nlstress bench 3` = 2.7×; `full` = 1.7×); **paket/wake ≈ 60** (para wake per batch, bukan per paket); drop=0 pure bench.
- catatan: consumer instan (tidak pernah di belakang) menghasilkan 1 wake/paket (wasEmpty tiap burst) — coalescing tervisible di skenario overload; itulah bentuknya yang diinginkan untuk latensi rendah.

### M3 ✅ Aksi netem dinamis per rule
- `NL_ACTION = ALLOW|BLOCK|DELAY|DROP_PCT|RATE` per rule lewat tabel EX (`NL_POLICY_EX`, ioctl `0x89002034`); `count=0` kembali ke tabel policy lama.
- First-match TANPA flow cache (aksi bisa acak/waktu-bergantung); dmac rule dievaluasi dulu, fallback parse IPv4.
- DELAY: satu antrean global (max 4096, due ≤60 s) + KTIMER/KDPC, proses luar kunci, re-arm due terkecil; TX NBL ditahan lalu di-forward apa adanya, RX frame di-indicate ulang; `DELAYSTAT` (0x89002038) pas/fired observability; drain complete-failure saat pause/detach.
- DROP_PCT: RNG LCG per rule; RATE: token bucket per rule (burst = 1 s rate, refill QPC dibagi Freq).
- Hasil guest: netem 4/4 + full 53/53 pass (block 100/100, drop50 ±2k, delay pending→fired=10 antrean kering, rate 256 kbps drop 393 dari 600 frame, burst 32 KB).

### M4 VM mini BPF-style (rule terkompilasi) ✅ 2026-11-06
- Endi rule match bebas-field: user-space mengkompilasi `(match expr) → action` menjadi
  bytecode kelas BPF klasik (ld/ldh/ldw/jump/ret, register A/X, scratch M[16]), evaluator
  `NlVmRun` ~200 baris di driver — copy program ke stack di bawah NlFlowLock, OOB load = 0,
  div-by-zero = 0, lompat keluar program = ALLOW, guard 100000 step.
- `NL_IOCTL_PROGRAM` (0x8900203C): `nl_vm_set(prog)`; `count=0` kembali ke tabel EX/lama.
- Program aktif => SEMUA tabel bypass; ret = `ACTION | (Param<<8)`; BLOCK/DELAY/DROP_PCT/RATE
  semua aksi netem tersedia dari VM; hit program masuk `NlRuleHits[0]` (slot 0 dipinjam VM),
  DROP_PCT/RATE pinjam slot netem 15.
- **NlGate ikut VM** (bug ketemu pas first-run): gate = mask Enabled program saat VM aktif;
  tanpa itu gate=0 saat tabel kosong dan evaluator tidak pernah dieksekusi (vmdbg=4 → 0 entry).
- Hasil guest: `vm` standalone 5/5 pass (block dmac+port 200/200 hit, delay 300 ms
  pending 10 → fired 10 antrean kering) + `full` 58/58 pass (stage vm M4 masuk suka 9/11).

### M5 Mirror + redirect + shim compat
- [x] MIRROR ✅ 2026-11-06: salin frame capture adapter sumber ke ring adapter target per arah
  (`NL_MIRROR_TX/RX`, ioctl `0x89002040`). Pasangan per-instance (`NlMirrorTx/NlMirrorRx`),
  target nalloc di bawah `FilterListLock` (`NlMirrorTake`, skip `NlDetached`) dan di-deref
  pasca-push; pasangan di-refer (RefCount per arah) dan dilepas `NlMirrorDetach` di
  `FilterDetach` (dipanggil sebelum list-ref turun). Self-pair & flags=0 = matikan.
- [x] REDIRECT ✅ 2026-11-06: `INJECT_TO` (0x89002044) kirim frame ke adapter tertentu —
  `NlInjectTo` resolve target + refer di bawah lock (4x retry 1 ms), `NlInjectTxCore(Target)`
  kirim ke instance target persis; delay-replay memakai `Target=NULL` (first-running).
- [x] Test nlstress `mirror`: inject_to ret+counter, mirror ON → ring dst terisi entri TX,
  mirror OFF → berhenti, self-pair noop ret=0; masuk `full` stage 10/12.
- [x] `ndisapi.dll` shim ✅ 2026-11-06: kompatiblitas biner API legacy filter (`ndisapi.dll` — nama asli helper user-mode; *driver* legacy filter adalah `nllegacy.sys`, ROADMAP awal salah sebut `nllegacy.dll`) di `src\shim\ndisapi.cpp` — mapping ke nlbridge: `GetTcpipBoundAdaptersInfo→nl_enum`, `Set/GetAdapterMode→nl_set_mode` (TUNNEL diperlakukan LISTEN), `ReadPacket/ReadPackets→nl_read_ring`, `SendPacketToAdapter→nl_inject_to`, `SendPacketToMstcp→nl_inject_rx`. Handle = nl_index+1; handle == 0 invalid. Keterbatasan diimplement: TUNNEL tidak menahan paket di adapter (capture/inject dua arah; tidak memblokir aliran asli). Layout packed terkunci via static_assert: `INTERMEDIATE_BUFFER`=1566, `ETH_REQUEST`=16, `ETH_M_REQUEST`=24, `TCP_AdapterList`=8836, `ADAPTER_MODE`=12. Probe `tests\shimprobe.c` PASS 13/13 di guest (build: `/LD /MT src\shim\ndisapi.cpp`; run: load→enum→mode→ReadPackets 20 pkt→send×2).

### M6 Hardening produksi
- Driver Verifier ✅ 2026-11-06 (temuan langsung): standard + NDIS flags (0x2209bb) untuk ndislwf.sys di VM test. Boot normal (DHCP bound via filter) + tFull 65/66 → tersisa `shm: adapter idx`. Urutan hasil:
  1. BSOD 0xC4/0x62 (pool tracking leak-on-unload) terekam 2× minidump saat uninstall/reinstall driver (Arg4=8182 lalu 2950 alokasi). **Root cause: `NlInjectComplete` tidak membebaskan `NdisFreeNetBufferList(Ent->Nbl)`** — tiap inject completed bocor 1 NBL dari `NlInjectNblPool` (hanya `NlInjectUninit` yang membebaskan 4 sumber). FIX: NBL dibebaskan di complete path.
  2. **Bug indeks: `NlInstanceCounter` init -1 + `InterlockedIncrement()-1` → attach PERTAMA per `NlIdx`=0xFFFFFFFF**; detach tidak decrement counter, jadi re-attach (restart instance) menghasilkan indeks salah relatif ordinal list yang dipakai ENUM/SET_MODE/MAP_RING/STATS-format. Dari `shm: adapter idx` (hdr.AdapterIndex==0xFFFFFFFF vs g_adapter=0). FIX: `NlIdx` dihitung dari posisi list saat insert (loop sampai node sendiri di bawah FilterListLock), counter global dipensiunkan.
  3. Parser minidump POSIX-light di `tests/parsesmall.mdmp.ps1` (header x64: code @0x38, P1..P4 @0x40/48/50/58) — tanpa kd.exe.
- Fuzz ioctl ✅ 2026-11-06 (tahap 1): `nl_ioctl_raw` (nlbridge, gateway raw tanpa validasi) + mode `fuzz <detik>` di nlstress — 3 thread `fzRaw` (raw ioctl acak: POLICY/EX/VM/MIRROR/INJECT/RING dgn garbage-but-typed buffer), `fzVal` (set valid acak urut policy/ex/vm/mirror), `fzSend` (beban UDP 2 soket) — target: tidak ada BSOD, stats `ret=0`. Bisect kind: `fuzzraw`=1, `fuzz3`=3, `fuzz5`=5, `fuzzval`=6, `fuzz`=7 (semua), + `nl_reset_all()` di cleanup. 6 siklus amplifikasi (fuzz→policy standalone) PASS; debug ioctl `0x89002050` (`nl_debug_state`, `[dbg]` print: gen/pol/ex/vm/mir/dly) terbukti menampilkan state kernel saat set.
- [x] Verifier symptom pass ✅ 2026-11-06: pasca fix2 (hash `00A3FD65…`), tFull **66/66 PASS** + chaos 10 menit target BSOD-0 **PASS** (mode/policy/map-ua ratusan-ribu siklus, traffic rx 24k/tx 86k, tanpa BSOD, tanpa reboot, unload bersih tanpa crash saat reinstall). Verifier 0x2209bb dibiarkan aktif untuk regresi berikutnya.
- [ ] Sisa M6: chase race jarang (2× fail pd M5 driver lalu 5× pass; tidak repro pd M6, mungkin sudah hilang bersama tweak M6 atau masih ada — pakai [dbg] saat ketangkep), sertifikat produksi.

## Keputusan ABI yang terkunci
- `nl_policy_t` 456 B TIDAK diubah (kompat); counter baru hanya via ioctl tambahan.
- Semua ioctl METHOD_BUFFERED; codes: 0x89002000 + (fn-0x800)<<2; INJECT_RX=0x8900202C (0x80b), RULEHITS=0x89002030 (0x80c), POLICY_EX=0x89002034 (0x80d), DELAYSTAT=0x89002038 (0x80e), PROGRAM=0x8900203C (0x80f), MIRROR=0x89002040 (0x810), INJECT_TO=0x89002044 (0x811), DEBUG=0x89002050 (0x814, out NL_DEBUG 48 B: `FlowGen` u64 + PolicyEnabled/PolicyCount/ExEnabled/ExCount/VmActive/VmEn/VmCount/MirrorPairs/InjectQueueLen u32 — snapshot state gate `NlGate` untuk observabilitas); next free = 0x89002054 (0x815).
- Struct ring entry 1524 B tidak diubah (kunci ABI shm).
