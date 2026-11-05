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

### M3 Aksi netem dinamis per rule
- `NL_ACTION = ALLOW|BLOCK|MIRROR|REDIRECT|DELAY|DROP_PCT|RATE_LIMIT`.
- `delay_ms`, `drop_pct`, `rate_kbps` per rule di eval kernel; implement: queue deadline (KeSetTimer/DPC) untuk delay, counter probabilitas untuk drop_pct, token-bucket untuk rate.

### M4 VM mini BPF-style (rule terkompilasi)
- Endi rule match bebas-field: user-space mengkompilasi `(match expr) → action` menjadi
  bytecode kelas BPF-c klasik (ld/ldh/jump/ret), driver menyimpan § byte dan evaluator ~200 baris
  (tanpa helper, tanpa reentrancy — di bawah NlFlowLock/MsFilterLock).
- `NL_IOCTL_PROGRAM`: set program per adapter + fallback ke policy table lama.
- Ketika program aktif, tabel rule 16-slot hanya fallback keceraman (compat).

### M5 Mirror + redirect + shim compat
- MIRROR: salin frame ke adapter lain/mon-mode (telaah pairing adapters); REDIRECT: kirim ulang ke NEIGHBOR adapter.
- `nllegacy.dll` shim: mirror API legacy filter (AdaptersEnumerateGetNative, PacketsSend, PacketsGet) → panggilan pintas ADI nlbridge — tool lama tanpa recompilasi.

### M6 Hardening produksi
- Sertifikat produksi (bukan test-sign), WHQL attestation jika perlu.
- Fuzz ioctl (FuzzRing), assertion off, pool verifier hot path, BSOD-0 target di chaos 10 menit.

## Keputusan ABI yang terkunci
- `nl_policy_t` 456 B TIDAK diubah (kompat); counter baru hanya via ioctl tambahan.
- Semua ioctl METHOD_BUFFERED; codes: 0x89002000 + (fn-0x800)<<2; INJECT_RX=0x8900202C (0x80b), RULEHITS=0x89002030 (0x80c); next free = 0x89002034 ke atas (0x80d).
- Struct ring entry 1524 B tidak diubah (kunci ABI shm).
