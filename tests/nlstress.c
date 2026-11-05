/*
 * nlstress.c -- harness benchmark & stress drive ndislwf.sys (via nlbridge.dll).
 *
 * Pemakaian:
 *   nlstress version
 *   nlstress enum
 *   nlstress stats
 *   nlstress capture <detik>        (butuh traffic latar dari runner)
 *   nlstress inject <N>
 *   nlstress policy
 *   nlstress shm <detik>            (butuh traffic latar dari runner)
 *   nlstress chaos <detik>
 *   nlstress full                   (butuh traffic latar dari runner)
 *
 * Kode keluar 0 = semua lulus pada yang dijalankan.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "nlbridge.h"
#pragma comment(lib, "nlbridge.lib")

C_ASSERT(sizeof(nl_adapter_t)    == 544);
C_ASSERT(sizeof(nl_stats_t)      == 56);
C_ASSERT(sizeof(nl_ring_entry_t) == 1524);
C_ASSERT(sizeof(nl_inject_t)     == 1524);
C_ASSERT(sizeof(nl_mode_t)       == 8);
C_ASSERT(sizeof(nl_policy_t)     == 456);
C_ASSERT(sizeof(nl_shm_hdr_t)    == 96);
C_ASSERT(NL_SHM_BYTES          == (128u + 2u * 1024u * 1524u));

static uint32_t g_adapter = 0;
static int      g_pass = 0, g_fail = 0;

#define PASS(name)   do { printf("[PASS] %s\n", name);  ++g_pass; } while (0)
#define FAIL(name)   do { printf("[FAIL] %s\n", name);  ++g_fail; } while (0)
#define CHECK(name, cond) do { if (cond) PASS(name); else FAIL(name); } while (0)

static void
retprint(const char *name, int r)
{
    printf("      %-30s ret=%d last=%lu\n", name, r, (unsigned long)GetLastError());
}

static ULONGLONG
qps(void)
{
    LARGE_INTEGER f, n;
    QueryPerformanceCounter(&n);
    QueryPerformanceFrequency(&f);
    return (ULONGLONG)((n.QuadPart * 1000) / f.QuadPart);  /* ms */
}

/* PRNG kecil (splitmix32) -- menghindari rand/Windows API. */
static uint32_t
smux(uint32_t *s)
{
    uint32_t z = (*s += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

/* ---------- frame builder ------------------------------------------------- */

static uint8_t MAC_BC[6]  = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static uint8_t MAC_TST[6] = { 0x0E,0x0B,0x0A,0x01,0x02,0x03 };

/* Frame Ethernet[type] + ARP/IPv4; len 60..1514, filler; ports byte order
 * jaringan (big endian) seperti di kawat. */
static uint32_t
buildFrame(uint8_t *d, uint32_t len, uint8_t ipNext, uint16_t dport,
           const uint8_t *dmac, uint32_t seed)
{
    uint32_t i, off;
    if (len < 60) len = 60;
    if (len > NL_FRAME_MAX) len = NL_FRAME_MAX;
    memset(d, 0, NL_FRAME_MAX);
    memcpy(d, dmac, 6);
    d[6]=0x52; d[7]=0x54; d[8]=0x00; d[9]=0xB1; d[10]=0x42; d[11]=0x7F;
    if (ipNext == 0) {
        d[12]=0x08; d[13]=0x06;                          /* ARP */
        off = 14;
    } else {
        d[12]=0x08; d[13]=0x00;                          /* IPv4 */
        d[14]=0x45; d[15]=0x00;
        d[16]=(uint8_t)((len-14)>>8); d[17]=(uint8_t)(len-14);
        d[18]=(uint8_t)(seed>>24); d[19]=(uint8_t)seed;
        d[20]=0x00; d[21]=0x00;
        d[22]=0x40; d[23]=ipNext;
        d[24]=0; d[25]=0;                                /* csum kosong */
        d[26]=10;  d[27]=0; d[28]=0; d[29]=1;            /* sip */
        d[30]=192; d[31]=168; d[32]=204; d[33]=1;        /* dip */
        if (ipNext == NL_PROTO_UDP) {
            d[34]=0x04; d[35]=0xD2;                      /* sport 1234 */
            d[36]=(uint8_t)(dport>>8); d[37]=(uint8_t)dport;
            d[38]=(uint8_t)((len-34)>>8); d[39]=(uint8_t)(len-34);
            d[40]=0; d[41]=0;
        }
        off = 42;
    }
    for (i = off; i + 4 <= len; i += 4)
        d[i] = (uint8_t)(0x5A + ((seed + i) & 0x0F));
    d[len-1] = (uint8_t)(seed ^ 0xA5);
    return len;
}

static int
injectN(uint32_t n, uint8_t ipNext, uint16_t dport, const uint8_t *dmac)
{
    uint8_t buf[NL_FRAME_MAX];
    uint32_t i;
    int bad = 0;
    for (i = 0; i < n; ++i) {
        uint32_t len = buildFrame(buf, 60 + (i % (NL_FRAME_MAX - 60)),
                                  ipNext, dport, dmac, i);
        if (nl_inject(buf, len) != 0) ++bad;
    }
    return bad;
}

/* ---------- tests --------------------------------------------------------- */

static int
tVersion(void)
{
    uint32_t magic = 0, ver = 0;
    int r = nl_version(&magic, &ver);
    printf("      nl_version ret=%d magic=0x%08X ver=0x%08X\n", r, magic, ver);
    CHECK("version: magic + ABI", r == 0 && magic == NL_ABI_MAGIC);
    return r == 0;
}

static int
tEnum(void)
{
    nl_adapter_t ad[NL_MAX_ADAPTERS];
    int count = 0, r;
    memset(ad, 0, sizeof ad);
    r = nl_enum(ad, NL_MAX_ADAPTERS, &count);
    printf("      nl_enum ret=%d count=%d\n", r, count);
    if (r == 0 && count > 0) {
        int i;
        for (i = 0; i < count && i < 4; ++i) {
            printf("      idx=%u medium=%u flags=0x%04X "
                   "mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                   ad[i].index, ad[i].medium, ad[i].flags,
                   ad[i].mac[0], ad[i].mac[1], ad[i].mac[2],
                   ad[i].mac[3], ad[i].mac[4], ad[i].mac[5]);
        }
        g_adapter = ad[0].index;
        PASS("enum: >= 1 adapter");
        return 1;
    }
    FAIL("enum: >= 1 adapter");
    return 0;
}

static int
tStats(void)
{
    nl_stats_t s;
    int r;
    memset(&s, 0, sizeof s);
    r = nl_stats(NULL, &s);
    printf("      nl_stats ret=%d rx=%llu tx=%llu capRX=%llu capTX=%llu "
           "inj=%llu drop=%llu units=%u abi=%u\n", r,
           (unsigned long long)s.rx_total, (unsigned long long)s.tx_total,
           (unsigned long long)s.rx_captured, (unsigned long long)s.tx_captured,
           (unsigned long long)s.injected, (unsigned long long)s.dropped,
           s.active_units, s.abi_ver);
    CHECK("stats: ok + ABI", r == 0 && s.abi_ver == NL_ABI_VERSION);
    return r == 0;
}

static int
tCapture(int seconds)
{
    ULONGLONG t0, end;
    int tx, rx;
    uint64_t bytes = 0;
    nl_stats_t a, b;
    int r0;

    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    (void)nl_stats(NULL, &a);
    r0 = nl_set_mode(g_adapter, NL_MODE_RX | NL_MODE_TX);
    printf("      nl_set_mode ret=%d\n", r0);
    CHECK("capture: set_mode", r0 == 0);
    if (r0 != 0) return 0;

    tx = rx = 0;
    {
        int atx = 0, arx = 0, ltx, lrx, first = 1;
        uint64_t abytes = 0;
        t0 = qps();
        end = t0 + 1000ull * seconds;
        /* trafik stack TX nyata via UDP socket broadcast — garansi
         * FilterSendNetBufferLists dipanggil walau guest idle. */
        {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
            {
                SOCKET sd = socket(AF_INET, SOCK_DGRAM, 0);
                if (sd != INVALID_SOCKET)
                {
                    struct sockaddr_in sa;
                    const char pay[48] = "nlstress-capture-probe";
                    DWORD br = 1;
                    (void)setsockopt(sd, SOL_SOCKET, SO_BROADCAST, (char *)&br, sizeof br);
                    memset(&sa, 0, sizeof sa);
                    sa.sin_family = AF_INET;
                    sa.sin_port = htons(47399);
                    sa.sin_addr.s_addr = 0xFFFFFFFFu;   /* broadcast */
                    (void)sendto(sd, pay, sizeof pay, 0, (SOCKADDR *)&sa, sizeof sa);
                    Sleep(150);
                    (void)sendto(sd, pay, sizeof pay, 0, (SOCKADDR *)&sa, sizeof sa);
                    closesocket(sd);
                }
                WSACleanup();
            }
        }
        while (qps() < end) {
            nl_ring_entry_t rb[32];
            int rn = 0;
            int e = nl_read_ring(NULL, rb, 32, &rn);
            if (e == 0)
            {
                int k;
                for (k = 0; k < rn; ++k)
                {
                    if ((rb[k].flags & NL_RING_TX) != 0) { ++atx; } else { ++arx; }
                    abytes += rb[k].len;
                }
                if (rn > 0 && first)
                {
                    first = 0;
                    printf("      raw: n=%d e0.len=%u e0.flags=0x%X e1.len=%u e1.flags=0x%X\n",
                           rn, rb[0].len, rb[0].flags,
                           (unsigned)(rn > 1 ? rb[1].len : 0),
                           (unsigned)(rn > 1 ? rb[1].flags : 0));
                }
            }
            Sleep(100);
        }
        ltx = lrx = 0;
        (void)nl_drain_ring(&ltx, &lrx, &abytes);
        atx += ltx; arx += lrx;
        tx = atx; rx = arx; bytes = abytes;
    }
    (void)nl_stats(NULL, &b);
    (void)nl_set_mode(g_adapter, 0);

    printf("      capture %d s: drain tx=%d rx=%d bytes=%llu | "
           "capTX delta=%llu capRX delta=%llu tx_total delta=%llu "
           "rx_total delta=%llu\n",
           seconds, tx, rx, (unsigned long long)bytes,
           (unsigned long long)(b.tx_captured - a.tx_captured),
           (unsigned long long)(b.rx_captured - a.rx_captured),
           (unsigned long long)(b.tx_total - a.tx_total),
           (unsigned long long)(b.rx_total - a.rx_total));
    CHECK("capture: frame terdrain", tx > 0 || rx > 0);
    return (tx > 0 || rx > 0) ? 1 : 0;
}

static int
tInject(uint32_t n)
{
    nl_stats_t a, b;
    ULONGLONG t0, t1;
    int bad;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    (void)nl_stats(NULL, &a);
    t0 = qps();
    bad = injectN(n, NL_PROTO_UDP, 54321, MAC_BC);
    t1 = qps();
    (void)nl_stats(NULL, &b);
    {
        unsigned long long dinj = b.injected - a.injected;
        unsigned long long ms = (t1 > t0) ? (t1 - t0) : 1;
        double mbps = (double)((uint64_t)n * 787) * 8.0 * 1000.0 / (1000.0 * (double)ms);
        printf("      inject: N=%u bad=%d deltaInj=%llu dur=%llu ms "
               "pps=%llu estimasi=%.1f Mbps\n",
               n, bad, dinj, ms,
               (unsigned long long)(dinj * 1000 / ms), mbps);
        CHECK("inject: semua OK + counter", bad == 0 && dinj == n);
    }
    return bad == 0 && (b.injected - a.injected) == n;
}

static int
tPolicy(void)
{
    nl_policy_t p;
    nl_stats_t a, b;
    int ret;

    memset(&p, 0, sizeof p);
    p.count = 1;
    p.enabled = NL_POLICY_EN_TX;
    p.rules[0].proto    = NL_PROTO_UDP;
    p.rules[0].match    = NL_MATCH_DST;
    p.rules[0].ip       = 0;
    p.rules[0].mask     = 0;
    p.rules[0].port_min = 9999;
    p.rules[0].port_max = 9999;

    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    (void)nl_stats(NULL, &a);
    ret = nl_policy_set(&p);
    printf("      nl_policy_set (TX UDP dport 9999) ret=%d\n", ret);
    CHECK("policy: set ok", ret == 0);
    Sleep(50);

    ret = injectN(500, NL_PROTO_UDP, 9999, MAC_BC);   /* harus drop */
    ret += injectN(200, NL_PROTO_UDP, 5678, MAC_BC);  /* must not */
    ret += injectN(100, 0, 0, MAC_BC);                /* ARP must not */
    Sleep(100);
    (void)nl_stats(NULL, &b);

    p.enabled = 0;
    (void)nl_policy_set(&p);
    {
        uint64_t ddr = b.dropped  - a.dropped;
        uint64_t din = b.injected - a.injected;
        printf("      policy: inj=%llu (harap 800) drop=%llu (harap 500)\n",
               (unsigned long long)din, (unsigned long long)ddr);
        CHECK("policy: inject delta 800", din == 800);
        CHECK("policy: drop delta 500", ddr == 500);
    }
    return 1;
}

static int
tPolicy2(void)
/*++  path dst-MAC: NlDmacBlocked (lewat NlPolicyEval, bypass flow cache).
    Rule proto ANY dstmac=MAC_TST; inject 300 dstmac TST (drop) + 200 BC. --*/
{
    nl_policy_t p;
    nl_stats_t a, b;
    int ret;

    memset(&p, 0, sizeof p);
    p.count = 1;
    p.enabled = NL_POLICY_EN_TX;
    p.rules[0].proto = NL_PROTO_ANY;
    p.rules[0].match = NL_MATCH_DSTMAC;
    memcpy(p.rules[0].dst_mac, MAC_TST, 6);

    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    (void)nl_stats(NULL, &a);
    ret = nl_policy_set(&p);
    printf("      nl_policy_set (TX dstmac 0E:0B:0A:01:02:03) ret=%d\n", ret);
    CHECK("policy2: set ok", ret == 0);
    Sleep(50);
    ret = injectN(300, NL_PROTO_UDP, 5678, MAC_TST);  /* harus drop */
    ret += injectN(200, NL_PROTO_UDP, 1234, MAC_BC);  /* must not */
    Sleep(100);
    (void)nl_stats(NULL, &b);
    p.enabled = 0;
    (void)nl_policy_set(&p);
    {
        uint64_t din = b.injected - a.injected;
        uint64_t ddr = b.dropped  - a.dropped;
        printf("      policy2: inj=%llu (harap 500) drop=%llu (harap 300)\n",
               (unsigned long long)din, (unsigned long long)ddr);
        CHECK("policy2: delta 500 inject", din == 500);
        CHECK("policy2: delta 300 drop", ddr == 300);
    }
    return 1;
}

static int
injectRxN(uint32_t n, uint8_t ipNext, uint16_t dport, const uint8_t *dmac)
{
    uint8_t buf[NL_FRAME_MAX];
    uint32_t i;
    int bad = 0;
    for (i = 0; i < n; ++i) {
        uint32_t len = buildFrame(buf, 60 + (i % (NL_FRAME_MAX - 60)),
                                  ipNext, dport, dmac, i);
        if (nl_inject_rx(buf, len) != 0) ++bad;
    }
    return bad;
}

static int
tInjectRx(void)
/*++  Inject RX (replay, ioctl v3): 400 frame UDP suap jalur RX; semua ioctl
     OK dan counter injected naik 400; stack tetap hidup sesudahnya. --*/
{
    nl_stats_t a, b;
    int bad;

    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    (void)nl_stats(NULL, &a);
    bad = injectRxN(400, NL_PROTO_UDP, 43521, MAC_BC);
    Sleep(200);
    (void)nl_stats(NULL, &b);
    {
        uint64_t din = b.injected - a.injected;
        printf("      injrx: bad=%d deltaInj=%llu (harap 400) drop=%llu\n",
               bad, (unsigned long long)din,
               (unsigned long long)(b.dropped - a.dropped));
        CHECK("injrx: semua OK + counter", bad == 0 && din == 400);
        CHECK("injrx: stack hidup post", b.active_units > 0);
    }
    return bad == 0 && (b.injected - a.injected) == 400;
}

static int
tRuleHits(void)
/*++  Hit per rule (ioctl v3): rule0 UDP dport 42001, rule1 dstmac MAC_TST.
     100 inj TX dport 42001 BC  -> hits[0] +100 (flow path, idx 0).
     100 inj TX dport 42002 TST -> hits[1] +100 (dstmac menang).
     100 inj TX dport 42003 BC  -> tidak block (hits rontok 0). BlockTx +200. --*/
{
    nl_policy_t p;
    nl_stats_t s;
    nl_rule_hits_t h0, h1;
    int ret;

    memset(&h0, 0, sizeof h0);
    ret = nl_rule_hits(&h0);
    CHECK("hits: ioctl ok", ret == 0);
    if (ret != 0) return 0;

    memset(&p, 0, sizeof p);
    p.count = 2;
    p.enabled = NL_POLICY_EN_TX;
    p.rules[0].proto    = NL_PROTO_UDP;
    p.rules[0].match    = NL_MATCH_DST;
    p.rules[0].ip       = 0;
    p.rules[0].mask     = 0;
    p.rules[0].port_min = 42001;
    p.rules[0].port_max = 42001;
    p.rules[1].proto    = NL_PROTO_ANY;
    p.rules[1].match    = NL_MATCH_DSTMAC;
    memcpy(p.rules[1].dst_mac, MAC_TST, 6);

    ret = nl_policy_set(&p);
    printf("      hits: nl_policy_set (2 rule) ret=%d\n", ret);
    CHECK("hits: set ok", ret == 0);
    Sleep(50);
    (void)injectN(100, NL_PROTO_UDP, 42001, MAC_BC);
    (void)injectN(100, NL_PROTO_UDP, 42002, MAC_TST);
    (void)injectN(100, NL_PROTO_UDP, 42003, MAC_BC);
    Sleep(100);
    p.enabled = 0;
    (void)nl_policy_set(&p);

    memset(&h1, 0, sizeof h1);
    ret = nl_rule_hits(&h1);
    memset(&s, 0, sizeof s);
    (void)nl_stats(NULL, &s);
    {
        uint64_t h0v = h1.hits[0] - h0.hits[0];
        uint64_t h1v = h1.hits[1] - h0.hits[1];
        uint64_t h2v = h1.hits[2] - h0.hits[2];
        uint64_t btx = h1.block_tx - h0.block_tx;
        printf("      hits: r0=%llu r1=%llu r2=%llu blockTx=%llu (harap 100/100/0/200)\n",
               (unsigned long long)h0v, (unsigned long long)h1v,
               (unsigned long long)h2v, (unsigned long long)btx);
        CHECK("hits: rule0 100", h0v == 100);
        CHECK("hits: rule1 100", h1v == 100);
        CHECK("hits: rule2 0",   h2v == 0);
        CHECK("hits: blockTx 200", btx == 200);
    }
    return 1;
}

static int
tPolicyRx(void)
/*++  Blok RX deterministik lewat inject RX (replay): policy EN_RX rule0
     UDP dport 41001. 300 injRX dport 41001 diblok SEBELUM indicate
     (NlInj +400, blockRx +300, hits[0] +300); 100 injRX dport 41002
     melewati policy dan masuk stack. --*/
{
    nl_policy_t p;
    nl_stats_t a, b;
    nl_rule_hits_t h0, h1;
    int ret, bad;

    memset(&h0, 0, sizeof h0);
    (void)nl_rule_hits(&h0);

    memset(&p, 0, sizeof p);
    p.count = 1;
    p.enabled = NL_POLICY_EN_RX;
    p.rules[0].proto    = NL_PROTO_UDP;
    p.rules[0].match    = NL_MATCH_DST;
    p.rules[0].ip       = 0;
    p.rules[0].mask     = 0;
    p.rules[0].port_min = 41001;
    p.rules[0].port_max = 41001;

    memset(&a, 0, sizeof a);
    (void)nl_stats(NULL, &a);
    ret = nl_policy_set(&p);
    printf("      policyrx: nl_policy_set (RX UDP dport 41001) ret=%d\n", ret);
    CHECK("policyrx: set ok", ret == 0);
    Sleep(50);

    bad  = injectRxN(300, NL_PROTO_UDP, 41001, MAC_BC);  /* harus blok */
    bad += injectRxN(100, NL_PROTO_UDP, 41002, MAC_BC);  /* masuk stack */
    Sleep(200);

    p.enabled = 0;
    (void)nl_policy_set(&p);

    memset(&b, 0, sizeof b);
    (void)nl_stats(NULL, &b);
    memset(&h1, 0, sizeof h1);
    (void)nl_rule_hits(&h1);
    {
        uint64_t din = b.injected - a.injected;
        uint64_t dbr = h1.block_rx - h0.block_rx;
        uint64_t h0v = h1.hits[0] - h0.hits[0];
        printf("      policyrx: bad=%d inj=%llu (harap 400) "
               "blockRx=%llu (harap 300) hits0=%llu (harap 300)\n",
               bad, (unsigned long long)din,
               (unsigned long long)dbr, (unsigned long long)h0v);
        CHECK("policyrx: injrx semua OK", bad == 0);
        CHECK("policyrx: inj delta 400", din == 400);
        CHECK("policyrx: blockRx 300", dbr == 300);
        CHECK("policyrx: hits rule0 300", h0v == 300);
        CHECK("policyrx: stack hidup post", b.active_units > 0);
    }
    return 1;
}

static volatile LONG g_chaosStop;
static void chInject(void *arg);
static void chSend(void *arg);

static uint32_t
setNetem(uint32_t action, uint32_t delayMs, uint32_t dropPct, uint32_t rateKbps)
{
    nl_policy_ex_t ex;
    memset(&ex, 0, sizeof ex);
    ex.count = 1;
    ex.enabled = NL_POLICY_EN_TX | NL_POLICY_EN_RX;
    ex.rules[0].r.proto     = NL_PROTO_ANY;
    ex.rules[0].r.match     = NL_MATCH_DSTMAC;
    memcpy(ex.rules[0].r.dst_mac, MAC_TST, 6);
    ex.rules[0].action   = action;
    ex.rules[0].delay_ms = delayMs;
    ex.rules[0].drop_pct = dropPct;
    ex.rules[0].rate_kbps = rateKbps;
    return (uint32_t)nl_policy_ex_set(&ex);
}

static void
clearNetem(void)
{
    nl_policy_ex_t ex;
    memset(&ex, 0, sizeof ex);   /* count 0 = EX mati, kembali tabel lama */
    (void)nl_policy_ex_set(&ex);
}

static int
tNetem(void)
/*++  M3 aksi netem per rule (tabel EX). Empat tahap, semua rule menyesar
     dst-MAC salah test (0E:0B:0A:01:02:03) sehingga frame bawaan stack
     (lis broadcast) tidak tersapu:
       A) BLOCK   — injex TX 100 TST: hits+100, drop+100; 200 BC lolos.
       B) DROP_PCT 50 — 4000 inj TX: hits dalam dasar statistik ±50%.
       C) DELAY 500ms — 10 inj TX (fase penuh): antrean pending naik lalu
          0 (fired +10) — diukur lewat ioctl DELAYSTAT (deterministik).
       D) RATE 256kbps — inj TX 200 payload ~1500B; frames digetToken,
          yang di atas kapasitas 1-detik di-drop; jumlah hit jelas < 200.
     EX aktif mem-bypass tabel policy lama; setiap tahap memasang tabel
     barus, diakhir EX dimatikan (count=0). --*/
{
    nl_stats_t a, b;
    nl_rule_hits_t h0, h1;
    nl_delay_stat_t d0, d1, d2;
    int ret, bad = 0;

    (void)nl_rule_hits(&h0);
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    (void)nl_stats(NULL, &a);
    memset(&d0, 0, sizeof d0);
    (void)nl_delay_stat(&d0);

    /* ---- tahap A: BLOCK ---- */
    ret = setNetem(NL_ACTION_BLOCK, 0, 0, 0);
    CHECK("netem: ex set (block)", ret == 0);
    bad += injectN(100, NL_PROTO_UDP, 45001, MAC_TST);   /* harus blok */
    bad += injectN(200, NL_PROTO_UDP, 45002, MAC_BC);    /* lolos */
    Sleep(100);

    memset(&h1, 0, sizeof h1);
    (void)nl_rule_hits(&h1);
    {
        uint64_t h0v = h1.hits[0] - h0.hits[0];
        uint64_t ddr = 0;
        nl_stats_t st;
        memset(&st, 0, sizeof st);
        (void)nl_stats(NULL, &st);
        ddr = st.dropped - a.dropped;
        printf("      netem A: hits0=%llu drop=%llu (harap 100,100) bad=%d\n",
               (unsigned long long)h0v, (unsigned long long)ddr, bad);
        CHECK("netem A: block hits 100", h0v == 100);
        CHECK("netem A: kesal drop 100", ddr == 100);
    }

    /* ---- tahap B: DROP_PCT 50% ---- */
    memset(&h1, 0, sizeof h1);
    (void)nl_rule_hits(&h1);
    ret = setNetem(NL_ACTION_DROP_PCT, 0, 50, 0);
    CHECK("netem: ex set (drop50)", ret == 0);
    injectN(4000, NL_PROTO_UDP, 45003, MAC_TST);
    Sleep(200);
    (void)nl_rule_hits(&h1);
    clearNetem();
    {
        uint64_t ddrop = h1.hits[0] - h0.hits[0] - 100;  /* kurangi tahap A */
        printf("      netem B: hit drop50=%llu (harap ~2000)\n",
               (unsigned long long)ddrop);
        CHECK("netem B: drop_pct 50% rata", ddrop >= 1700 && ddrop <= 2300);
    }

    /* ---- tahap C: DELAY 500ms ---- */
    ret = setNetem(NL_ACTION_DELAY, 500, 0, 0);
    CHECK("netem: ex set (delay500)", ret == 0);
    memset(&h0, 0, sizeof h0);
    (void)nl_rule_hits(&h0);
    memset(&d1, 0, sizeof d1);
    ret = nl_delay_stat(&d1);
    CHECK("netem: delaystat ok", ret == 0);
    injectN(10, NL_PROTO_UDP, 45004, MAC_TST);           /* 10 frame ditahan */
    Sleep(200);
    memset(&h1, 0, sizeof h1);
    (void)nl_rule_hits(&h1);
    memset(&d2, 0, sizeof d2);
    (void)nl_delay_stat(&d2);
    {
        uint64_t pend = d2.pending - d1.pending;
        uint64_t delayHits = h1.hits[0] - h0.hits[0];
        printf("      netem C: antre delay500ms: pending=%llu hits=%llu (harap >=10, 10)\n",
               (unsigned long long)pend, (unsigned long long)delayHits);
        CHECK("netem C: pending naik sebelum due", pend >= 10);
        CHECK("netem C: hits delay diantre", delayHits == 10);
    }
    Sleep(1200);   /* 500 due + slack call */
    memset(&d1, 0, sizeof d1);
    (void)nl_delay_stat(&d1);
    {
        uint64_t hd = d1.fired - d2.fired;
        printf("      netem C: matang %llu item; sisa pending=%llu\n",
               (unsigned long long)hd, (unsigned long long)d1.pending);
        CHECK("netem C: fired +10 setelah due", hd >= 10);
        CHECK("netem C: antrean kering", d1.pending == 0);
    }
    clearNetem();

    /* ---- tahap D: RATE 256 kbps ---- */
    memset(&h0, 0, sizeof h0);
    (void)nl_rule_hits(&h0);
    ret = setNetem(NL_ACTION_RATE, 0, 0, 256);
    CHECK("netem: ex set (rate256)", ret == 0);
    injectN(600, NL_PROTO_UDP, 45005, MAC_TST);  /* ~60..259 byte each */
    Sleep(300);
    (void)nl_rule_hits(&h1);
    clearNetem();
    {
        uint64_t drate = h1.hits[0] - h0.hits[0];
        printf("      netem D: hit rate=%llu (harap > 0, < 600, burst 32KB)\n",
               (unsigned long long)drate);
        CHECK("netem D: rate cap", drate < 600 && drate > 0);
    }

    clearNetem();
    {
        nl_policy_t p;
        memset(&p, 0, sizeof p);
        (void)nl_policy_set(&p);   /* bersihkan policy lama yang biasa */
    }
    return 1;
}

static int
tBench(int seconds)
/*++  M2 batch drain async. Dua fase, masing-masing `seconds` detik, 4 thread
     socket memompa frame stack TX nyata (nl_inject bypass capture):
       A) drain READ_RING (copy, batch max 32/ioctl)
       B) drain zero-copy SHM (tail app langsung di shm, event coalesced)
     Diukur: frame terdrain, pps, jumlah wake event, rasio paket-per-wake. --*/
{
    nl_stats_t s0, s1;
    VOID *va;
    volatile nl_shm_hdr_t *hdr;
    uint64_t evh = 0;
    int r, i;
    uintptr_t ths[4];
    ULONGLONG t0, pA, pB, dur;
    LONG64 totalA = 0, totalB = 0;
    UINT wake0, wakeD;

    va = NULL;
    hdr = NULL;
    dur = (ULONGLONG)seconds * 1000u;
    memset(&s0, 0, sizeof s0); memset(&s1, 0, sizeof s1);
    (void)nl_stats(NULL, &s0);
    if (nl_set_mode(g_adapter, NL_MODE_RX | NL_MODE_TX) != 0)
    {
        FAIL("bench: set_mode");
        return 0;
    }

    /* Fase A: drain lama (READ_RING); jangkar load = socket stack TX nyata
     * (nl_inject TX bypass capture, lihat lesson tCapture). */
    g_chaosStop = 0;
    printf("      bench: fase A mulai (mode=%08x)\n", NL_MODE_RX | NL_MODE_TX);
    for (i = 0; i < 4; ++i)
    {
        ths[i] = _beginthread(chSend, 0, (void *)(uintptr_t)(10 + i));
    }
    t0 = qps();
    while (qps() - t0 < dur)
    {
        nl_ring_entry_t rb[32];
        int rn = 0;
        int e = nl_read_ring(NULL, rb, 32, &rn);
        if (e == 0) totalA += rn;
    }
    pA = qps() - t0; if (pA == 0) pA = 1;
    printf("      bench: fase A selesai totalA=%lld ms=%llu\n",
           (long long)totalA, (unsigned long long)pA);

    /* Fase B: drain zero-copy SHM + event coalesced */
    va = VirtualAlloc(NULL, NL_SHM_BYTES, MEM_COMMIT | MEM_RESERVE,
                      PAGE_READWRITE);
    r = (va != NULL) ? nl_shm_map(g_adapter, va, NL_SHM_BYTES) : -1;
    CHECK("bench: map shm", r == 0);
    if (r != 0)
    {
        g_chaosStop = 1;
        VirtualFree(va, 0, MEM_RELEASE);
        (void)nl_set_mode(g_adapter, 0);
        return 0;
    }
    hdr = (volatile nl_shm_hdr_t *)va;
    (void)nl_shm_event(g_adapter, &evh);
    printf("      bench: fase B mulai events=%llu\n", (unsigned long long)evh);
    wake0 = hdr->tx_wake + hdr->rx_wake;
    t0 = qps();
    {
        ULONG rounds = 0;
        /* drain diberi drift (Sleep tiap 8 batch) sengaja: coalescing
         * event tervisible hanya saat backlog menumpuk; consumer instan
         * menghasilkan 1 wake per paket (wasEmpty tiap burst). */
        while (qps() - t0 < dur)
        {
            LONG tt = hdr->tx_tail, th = hdr->tx_head;
            LONG rt = hdr->rx_tail, rh = hdr->rx_head;
            LONG n;
            n = (th - tt) & (NL_RING_ENTRIES - 1);
            if (n != 0) { totalB += n; hdr->tx_tail = th; }
            n = (rh - rt) & (NL_RING_ENTRIES - 1);
            if (n != 0) { totalB += n; hdr->rx_tail = rh; }
            if ((++rounds & 7u) == 0) Sleep(1);
        }
    }
    pB = qps() - t0; if (pB == 0) pB = 1;

    g_chaosStop = 1;
    Sleep(400);
    wakeD = hdr->tx_wake + hdr->rx_wake - wake0;
    (void)nl_shm_unmap(g_adapter);
    (void)nl_set_mode(g_adapter, 0);
    VirtualFree(va, 0, MEM_RELEASE);
    (void)nl_stats(NULL, &s1);

    {
        double ppsA = (double)totalA * 1000.0 / (double)pA;
        double ppsB = (double)totalB * 1000.0 / (double)pB;
        double perwake = (wakeD == 0) ? 0.0 : ((double)totalB / (double)wakeD);
        printf("      bench: A(READ_RING) drain=%lld pps=%.0f | "
               "B(SHM) drain=%lld pps=%.0f wakes=%u paket/wake=%.1f "
               "| cap delta=%llu drop=%llu\n",
               (long long)totalA, ppsA, (long long)totalB, ppsB, wakeD, perwake,
               (unsigned long long)(s1.tx_captured + s1.rx_captured -
                                    s0.tx_captured - s0.rx_captured),
               (unsigned long long)s1.dropped);
        CHECK("bench: drain shm aktif", totalB > 0);
        CHECK("bench: paket/wake >= 5", perwake >= 5.0);
    }
    return 1;
}

static int
tShm(int seconds)
{
    void *va, *va2;
    uint64_t evh = 0;
    HANDLE h;
    int r, evcount = 0;
    volatile nl_shm_hdr_t *hdr;
    ULONGLONG t0, end;

    va = VirtualAlloc(NULL, NL_SHM_BYTES, MEM_COMMIT | MEM_RESERVE,
                      PAGE_READWRITE);
    if (!va) { FAIL("shm: alloc"); return 0; }
    printf("      shm va=%p\n", va);

    (void)nl_set_mode(g_adapter, NL_MODE_RX | NL_MODE_TX);
    r = nl_shm_map(g_adapter, va, NL_SHM_BYTES);
    retprint("nl_shm_map", r);
    CHECK("shm: map", r == 0);
    if (r != 0) { VirtualFree(va, 0, MEM_RELEASE); return 0; }

    /* double-map harus ditolak */
    va2 = VirtualAlloc(NULL, NL_SHM_BYTES, MEM_COMMIT | MEM_RESERVE,
                       PAGE_READWRITE);
    r = nl_shm_map(g_adapter, va2, NL_SHM_BYTES);
    retprint("double-map (harus gagal)", r);
    CHECK("shm: double map ditolak", r != 0);
    if (va2) VirtualFree(va2, 0, MEM_RELEASE);

    r = nl_shm_event(g_adapter, &evh);
    retprint("nl_shm_event", r);
    CHECK("shm: event", r == 0 && evh != 0);
    h = (HANDLE)(uintptr_t)evh;
    hdr = (volatile nl_shm_hdr_t *)va;

    Sleep(100);
    CHECK("shm: magic", hdr->magic == NL_SHM_MAGIC);
    CHECK("shm: adapter idx", (uint32_t)hdr->adapter_index == g_adapter);

    t0 = qps();
    end = t0 + 1000ull * seconds;
    while (qps() < end) {
        if (WaitForSingleObject(h, 100) == WAIT_OBJECT_0) ++evcount;
    }
    {
        int htx = (int)hdr->tx_head, hrx = (int)hdr->rx_head;
        int ttx = (int)hdr->tx_tail, trx = (int)hdr->rx_tail;
        uint64_t cTx = hdr->cap_tx_mirror, cRx = hdr->cap_rx_mirror;
        printf("      shm: events=%d head(tx=%d rx=%d) tail(tx=%d rx=%d) "
               "capTX=%llu capRX=%llu bytes(tx=%llu rx=%llu)\n",
               evcount, htx, hrx, ttx, trx,
               (unsigned long long)cTx, (unsigned long long)cRx,
               (unsigned long long)hdr->tx_bytes_mirror,
               (unsigned long long)hdr->rx_bytes_mirror);
        CHECK("shm: head bergerak", htx > 0 || hrx > 0);
        CHECK("shm: event terbangun", evcount > 0);
        CHECK("shm: tail tidak lompat", ttx <= htx + NL_RING_ENTRIES &&
                                        trx <= hrx + NL_RING_ENTRIES);
    }

    r = nl_shm_unmap(g_adapter);
    retprint("nl_shm_unmap", r);
    CHECK("shm: unmap", r == 0);

    r = nl_set_mode(g_adapter, 0);
    CHECK("shm: set_mode pasca", r == 0);
    VirtualFree(va, 0, MEM_RELEASE);
    return 1;
}

/* =================== chaos threads ==================== */

static volatile LONG g_chaosStop = 0;

static void
chPolicy(void *arg)
{
    uint32_t seed = (uint32_t)(uintptr_t)arg;
    nl_policy_t p;
    ULONGLONG c = 0;
    while (!g_chaosStop) {
        int i, n = 1 + (smux(&seed) % 4);
        memset(&p, 0, sizeof p);
        p.count = (uint32_t)n;
        p.enabled = (smux(&seed) & 1) ? NL_POLICY_EN_TX : 0;
        if (smux(&seed) & 2) p.enabled |= NL_POLICY_EN_RX;
        for (i = 0; i < n; ++i) {
            uint16_t port = (uint16_t)(smux(&seed) & 0xFFFF);
            p.rules[i].proto = NL_PROTO_TCP;
            p.rules[i].match = (smux(&seed) & 1) ? NL_MATCH_SRC : NL_MATCH_DST;
            p.rules[i].ip    = 192 + (smux(&seed) << 8);
            p.rules[i].mask  = (smux(&seed) & 1) ? 0xFFFFFF00u : 0u;
            p.rules[i].port_min = port;
            p.rules[i].port_max = (uint16_t)(port + (smux(&seed) & 7));
            memset(p.rules[i].dst_mac, (int)(smux(&seed) & 0xFF), 6);
        }
        if (nl_policy_set(&p) == 0) ++c;
        Sleep(1);
    }
    printf("      chaos policy: %llu siklus\n", c);
}

static void
chMode(void *arg)
{
    uint32_t seed = (uint32_t)(uintptr_t)arg + 1;
    ULONGLONG c = 0;
    while (!g_chaosStop) {
        uint32_t flags = smux(&seed) & 3;
        if (nl_set_mode(g_adapter, flags) == 0) ++c;
        Sleep(1);
    }
    printf("      chaos mode: %llu siklus\n", c);
}

static void
chMap(void *arg)
{
    ULONGLONG c = 0;
    (void)arg;
    while (!g_chaosStop) {
        void *va = VirtualAlloc(NULL, NL_SHM_BYTES, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
        if (va) {
            int rm = nl_shm_map(g_adapter, va, NL_SHM_BYTES);
            if (rm == 0) {
                uint64_t evh = 0;
                (void)nl_shm_event(g_adapter, &evh);
                Sleep(2);
                (void)nl_shm_unmap(g_adapter);
            }
            VirtualFree(va, 0, MEM_RELEASE);
        }
        ++c;
        Sleep(4);
    }
    printf("      chaos map/unmap: %llu siklus\n", c);
}

static void
chInject(void *arg)
{
    uint8_t buf[NL_FRAME_MAX];
    ULONGLONG c;
    (void)arg;
    for (c = 0; !g_chaosStop; ++c) {
        uint32_t len = buildFrame(buf, 60 + (c % 700),
                                  NL_PROTO_UDP,
                                  (uint16_t)c, MAC_BC, (uint32_t)c);
        (void)nl_inject(buf, len);
        if ((c & 0x3F) == 0) {
            int tx, rx;
            uint64_t b;
            (void)nl_drain_ring(&tx, &rx, &b);
        }
        if ((c & 0x1F) == 0) Sleep(1);
    }
    printf("      chaos inject: %llu frame\n", c);
}

static void
chSend(void *arg)
{
    WSADATA wsa;
    SOCKET sd;
    struct sockaddr_in sa;
    DWORD br = 1;
    const char pay[128] = "nlstress-bench-load";
    ULONGLONG c = 0;
    (void)arg;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd == INVALID_SOCKET) { WSACleanup(); return; }
    (void)setsockopt(sd, SOL_SOCKET, SO_BROADCAST, (char *)&br, sizeof br);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)(47300 + (int)(uintptr_t)arg));
    sa.sin_addr.s_addr = 0xFFFFFFFFu;
    for (c = 0; !g_chaosStop; ++c) {
        (void)sendto(sd, pay, sizeof pay, 0, (SOCKADDR *)&sa, sizeof sa);
        if ((c & 0x3F) == 0) Sleep(0);
    }
    closesocket(sd);
    WSACleanup();
}

static int
tChaos(int seconds)
{
    uintptr_t t1, t2, t3, t4;
    ULONGLONG t0;
    nl_stats_t s;
    int r;

    t0 = qps();
    g_chaosStop = 0;
    t1 = _beginthread(chPolicy, 0, (void *)1);
    t2 = _beginthread(chMode,   0, (void *)2);
    t3 = _beginthread(chMap,    0, (void *)3);
    t4 = _beginthread(chInject, 0, (void *)4);
    Sleep(1000ull * seconds);
    g_chaosStop = 1;
    Sleep(400);
    (void)t1; (void)t2; (void)t3; (void)t4;

    memset(&s, 0, sizeof s);
    r = nl_stats(NULL, &s);
    printf("      chaos %d ms; stats ret=%d rx=%llu tx=%llu capRX=%llu "
           "capTX=%llu inj=%llu drop=%llu units=%u\n",
           (int)(qps() - t0), r,
           (unsigned long long)s.rx_total, (unsigned long long)s.tx_total,
           (unsigned long long)s.rx_captured,
           (unsigned long long)s.tx_captured,
           (unsigned long long)s.injected, (unsigned long long)s.dropped,
           s.active_units);
    CHECK("chaos: stats ok + inject>0", r == 0 && s.injected > 0);
    {
        nl_policy_t p;
        memset(&p, 0, sizeof p);
        (void)nl_policy_set(&p);
        (void)nl_set_mode(g_adapter, 0);
    }
    return (r == 0 && s.injected > 0) ? 1 : 0;
}

static int
tFull(void)
{
    int rc;
    printf("=== (1/10) version & enum ===\n");
    if (!tVersion()) { printf("GAGAL early\n"); return 3; }
    (void)tEnum();
    (void)tStats();
    printf("=== (2/10) capture 3s ===\n");
    (void)tCapture(3);
    printf("=== (3/10) inject 4000 ===\n");
    (void)tInject(4000);
    printf("=== (4/10) policy blok TX ===\n");
    (void)tPolicy();
    (void)tPolicy2();
    printf("=== (5/10) inject RX ===\n");
    (void)tInjectRx();
    printf("=== (6/10) hit per rule ===\n");
    (void)tRuleHits();
    printf("=== (7/10) policy blok RX ===\n");
    (void)tPolicyRx();
    printf("=== (8/10) netem (M3) ===\n");
    (void)tNetem();
    printf("=== (9/10) shm event 3s ===\n");
    (void)tShm(3);
    printf("=== (10/10) bench drain 2+2s ===\n");
    (void)tBench(2);
    printf("=== (x/10) chaos 20s ===\n");
    (void)tChaos(20);
    printf("=== RINGKASAN: pass=%d fail=%d ===\n", g_pass, g_fail);
    rc = (g_fail == 0) ? 0 : 2;
    return rc;
}

int
main(int argc, char **argv)
{
    const char *cmd;
    int r;
    if (argc < 2) { printf("pakai: nlstress <mode> [arg]\n"); return 9; }
    cmd = argv[1];
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: jejak tahan crash farfail */
    if (nl_open() != 0) {
        printf("[FAIL] nl_open gagal last=%lu\n", (unsigned long)GetLastError());
        return 8;
    }
    r = 0;
    if      (!strcmp(cmd, "version")) r = tVersion() ? 0 : 3;
    else if (!strcmp(cmd, "enum"))    r = tEnum()    ? 0 : 3;
    else if (!strcmp(cmd, "stats"))   r = tStats()   ? 0 : 3;
    else if (!strcmp(cmd, "capture")) r = tCapture(atoi(argv[2]));
    else if (!strcmp(cmd, "inject"))  r = tInject((uint32_t)strtoul(argv[2], NULL, 0));
    else if (!strcmp(cmd, "policy"))  r = tPolicy()  ? 0 : 4;
    else if (!strcmp(cmd, "policy2")) r = tPolicy2() ? 0 : 5;
    else if (!strcmp(cmd, "injr"))    r = tInjectRx()  ? 0 : 6;
    else if (!strcmp(cmd, "hits"))    r = tRuleHits()  ? 0 : 7;
    else if (!strcmp(cmd, "policyrx"))r = tPolicyRx()  ? 0 : 8;
    else if (!strcmp(cmd, "netem"))   r = tNetem()     ? 0 : 10;
    else if (!strcmp(cmd, "shm"))     r = tShm(atoi(argv[2]));
    else if (!strcmp(cmd, "bench"))   r = tBench(atoi(argv[2]));
    else if (!strcmp(cmd, "chaos"))   r = tChaos(atoi(argv[2]));
    else if (!strcmp(cmd, "full"))    r = tFull();
    else { printf("mode tidak dikenal: %s\n", cmd); r = 9; }
    nl_close();
    printf("EXIT %d\n", r);
    return r;
}
