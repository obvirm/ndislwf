# ROADMAP — nlwf: filosofi dan arah

> Prinsip: **kernel = dataplane tipis; kecerdasan & kebijakan tinggal di user-space.**
> Rule statis ber-field-tetap + satu paket per call bukan arah kami. Yang dianut:
> rule terkompilasi (bytecode), batch zero-copy, aksi netem dinamis,
> state per-flow, observability per-rule, dan replay/inject dua arah.

## Milestone (urut dari paling mudah; tanda ✅ = tuntas)

### M1 ✅ Blok RX + counter hit per rule + inject RX (replay)
- [x] `NL_POLICY_EN_RX` benar-benar dievaluasi di path RX (`NlPolicyEval(..., NL_POLICY_EN_RX)`) — dulu bit-nya hanya TX.
- [x] Counter hit per rule (`NlRuleHits[16]`, u64) — ioctl baru `NL_IOCTL_RULEHITS`.
- [x] `NL_IOCTL_INJECT_RX`: inject frame masuk jalur RX (`NdisFIndicateReceiveNetBufferLists`) — alat replay/test; sekaligus membuat test policy RX deterministik.
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
- [x] `ndisapi.dll` shim ✅ 2026-11-06: lapisan kompatibilitas untuk tool user-mode legacy yang memanggil `ndisapi.dll` (ABI membeku, tanpa recompilation) di `src\shim\ndisapi.cpp` — mapping ke nlbridge: `GetTcpipBoundAdaptersInfo→nl_enum`, `Set/GetAdapterMode→nl_set_mode` (TUNNEL diperlakukan LISTEN), `ReadPacket/ReadPackets→nl_read_ring`, `SendPacketToAdapter→nl_inject_to`, `SendPacketToMstcp→nl_inject_rx`. Handle = nl_index+1; handle == 0 invalid. Keterbatasan diimplement: TUNNEL tidak menahan paket di adapter (capture/inject dua arah; tidak memblokir aliran asli). Layout packed terkunci via static_assert: `INTERMEDIATE_BUFFER`=1566, `ETH_REQUEST`=16, `ETH_M_REQUEST`=24, `TCP_AdapterList`=8836, `ADAPTER_MODE`=12. Probe `tests\shimprobe.c` PASS 13/13 di guest (build: `/LD /MT src\shim\ndisapi.cpp`; run: load→enum→mode→ReadPackets 20 pkt→send×2).

### M6 Hardening produksi
- Driver Verifier ✅ 2026-11-06 (temuan langsung): standard + NDIS flags (0x2209bb) untuk ndislwf.sys di VM test. Boot normal (DHCP bound via filter) + tFull 65/66 → tersisa `shm: adapter idx`. Urutan hasil:
  1. BSOD 0xC4/0x62 (pool tracking leak-on-unload) terekam 2× minidump saat uninstall/reinstall driver (Arg4=8182 lalu 2950 alokasi). **Root cause: `NlInjectComplete` tidak membebaskan `NdisFreeNetBufferList(Ent->Nbl)`** — tiap inject completed bocor 1 NBL dari `NlInjectNblPool` (hanya `NlInjectUninit` yang membebaskan 4 sumber). FIX: NBL dibebaskan di complete path.
  2. **Bug indeks: `NlInstanceCounter` init -1 + `InterlockedIncrement()-1` → attach PERTAMA per `NlIdx`=0xFFFFFFFF**; detach tidak decrement counter, jadi re-attach (restart instance) menghasilkan indeks salah relatif ordinal list yang dipakai ENUM/SET_MODE/MAP_RING/STATS-format. Dari `shm: adapter idx` (hdr.AdapterIndex==0xFFFFFFFF vs g_adapter=0). FIX: `NlIdx` dihitung dari posisi list saat insert (loop sampai node sendiri di bawah FilterListLock), counter global dipensiunkan.
  3. Parser minidump POSIX-light di `tests/parsesmall.mdmp.ps1` (header x64: code @0x38, P1..P4 @0x40/48/50/58) — tanpa kd.exe.
- Fuzz ioctl ✅ 2026-11-06 (tahap 1): `nl_ioctl_raw` (nlbridge, gateway raw tanpa validasi) + mode `fuzz <detik>` di nlstress — 3 thread `fzRaw` (raw ioctl acak: POLICY/EX/VM/MIRROR/INJECT/RING dgn garbage-but-typed buffer), `fzVal` (set valid acak urut policy/ex/vm/mirror), `fzSend` (beban UDP 2 soket) — target: tidak ada BSOD, stats `ret=0`. Bisect kind: `fuzzraw`=1, `fuzz3`=3, `fuzz5`=5, `fuzzval`=6, `fuzz`=7 (semua), + `nl_reset_all()` di cleanup. 6 siklus amplifikasi (fuzz→policy standalone) PASS; debug ioctl `0x89002050` (`nl_debug_state`, `[dbg]` print: gen/pol/ex/vm/mir/dly) terbukti menampilkan state kernel saat set.
- [x] Verifier symptom pass ✅ 2026-11-06: pasca fix2 (hash `00A3FD65…`), tFull **66/66 PASS** + chaos 10 menit target BSOD-0 **PASS** (mode/policy/map-ua ratusan-ribu siklus, traffic rx 24k/tx 86k, tanpa BSOD, tanpa reboot, unload bersih tanpa crash saat reinstall). Verifier 0x2209bb dibiarkan aktif untuk regresi berikutnya.
- [x] Chase race jarang ✅ 2026-11-06 (ditutup tanpa repro): 2× fail eksklusif pada driver M5 lama; pasca fix2 tidak pernah muncul lagi — 6 siklus amplifikasi fuzz→policy PASS + chaos 10 menit PASS + **6× tFull 66/66 PASS berurutan di bawah Verifier (0x2209bb)**. Bila muncul lagi cukup pakai `[dbg]` (debug ioctl) + bisect kind.
- [ ] Sertifikat produksi (.hostname bukan test-sign), WHQL attestation jika perlu — butuh proses nyata di luar repo.

### M7 Operasional harian
- [x] `nlctl.exe` CLI ✅ 2026-11-06: panggilan tunggal untuk operasi live tanpa harness —
  `show` (version + kernel state `[dbg]` + stats + rulehits + delaystat + adapter),
  `enum`, `stats [idx]`, `mode <idx> <0|rx|tx|txrx>`, dan satu-rule-rule EX:
  `block/allow <dir> [proto] [ip[/p]] [port] [mac]`, `delay ... <ms>`,
  `drop ... <pct>`, `rate ... <kbps>` (setiap perintah rule MENIMPA tabel EX jadi
  count=1 — tanpa merge implisit), `clear` (kosongkan EX/POLICY/VM),
  `reset` (clear + mode 0 semua adapter). Kode keluar 0/1/2; parser IP pakai
  byte-order paket (sscanf dotted-quad → dword lawanan `*(ULONG*)&Data[off+12]`
  kernel), prefix /0-/32, port N|N-M, dmac aa:bb:cc:dd:ee:ff timpa match IP.
  Guard pemakaian tervalidasi di guest: dir/langkah tak lengkap → usage (exit 2),
  rule tanpa field match (proto saja) ditolak — tidak ada rule kosong yang
  diam-diam memasang match-any. Smoke guest lengkap PASS di bawah Verifier:
  mode rx, block udp 9999 (kernel ex=en=1 cnt=1), block tcp 10.0.11.5/24 port 80,
  delay 500 ms, drop 50%, rate 256 kbps, mac rule, usage-2, clear/reset bersih.
- [x] `nlctl load <file>` ✅ 2026-11-06: ruleset multi-baris ke tabel EX (16 cap) —
  satu baris satu rule dgn format sama (block/allow/delay/drop/rate, nilai aksi
  di ujung), `#` komentar, baris kosong dilewati, `enabled` union arah semua
  baris; gagal exit 1 dengan nomor baris yang salah (perintah tak dikenal,
  dir hilang, nilai aksi tak sah, rule tanpa field match). Guest smoke:
  5 rule dipasang (kernel `ex(en=3 cnt=5)`), file hilang exit 1, baris jahat
  exit 1, clear bersih — di bawah Verifier.
- [x] `nlctl cap`/`send` ✅ 2026-11-06: dump/replay frame operasional —
  `cap <idx> <tx|rx|txrx> <out.bin> [detik=5]` nyala mode → drain READ_RING
  (256/batch, poll 50 ms) → file `NCLC` (header 16 B: magic/ver/adapter/pad +
  rekaman 1524 B fixed per frame: len/flags/frame) → kembalikan mode 0;
  `send <dump.bin> [rx|to <idx>]` replay — arah per frame mengikuti flag
  rekaman (TX→`INJECT_TO`, RX→`INJECT_RX`); override `rx` (semua ke TCP/IP)
  atau `to <idx>` (semua ke adapter). Guest smoke PASS di bawah Verifier:
  cap 5 s dgn ping latar menangkap 3 frame TX ICMP (file 4,588 B = 16+3×1524),
  send default 3 + `rx` 3, stats `inj` naik persis +6. Format dump = biner
  fixed-record agar bisa dibaca tanpa seek oleh tool lain.
- [x] `nlctl mirror` + `nlctl pcap` ✅ 2026-11-06: `mirror <src> <dst>
  <tx|rx|txrx|off>` memasang/mematikan pasangan mirror ioctl MIRROR (off dan
  self-pair noop ret=0, arah salah → usage exit 2); `pcap <dump.bin>`
  mengkonversi NCLC → pcap klasik LE (header 24 B: magic `D4 C3 B2 A1`,
  vmaj=min=2/4, snaplen 65535, network=1 Ethernet; per frame ts berurut,
  incl=orig=len) — hasil dapat dibaca semua tool pcap standar.`pcap` tidak
  butuh driver (bypass `nl_open`, konversi off-line jalan di host).
  Tervalidasi host: 3 rekaman 110/74/110 B, header snaplen=65535.
- [x] Mirror end-to-end di guest ✅ 2026-11-06 (R12/R13, drv `9ede9491`):
  copy TX sumber masuk ring TX target dan counter per-adapter terbaca.
  Tiga akar masalah lama ditambal: (1) bit arah SET_MODE bentrok dgn bit
  policy — `mode X tx` (parse lama) kirim bit policy TX=0x1, di kernel itu
  NL_MODE_RX → semua "capture TX" lama sesungguhnya capture RX (cap_tx
  abadi 0); nlctl kini pisah parse_mode_dir (bit NL_MODE_*) bebas dr
  parse_dir (bit policy). (2) `stats <idx>` & `cap <idx>` pakai NlIdx
  stale, resolver ordinal `NlFilterByIdxLocked` dipakai device.c. (3)
  driver guest ternyata build lama (hash beda) — redeploy + verify SHA256.
  Bukti: `mirror 1 0 tx` + ping → ad1 TX 3 frame asli (74 B ICMP echo),
  ring TX ad0 berisi 3 copy, `stats 0` cap_tx=3, `stats 1` cap_tx=3,
  mirror off → bisu. R12 bungkuk: `cap 1 tx` (nlctl7 lama) = RX replies
  (flags=0x2) — petunjuk awal biar akar masalah terbaca.
- [x] `nlctl vm <off|file>` ✅ 2026-11-06: assembler teks untuk VM mini-BPF —
  M4 jadi bisa diprogram hari-hari tanpa C. Satu insn per baris, `#` komentar,
  label `:nama` atau `nama:` (dua pass, lompatan maju sah), `enabled
  tx|rx|txrx` (default txrx); mnemonic `ldw/ldh/ldb/ldxw/ldxh/ldxb` ×
  `abs <k>`/`ind <k>`/`imm <k>`), `ldw len`, `ldw|ldx mem <n>`, `st/stx <n>`,
  `tax/txa`, `alu add|sub|mul|div|or|and|lsh|rsh|neg <k|x>`,
  `jmp eq|gt|ge|set <k|x> <tgt>t <tgt>f>` / `jmp a <tgt>` (tgt = nomor insn
  absolut mulai 0 atau label, maju saja, u8), dan `ret
  a|allow|block|delay <ms>|drop <pct>|rate <kbps>` (ret RTK = aksi|param<<8,
  semua aksi netem dari teks). `vm off` = count=0 kembali ke tabel.
  Guest smoke di bawah Verifier: program blok ICMP TX (`ldb abs 34; jmp eq 8;
  ret block/allow`) → ping 1.1.1.1 **100% loss** saat aktif (kernel
  `vm(a=1 en=1 cnt=4)`), `vm off` → ping pulih 2/2; file mnemonic tak dikenal
  exit 1 dgn nomor baris; `vm off` bersih (`vm a=0 en=0 cnt=0`).
- [x] `nlctl replay <pcap> rx|to <idx>` ✅ 2026-11-06: replay pcap klasik
  (hasil pcap-tool lain termasuk export Wireshark) langsung ke driver —
  header 24 B divalidasi (magic LE `A1B2C3D4`, network=1), record LE
  ts/incl/orig, frame >1514 dipaksa lewat (hitung skip), `rx` = semua
  `INJECT_RX`, `to <idx>` = semua `INJECT_TO`. Guest smoke di bawah
  Verifier: 3 frame × pcap hasil `nlctl pcap` → `to 0` dan `rx`, stats
  `inj` naik persis +6; bukan-pcap exit 1, argumen kurang exit 2.

### M8 Kualitas observability & operabilitas (mulai 2026-11-06)
- [x] Reset counter delay via `clear` ✅ 2026-11-06: `nlctl clear` kini
  memanggil `nl_clear()` (NL_IOCTL_CLEAR); kernel: `NlDelayFlush` --
  semua item antrean dibuat jatuh tempo lalu ditembakkan lewat rutin DPC
  sama (NBL TX dicomplete, inject TX/RX di-forward, tanpa bocor), counter
  `fired`/`fired_rule` lalu di-nol-kan. Guest R15 (drv `d7519fee`):
  sebelum `clear` `fired=3`, sesudah `pending=0 fired=0` tanpa nilai negatif.

- [x] MAC adapter sejak attach ✅ 2026-11-06: `FilterRestart` kini query
  `OID_802_3_CURRENT_ADDRESS` via `filterDoInternalRequest` (sinkron,
  PASSIVE) dan mengisi `NlMac` sebelum Running — `enum`/`show` menampilkan
  MAC asli tanpa harus menunggu capture TX pertama (fallback capture tetap
  hidup bila OID gagal). Guest (drv `db3f f30f`): `enum` →
  `ad[0] mac=00-0C-29-7F-40-74`, `ad[1] mac=00-0C-29-7F-40-7E` langsung.
- [x] `tests/deploy-driver.ps1` ✅ 2026-11-06: satu perintah deploy driver
  host → guest — copy 3 berkas build (sys/inf/cat, cat dari sub-dir
  `ndislwf\`) → runProgramInGuest `h_rdrv.cmd` (pola wrapper ps1 yang
  terbukti M1–M3) → tarik `h_reinstall_out.txt` → verify SHA256 `sys`
  host vs guest (throw bila beda) → `-Bind` opsional `h_nlbind.cmd` +
  tarik `nlbind.txt`; `-SkipReinstall` = copy saja. Syarat deploy kini
  wajib verify hash — mencegah "driver guest = build lama" (akar masalah
  mirror #3). Copy vmrun asinkron di-dur: `fileExistsInGuest` tiap berkas.
- [x] DELAYSTAT_EX per-rule ✅ 2026-11-06: ioctl baru `0x89002054` (0x815,
  `NL_DELAY_STAT_EX` 144 B: pending/fired + `FiredRule[16]`) --
  kernel: `NlDelayFiredRule[]`, `NL_DELAY_ITEM.Rule`, `NlDelayAdd(+Rule)`;
  ABI `nl_delay_stat_ex`; CLI `dlstat` menampilkan `rule[i] fired=`. Guest R14
  (drv `bc960de2`): rule `delay tx icmp /24 700` → dlstat saat antre
  `pending=1 fired=2 rule[0]=2`, matang `fired=6 rule[0]=6`.
  Catatan perilaku: `reset`/`clear` TIDAK mereset counter delay (hanya boot) --
  item lanjutan bila dipakai test jangka panjang; parser nlctl rule memang
  wajib mempunyai match selain proto (proto-only ditolak -- `has_match`).
- [x] `NL_PROTO_ICMP` (1) + alias parser `icmp`/`1` ✅ 2026-11-06: define di
  `filteruser.h` + `nlbridge.h`; `parse_proto` nlctl kini menerima `icmp`.

## Keputusan ABI yang terkunci
- `nl_policy_t` 456 B TIDAK diubah (kompat); counter baru hanya via ioctl tambahan.
- Semua ioctl METHOD_BUFFERED; codes: 0x89002000 + (fn-0x800)<<2; INJECT_RX=0x8900202C (0x80b), RULEHITS=0x89002030 (0x80c), POLICY_EX=0x89002034 (0x80d), DELAYSTAT=0x89002038 (0x80e), PROGRAM=0x8900203C (0x80f), MIRROR=0x89002040 (0x810), INJECT_TO=0x89002044 (0x811), DEBUG=0x89002050 (0x814, out NL_DEBUG 48 B: `FlowGen` u64 + PolicyEnabled/PolicyCount/ExEnabled/ExCount/VmActive/VmEn/VmCount/MirrorPairs/InjectQueueLen u32 — snapshot state gate `NlGate` untuk observabilitas), DELAYSTAT_EX=0x89002054 (0x815, out NL_DELAY_STAT_EX 144 B); next free = 0x89002058 (0x816).
- Struct ring entry 1524 B tidak diubah (kunci ABI shm).

### Penyiapan open source (2026-11-06)
- [x] Lisensi BSD-3-Clause ✅ (menggantikan GPL-2; kompatibel MIT sample
  ndislwf milik Microsoft — kredit ditaruh di README).
- [x] Model distribusi: **source-first** — user build sendiri + self-sign
  (test-signing); sertifikat produksi/attestation jadi OPSIONAL, hanya bila
  nanti ingin menyediakan installer signed (biaya ~$250-400/tahun).
- [x] Sanitasi repo: `build_um.cmd` parameterize via `EWDK_ROOT`
  (default F:\BuildEnv, pemilik repo); `deploy-driver.ps1` wajib param atau
  env `NL_VMRUN/NL_VMX/NL_GUEST_USER/NL_GUEST_PASS` (tidak ada kredensial
  di kode); seluruh `tests/guest/` keluar dari git (lab lokal, rule
  `.gitignore`).
- [x] README bahasa Inggris + disclaimer legal lengkap; dokumen catatan
  pengembangan bahasa Indonesia dipindah ke `docs/ID.md`.
- [ ] CI GitHub Actions: DITUNDA sampai kontributor luar pertama (workflow
  ringan msbuild + artifact unsigned saat itu).
- Riwayat git dipublikasikan utuh; email di rewrite ke
  `obvirm@users.noreply.github.com` (noreply GitHub).

### Integrasi l2split — VPN bypass per-aplikasi atas NDISLWF (2026-11-06)
- Fokus pindah ke `E:\project\l2split` (Tauri/Rust): GUI aplikasi VPN bypass
  per-aplikasi yang **sudah jalan di atas driver sendiri** — `
  nlclient.rs` membuka `\\.\NDISLWF` dgn ABI v1 mirroring `nlbridge.h`
  (CTL_CODE formula identik), `engine.rs` = engine live, `nlengine.rs` =
  varian diagnostik CLI.
- Model bypass (murni NDIS, tanpa WFP/TDI/hooking): (1) rule kernel POLICY
  v3 match dst-MAC gateway VPN = BLOCK TX — frame tetap jatuh ke ring TX;
  (2) engine user-mode parse IPv4 + port sumber, PID via
  `proclookup.rs` (`GetExtendedTcpTable`, refresh periodik); (3) app
  whitelist → rewrite dst-MAC ke gateway LAN + `INJECT` (jalur inject
  kernel `NdisFSendNetBufferLists` langsung — tidak kena FilterSend /
  rule blok ulang); (4) app lain → inject apa adanya → tetap ke VPN.
  Temuan penting: batasan "NDIS tak bisa per-app" diatasi karena identitas
  aplikasi (PID) dihitung di user-space; driver hanya menyediakan
  blok-by-MAC + ring + inject.
- GUI sudah wired: `gui.rs::engine_bridge` (detect adapter VPN FROM otomatis,
  adapter LAN TO dari pilihan user, MAC kedua gateway via ping, spawn
  thread engine, stats 250 ms, cancel flag; per-app toggle via
  `Shared::set_app`).
- **Penambahan baru**: bundle penyebaran `l2split\driver\` —
  `install.ps1` (nilai default inf/sys/cat satu folder; cek testsigning;
  netcfg -u + pnputil import + netcfg -l pasang komponen + verifikasi
  layanan/binding), `uninstall.ps1`, `testsigning.ps1` (on/off bcdedit +
  deteksi status), `driver.cmd` (menu self-elevate UAC: install/uninstall/
  testsigning on/off) — siklus tombol testsigning per mesin (ON → reboot →
  install; OFF → reboot) yang membuat orang lain bisa pakai tanpa
  sertifikasi produksi. `nlclient.rs::cmd_driver_check` + `gui.rs`
  (`get_net`, `engine_bridge`) kini menunjuk `driver\driver.cmd` saat
  device belum terpasang. `cargo check` bersih.
- **Keputusan nama & positioning l2split** (via /grill-me, 2026-11-07):
  (1) nama proyek tetap **l2split** — kata kunci industri "split tunneling"
  persis namakan mekanismenya (per-app split tunneling di layer 2); "direct"
  = istilah akses/tujuan, "bypass" = istilah pop kurang presisi. Latar
  istilah yang dikunci dengan user: layer model = ISO 7498-1 (1984),
  L2 = Data Link (Ethernet/MAC, Metcalfe 1973 di Xerox PARC); nama "L2split"
  terpercaya tak berpasangan.
  (2) positioning l2split: **per-app + luas bertahap** (mulai per-app, nanti
  rule domain/IP); l2split indie — tidak menyebut/membandingkan proyek lain
  di repo publik (aturan proyek suci).
  (3) tagline About (terbukti di README.md + Cargo.toml description):
  *"Pick any app — send it straight out the Ethernet, bypassing the VPN.
  An alternative split tunneling."*
  (4) Lisensi l2split: **MIT murni** (bukan dual; Cargo `license = "MIT"`
  + file LICENSE, copyright "l2split contributors").
  (5) Rebrand identitas app selesai: `tauri.conf.json` productName/judul
  jendela = "l2split", identifier `dev.l2split.app`; `Cargo.toml` bin
  `nlbypass` → `l2split`; autostart reg-key gui.rs "NL Bypass Bridge" →
  "l2split"; cli.rs usage "nlbypass" → "l2split"; `ui/index.html` semua
  string "NL Bypass Bridge" → "l2split"; mock
  `nlbypass_bridge_ui.html` → `l2split_ui.html`. `cargo check` exit 0.
- **One-click setup wizard** (2026-11-07): `driver\prepare.ps1` (elevated:
  testsigning ON + daftar `HKLM\...\RunOnce` "l2split-install" →
  `install.ps1` + reboot otomatis) dan `driver\teardown.ps1` (uninstall +
  testsigning OFF). Siklus app-resmi: **Yes (UAC) → reboot → terpasang
  sendiri via RunOnce** — tanpa buka folder/cmd. GUI: wiring command
  `driver_check` / `driver_setup` / `driver_teardown` di gui.rs (Start-
  Process powershell -Verb RunAs) + overlay di `ui/src/main.ts`
  (tambal "Pasang driver" saat `\\.\NDISLWF` gagal dibuka). 
- **Pelajaran PowerShell 5.1**: method-chain multi-line
  `if (-not [type]::GetCurrent()\n .IsInRole(...))` = ParserError di PS 5.1
  (guest); semua skrip driver diubah ke helper `Assert-Admin` one-line.
- **Guest test end-to-end (R16)** ✅ 2026-11-07: bundel 9 berkas
  `l2split\driver\` (sys/inf/cat hasil build baru + 5 skrip) → guest
  `C:\l2split` via vmrun → wrapper `l2inst.cmd` → `install.ps1`` PASS:
  testsigning Yes, pnputil install success (oem10.inf), layanan NDISLWF
  **RUNNING**, binding **True** di Ethernet0 + Ethernet1. Lanjut: app CLI
  `l2split.exe driver` → `driver: TERBUKA, ABI v1.0 (magic ok)`, enum 2
  adapter (MAC 7F-40-7E/7F-40-74), `stats` passthrough rx 58k/tx 86k, 0
  dropped. `cargo build --release` exit 0 (bin `l2split.exe`).
- **Push publik** ✅ 2026-11-07: repo **github.com/obvirm/l2split** (public,
  MIT, 38 berkas — biner .sys/.cat/inf tidak dirilis: source-first, user
  membangun driver sendiri dari repo ini; tagline cara kerja + README +
  topics rust/ndis/windows-driver/split-tunneling/vpn/network-tools/tauri).
- **Aturan proyek suci & indie** (2026-11-07, koreksi keras dari user):
  repo publik TIDAK BOLEH menyebut/membandingkan/menautkan proyek atau brand
  lain (saingan, shim, "pengganti X") — narasi harus berdiri sendiri (indie).
  Cross-check = bukti grep yang benar (pola huruf kecil, tanpa flag nyasar)
  yang ditampilkan, bukan klaim "sudah dicek". Tidak boleh bohong munafik ke
  user. Aturan ini dipasang permanen di `C:\Users\X\.claude\CLAUDE.md`
  (global).
- **Scrub history** ✅ 2026-11-07: temuan user — nama/kata pihak-ketiga
  bocor di tree DAN history + pesan commit (di kedua repo publik ini:
  sabagian produk filter paket komersial, nama driver komersial lama, dan
  proyek sejenis lain; termasuk di README, docs, komentar kode, string UI).
  Pembersihan:
  1. tree ndislwf — README shim diringkas jadi "ndisapi.dll compat shim
     untuk tool legacy"; judul ROADMAP → "filosofi dan arah"; M1/M5 rewrite;
     pointer typedef di `src/shim/ndisapi.cpp` diganti netral
     (`..._ETH_Packet` → `NL_ETH_Packet`); `tests/shimprobe.c` komentar +
     arg nama → `"nlwf"` (arg diabai shim, perilaku tetap); komentar RATE
     di `filter.c`.
  2. tree l2split — blok perbandingan dihapus dari README, string UI
     dibereskan; satu-satunya commit di-amend (RELEASE.md pindah ke luar
     repo) → force-push bersih.
  3. history ndislwf — `git filter-branch -f --tree-filter` (PS 5.1 script
     di luar repo; mengganti nama-nama pihak-ketiga di atas jadi kata
     netral) atas 40 commit (~44 s), lalu `--msg-filter` (pwsh 7, UTF-8
     aman) atas pesan; `refs/original` dihapus; verifikasi `git log -S` +
     scan `%B` atas `main` dan `--all` pasca-push = 0 temuan. Catatan
     teknik: path script di filter-branch harus forward slash (`E:/temp/`
     — backslash dimakan), scan `--all` sebelum push tetap memuat
     `origin/main` lama sehingga verifikasi yang sah hanya bisa dilakukan
     pasca-push.
  4. tip pasca-rewrite pertama = b0c31c3 → force-push sukses.
  5. **Kebocoran tellur kemudian**: entri log ini sendiri (tertulis malam
     2026-11-07) memasukkan kembali nama-nama pihak-ketiga ke tree
     (commit 45fb414 + 1c90e05) — lingkaran ilat bahwa catatan scrub pun
     harus suci. Fix final: log ini ditulis tanpa satu pun nama pihak
     ketiga; seluruh commit penyusun log diasaring ulang via kedua filter
     sampai kata-kata itu tidak pernah nampak di commit apa pun.
- **GitHub Release v0.1.0** ✅ 2026-11-07: https://github.com/obvirm/l2split/releases/tag/v0.1.0 —
  asset `l2split_0.1.0_x64-setup.exe` (2,204,980 B, sha256
  34d6fbf9ab727a99b806ea03ecb80b25066232bffafc974da5df3c253576be8c).
  Bundel: `npx tauri build` (tauri-cli **2.12.4** dipasang lokal di ui —
  global cargo-tauri 1.6.6 versi v1 dan TIDAK BISA untuk project Tauri 2;
  run dari root crate karena CLI mencari tauri.conf.json di cwd/subfolder,
  bukan parent; frontend `npm run build` dulu karena config tanpa
  beforeBuildCommand). `bundle.resources` pakai prefix `driver/` agar .sys/
  .inf/.cat + 6 skrip ps1/cmd masuk `$INSTALL\driver\` — terverifikasi via
  `7z l` isi installer (9 berkas + app). Alur end-user (notes full English
  di release): setup → SmartScreen Run anyway → wizard in-app → UAC Yes →
  reboot otomatis (testsigning ON + RunOnce) → driver terpasang sendiri.


- **Alih driver l2split → installer paket (v0.1.1)** ✅ 2026-11-07: keputusan
  user — driver kustom hasil build sendiri tertahan masalah sistemik: hanya
  bisa load saat test signing aktif, sedangkan Secure Boot memblokirnya, jadi
  mayoritas user nyata tidak akan bisa memakainya tanpa mengubah BIOS.
  Jalan keluar final: pakai driver packet-filter yang **tertanda WHQL**
  (bukti diverifikasi sendiri: `Get-AuthenticodeSignature` atas berkas .sys
  hasil ekstraksi MSI terisolasi → Signer = Microsoft Windows Hardware
  Compatibility Publisher) dan gratis, sehingga load di Windows 10/11 apa
  adanya tanpa testsigning, tanpa mati Secure Boot, tanpa reboot.
  Berikut port-nya:
  - crate Rust untuk bicara driver, langsung DeviceIoControl ke device
    (nama device = konstanta ABI fungsional, tidak bisa diganti);
  - `src/ndclient.rs` baru: `NdisDevice` (open/adapters/capture_tx/clear/
    set_event/read_batch/send_batch), `SniAdapter` (nama friendly + MAC +
    handle), filter kernel = static filter dst-MAC gateway VPN arah TX,
    action REDIRECT — semantik disimpulkan dari contoh resmi crate
    (passthru/packthru/filter): hanya frame yang match yang ditahan ke
    user-mode, sisanya lewat kernel tanpa disentuh user-mode;
  - `src/engine.rs` ditulis ulang: satu thread pembaca per adapter
    (event Win32 per adapter → baca batch 256 buffer) → keputusan per
    frame (owner exe via tabel port sumber) → whitelist: tulis ulang
    dst-MAC ke gateway LAN + inject ke adapter LAN; sisanya: inject apa
    adanya ke adapter asal. Pijakan API dikonfirmasi dari source crate di
    tempat terisolasi (push/packthru menata request dulu baru kirim begitu
    saja — tidak ada `send_batch` di bawah level request);
  - `helpers.rs` dibuat generik lewat trait `AdapterInfo` (implementasi
    NlAdapter kustom + adapter paket) agar CLI lama tidak pecah;
  - wizard baru: MSI driver **dibungkus sebagai resource** di dalam
    setup.exe (`driver/ndis.msi`, nama netral), instalasi silent
    `msiexec /i … /qn` + UAC sekali, verifikasi device setelahnya —
    testsigning/reboot/RunOnce dikabur;
  - 9 berkas driver lama (.sys/inf/cat + skrip) dihapus dari tree;
  - versi naik 0.1.1, release https://github.com/obvirm/l2split/releases/tag/v0.1.1
    (aset `l2split_0.1.1_x64-setup.exe` 2.41 MiB, terverifikasi `7z l`
    memuat `driver\ndis.msi`).
- **Kejujuran status pengujian**: sampai momen ini l2split **belum** pernah
  di stress test / benchmark / alur one-click end-to-end di mesin asli —
  semua klaim fungsi masih level kode, bukan level hasil uji.
