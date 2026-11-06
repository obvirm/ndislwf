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

static void
dbgPrint(const char *label)
{
    nl_debug_state_t d;
    if (nl_debug_state(&d) == 0)
    {
        printf("      [dbg] %s: gen=%llu pol(en=%u cnt=%u) ex(en=%u cnt=%u) "
               "vm(a=%u en=%u cnt=%u) mir=%u dly=%u\n",
               label,
               (unsigned long long)d.flow_gen,
               d.policy_enabled, d.policy_count,
               d.ex_enabled, d.ex_count,
               d.vm_active, d.vm_en, d.vm_count,
               d.mirror_pairs, d.inject_queue_len);
    }
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
    dbgPrint("setelah set");
    Sleep(50);
    dbgPrint("setelah sleep 50");

    ret = injectN(500, NL_PROTO_UDP, 9999, MAC_BC);   /* harus drop */
    ret += injectN(200, NL_PROTO_UDP, 5678, MAC_BC);  /* must not */
    ret += injectN(100, 0, 0, MAC_BC);                /* ARP must not */
    Sleep(100);
    dbgPrint("setelah inject");
    (void)nl_stats(NULL, &b);
    dbgPrint("setelah stats");

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
tVm(void)
/*++  M4 vm: program BPF klasik (rule terkompilasi) lewat NL_IOC_PROGRAM.
      Program1: BLOCK bila dmac[0..1]==0x0E0B (MAC_TST) ATAU dport==46005;
      else ALLOW. Program2: DELAY 300 ms bila dport==46006; else ALLOW.
      VM aktif menggantikan semua tabel; Count=0 mematikan. --*/
{
    nl_vm_prog_t    p;
    nl_rule_hits_t  h0, h1;
    nl_delay_stat_t d0, d1;
    int ret, bad;

    /* ---- program 1: blok via MAC rule + port rule ---- */
    memset(&p, 0, sizeof p);
    p.count   = 6;
    p.enabled = NL_POLICY_EN_TX | NL_POLICY_EN_RX;
    p.insns[0].code = NL_VM_CL_LD | NL_VM_SZ_H | NL_VM_MD_ABS;
    p.insns[0].k    = 0;            /* A = mac[0..1] big-endian */
    p.insns[1].code = NL_VM_CL_JMP | NL_VM_JEQ;
    p.insns[1].jt   = 3; p.insns[1].jf = 0;
    p.insns[1].k    = 0x0E0B;       /* == 0E:0B:.. (MAC_TST) → [5] BLOCK */
    p.insns[2].code = NL_VM_CL_LD | NL_VM_SZ_H | NL_VM_MD_ABS;
    p.insns[2].k    = 36;           /* A = dport UDP */
    p.insns[3].code = NL_VM_CL_JMP | NL_VM_JEQ;
    p.insns[3].jt   = 1; p.insns[3].jf = 0;
    p.insns[3].k    = 46005;        /* == 46005 → [5] BLOCK */
    p.insns[4].code = NL_VM_CL_RET | NL_VM_RTK;
    p.insns[4].k    = NL_ACTION_ALLOW;
    p.insns[5].code = NL_VM_CL_RET | NL_VM_RTK;
    p.insns[5].k    = NL_ACTION_BLOCK;

    ret = nl_vm_set(&p);
    printf("      vm: nl_vm_set ret=%d\n", ret);
    CHECK("vm: set program1", ret == 0);
    Sleep(50);

    memset(&h0, 0, sizeof h0);
    (void)nl_rule_hits(&h0);
    bad  = injectN(100, NL_PROTO_UDP, 12345, MAC_TST);   /* mac rule → block */
    bad += injectN(100, NL_PROTO_UDP, 46005, MAC_BC);    /* port rule → block */
    bad += injectN(100, NL_PROTO_UDP, 54321, MAC_BC);    /* tidak match → pass */
    Sleep(200);
    p.count = 0;
    (void)nl_vm_set(&p);
    memset(&h1, 0, sizeof h1);
    (void)nl_rule_hits(&h1);
    {
        uint64_t hv = h1.hits[0] - h0.hits[0];
        uint64_t bx = h1.block_tx - h0.block_tx;
        printf("      vm A: hit=%llu blockTx=%llu bad=%d\n",
               (unsigned long long)hv, (unsigned long long)bx, bad);
        CHECK("vm A: block dmac/port", ret == 0 && bad == 0 && hv == 200 && bx == 200);
    }

    /* ---- program 2: DELAY 300ms pada dport 46006 ---- */
    memset(&p, 0, sizeof p);
    p.count   = 4;
    p.enabled = NL_POLICY_EN_TX;
    p.insns[0].code = NL_VM_CL_LD | NL_VM_SZ_H | NL_VM_MD_ABS;
    p.insns[0].k    = 36;           /* A = dport */
    p.insns[1].code = NL_VM_CL_JMP | NL_VM_JEQ;
    p.insns[1].jt   = 1; p.insns[1].jf = 0;
    p.insns[1].k    = 46006;        /* == 46006 → [3] DELAY */
    p.insns[2].code = NL_VM_CL_RET | NL_VM_RTK;
    p.insns[2].k    = NL_ACTION_ALLOW;
    p.insns[3].code = NL_VM_CL_RET | NL_VM_RTK;
    p.insns[3].k    = NL_ACTION_DELAY | (300u << 8);

    memset(&d0, 0, sizeof d0);
    (void)nl_delay_stat(&d0);
    ret = nl_vm_set(&p);
    printf("      vm: nl_vm_set (delay) ret=%d\n", ret);
    CHECK("vm: set program2", ret == 0);
    injectN(10, NL_PROTO_UDP, 46006, MAC_BC);
    Sleep(80);
    (void)nl_delay_stat(&d1);
    p.count = 0;
    (void)nl_vm_set(&p);
    printf("      vm B: antrean delay300: pending=%llu (harap >=10)\n",
           (unsigned long long)d1.pending);
    CHECK("vm B: delay masuk antrean", d1.pending >= 10);
    Sleep(400);   /* due 300 ms lewat */
    memset(&d1, 0, sizeof d1);
    (void)nl_delay_stat(&d1);
    printf("      vm B matang: pending=%llu fired-delta=%llu (harap 0, >=10)\n",
           (unsigned long long)d1.pending,
           (unsigned long long)(d1.fired - d0.fired));
    CHECK("vm B: matang terhitung", d1.pending == 0 && (d1.fired - d0.fired) >= 10);

    {
        nl_policy_t q;
        memset(&q, 0, sizeof q);
        (void)nl_policy_set(&q);   /* pastikan tabel lama mati */
    }
    return 1;
}

static void
udpTx(uint32_t n)
/*++ anchor TX nyata: socket UDP broadcast (pola tCapture). --*/
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { return; }
    {
        SOCKET sd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sd != INVALID_SOCKET)
        {
            struct sockaddr_in sa;
            const char pay[24] = "nlstress-mirror-probe";
            DWORD br = 1;
            uint32_t i;
            (void)setsockopt(sd, SOL_SOCKET, SO_BROADCAST, (char *)&br, sizeof br);
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(47601);
            sa.sin_addr.s_addr = 0xFFFFFFFFu;   /* broadcast */
            for (i = 0; i < n; ++i)
            {
                (void)sendto(sd, pay, sizeof pay, 0, (SOCKADDR *)&sa, sizeof sa);
            }
            closesocket(sd);
        }
    }
    WSACleanup();
}

static int
drainAdpt(uint32_t idx, nl_ring_entry_t rb[], int max)
{
    int rn = 0;
    int e = nl_read_ring(&idx, rb, max, &rn);
    if (e != 0) { return -1; }
    return rn;
}

static int
tMirror(void)
/*++  M5: MIRROR TX + REDIRECT inject-to.
      Redirect: nl_inject_to ret=0 + counter injected naik (NlInjectTxCore).
      Mirror: nl_mirror_set(src->dst, TX) — TX socket nyata di src harus
      muncul sebagai entri NL_RING_TX di ring internal dst; nircmap off
      (flags=0) menghentikannya. Mirror ke diri sendiri ditolak (noop). --*/
{
    nl_adapter_t ad[NL_MAX_ADAPTERS];
    nl_ring_entry_t rb[64];
    nl_stats_t s0, s1;
    nl_mirror_cfg_t m;
    int n = 0, r, rn0, rn1, rn2;
    uint32_t src, dst;

    memset(ad, 0, sizeof ad);
    r = nl_enum(ad, NL_MAX_ADAPTERS, &n);
    CHECK("mirror: enum", r == 0 && n >= 1);
    if (r != 0 || n < 1) { return 0; }
    src = ad[0].index;
    dst = (n >= 2) ? ad[1].index : src;
    printf("      mirror: src=%u dst=%u%s\n", src, dst,
           (n >= 2) ? "" : "  (1 adapter: dst==src)");

    /* ---- REDIRECT: inject_to ret + counter injected ---- */
    memset(&s0, 0, sizeof s0); (void)nl_stats(&src, &s0);
    {
        uint8_t f[NL_FRAME_MAX];
        uint32_t len = buildFrame(f, 84, NL_PROTO_UDP, 46111, MAC_BC, 9);
        r = nl_inject_to(dst, f, len);
    }
    printf("      redirect: nl_inject_to ret=%d\n", r);
    CHECK("mirror: inject_to diterima", r == 0);
    Sleep(100);
    memset(&s1, 0, sizeof s1); (void)nl_stats(&src, &s1);
    printf("      redirect: injected %llu -> %llu\n",
           (unsigned long long)s0.injected, (unsigned long long)s1.injected);
    CHECK("mirror: inject_to dihitung kernel", s1.injected > s0.injected);

    /* ---- baseline: ring dst harus kosong sebelum mirror ---- */
    rn0 = drainAdpt(dst, rb, 64);
    printf("      mirror: baseline dst ring n=%d\n", rn0);

    /* ---- mirror self ditolak (noop; tak boleh crash) ---- */
    memset(&m, 0, sizeof m);
    m.src_index = src;
    m.dst_index = src;
    m.flags     = NL_MIRROR_TX;
    r = nl_mirror_set(&m);
    CHECK("mirror: set self noop ret=0", r == 0);

    /* ---- mirror src->dst TX ON ---- */
    memset(&m, 0, sizeof m);
    m.src_index = src;
    m.dst_index = dst;
    m.flags     = NL_MIRROR_TX;
    r = nl_mirror_set(&m);
    printf("      mirror: nl_mirror_set(tx) ret=%d\n", r);
    CHECK("mirror: set ON", r == 0);

    udpTx(12);
    Sleep(300);
    rn1 = drainAdpt(dst, rb, 64);
    printf("      mirror: dst ring setelah ON n=%d flags0=0x%X len0=%u\n",
           rn1, (rn1 > 0) ? rb[0].flags : 0, (rn1 > 0) ? rb[0].len : 0);
    if (n >= 2)
    {
        CHECK("mirror: TX dst menerima", rn1 > 0);
        CHECK("mirror: entri flag TX", (rb[0].flags & NL_RING_TX) != 0);
    }
    else
    {
        /* 1 adapter: self-pair = noop kernel → tak ada entri baru. */
        CHECK("mirror: self-pair noop (ring tetap kosong)", rn1 == 0);
    }

    /* ---- mirror OFF: dst berhenti menerima ---- */
    memset(&m, 0, sizeof m);
    m.src_index = src;
    m.dst_index = dst;
    m.flags     = 0;
    r = nl_mirror_set(&m);
    CHECK("mirror: set OFF", r == 0);
    Sleep(50);
    udpTx(12);
    Sleep(300);
    rn2 = drainAdpt(dst, rb, 64);
    printf("      mirror: dst ring setelah OFF n=%d (baseline0=%d)\n", rn2, rn0);
    if (n >= 2) { CHECK("mirror: OFF menghentikan", rn2 == 0); }

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

static void nl_reset_all(void);

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
    nl_reset_all();
    return (r == 0 && s.injected > 0) ? 1 : 0;
}

/* ---------------- fuzz ioctl (M6) ---------------- */

static volatile LONG g_fuzzStop = 0;

static uint32_t
fzCode(uint32_t fn)
{
    return 0x89000000u | (fn << 2);
}

static void
fzRaw(void *arg)
{
    uint32_t seed = (uint32_t)(uintptr_t)arg;
    uint32_t buf[1030];   /* lebih besar dari ioctl terbesar (1524 B) */
    uint32_t obuf[520];
    uint32_t k, c = 0;
    while (!g_fuzzStop) {
        uint32_t fn, code, inlen, outlen;
        uint32_t *inb = buf;
        fn = 0x800u + (smux(&seed) % 0x14u);
        if (smux(&seed) & 4) fn = 0x840u + (smux(&seed) % 0x7C0u);
        code = fzCode(fn);
        inlen = (smux(&seed) & 1) ? (smux(&seed) % 3200u) : 0u;
        for (k = 0; k < (inlen + 3) / 4; ++k) buf[k] = smux(&seed);
        outlen = smux(&seed) % 2049u;
        if (inlen == 0 && (smux(&seed) & 1)) inb = NULL;
        (void)nl_ioctl_raw(code, inb, inlen, obuf, outlen, NULL);
        ++c;
    }
    printf("      fuzz raw: %llu siklus\n", (unsigned long long)c);
}

static void
fzVal(void *arg)
{
    uint32_t seed = (uint32_t)(uintptr_t)arg + 7;
    nl_policy_ex_t ex;
    nl_vm_prog_t vm;
    uint8_t data[NL_FRAME_MAX];
    uint32_t k, c = 0;
    while (!g_fuzzStop) {
        nl_mirror_cfg_t mir;
        mir.src_index = smux(&seed) % 40u;
        mir.dst_index = smux(&seed) % 40u;
        mir.flags = smux(&seed) % 16u;
        (void)nl_mirror_set(&mir);

        memset(&vm, 0, sizeof vm);
        vm.count = (smux(&seed) % 10u == 0) ? (smux(&seed) % 200u)
                                            : (smux(&seed) % 64u);
        vm.enabled = smux(&seed) & 3u;
        for (k = 0; k < (uint32_t)vm.count; ++k) {
            vm.insns[k].code = (uint16_t)smux(&seed);
            vm.insns[k].jt   = (uint8_t)smux(&seed);
            vm.insns[k].jf   = (uint8_t)smux(&seed);
            vm.insns[k].k    = smux(&seed);
        }
        (void)nl_vm_set(&vm);

        memset(&ex, 0, sizeof ex);
        ex.count = smux(&seed) % 20u;
        ex.enabled = smux(&seed) & 3u;
        for (k = 0; k < (uint32_t)ex.count; ++k) {
            ex.rules[k].r.proto = (uint8_t)smux(&seed);
            ex.rules[k].r.match = smux(&seed) & 1u;
            ex.rules[k].r.ip    = smux(&seed);
            ex.rules[k].r.mask  = smux(&seed);
            ex.rules[k].r.port_min = (uint16_t)smux(&seed);
            ex.rules[k].r.port_max = (uint16_t)smux(&seed);
            memset(ex.rules[k].r.dst_mac, (int)(smux(&seed) & 0xFF), 6);
            ex.rules[k].action   = smux(&seed) % 8u;
            ex.rules[k].delay_ms = smux(&seed) % 70000u;
            ex.rules[k].drop_pct = smux(&seed) % 140u;
            ex.rules[k].rate_kbps = smux(&seed) % 0x80000000u;
        }
        (void)nl_policy_ex_set(&ex);

        (void)nl_inject(data, 0);
        (void)nl_inject(data, NL_FRAME_MAX + 1);
        (void)nl_inject_to(smux(&seed) % 40u, data, NL_FRAME_MAX);
        (void)nl_inject_rx(data, smux(&seed) % (NL_FRAME_MAX + 1u));
        ++c;
    }
    printf("      fuzz struct: %llu siklus\n", (unsigned long long)c);
}

static void
fzSend(void *arg)
{
    WSADATA wsa;
    SOCKET sd;
    struct sockaddr_in sa;
    DWORD br = 1;
    const char pay[100] = "nlstress-fuzz-load";
    ULONGLONG c = 0;
    (void)arg;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd == INVALID_SOCKET) { WSACleanup(); return; }
    (void)setsockopt(sd, SOL_SOCKET, SO_BROADCAST, (char *)&br, sizeof br);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(47401);
    sa.sin_addr.s_addr = 0xFFFFFFFFu;
    for (c = 0; !g_fuzzStop; ++c) {
        (void)sendto(sd, pay, sizeof pay, 0, (SOCKADDR *)&sa, sizeof sa);
        if ((c & 0x7F) == 0) Sleep(0);
    }
    closesocket(sd);
    WSACleanup();
    printf("      fuzz send: %llu paket\n", c);
}

static int
tFuzzN(int seconds, int kind)
{
    uintptr_t t1 = 0, t2 = 0, t3 = 0;
    nl_stats_t s;
    int r = 9;

    g_fuzzStop = 0;
    if (kind & 1) t1 = _beginthread(fzRaw, 0, (void *)11);
    if (kind & 2) t2 = _beginthread(fzVal, 0, (void *)12);
    if (kind & 4) t3 = _beginthread(fzSend, 0, (void *)13);
    Sleep(1000ull * seconds);
    g_fuzzStop = 1;
    Sleep(400);
    (void)t1; (void)t2; (void)t3;

    memset(&s, 0, sizeof s);
    r = nl_stats(NULL, &s);
    printf("      fuzz%d %d ms; stats ret=%d rx=%llu tx=%llu inj=%llu drop=%llu\n",
           kind, seconds * 1000, r,
           (unsigned long long)s.rx_total, (unsigned long long)s.tx_total,
           (unsigned long long)s.injected, (unsigned long long)s.dropped);
    CHECK("fuzz: driver tetap hidup (stats ok)", r == 0);
    nl_reset_all();
    return (r == 0) ? 1 : 0;
}

static int
tFuzz(int seconds)
{
    return tFuzzN(seconds, 7);
}

static void
nl_reset_all(void)
{
    nl_policy_t p;
    nl_policy_ex_t ex;
    nl_vm_prog_t vm;
    nl_mirror_cfg_t mir;
    memset(&p, 0, sizeof p);
    memset(&ex, 0, sizeof ex);
    memset(&vm, 0, sizeof vm);
    memset(&mir, 0, sizeof mir);
    (void)nl_vm_set(&vm);
    (void)nl_policy_ex_set(&ex);
    (void)nl_policy_set(&p);
    (void)nl_mirror_set(&mir);
    (void)nl_set_mode(g_adapter, 0);
    (void)nl_clear();
}

static int
tFull(void)
{
    int rc;
    printf("=== (1/13) version & enum ===\n");
    if (!tVersion()) { printf("GAGAL early\n"); return 3; }
    nl_reset_all();
    (void)tEnum();
    (void)tStats();
    printf("=== (2/13) capture 3s ===\n");
    (void)tCapture(3);
    printf("=== (3/13) inject 4000 ===\n");
    (void)tInject(4000);
    printf("=== (4/13) policy blok TX ===\n");
    (void)tPolicy();
    (void)tPolicy2();
    printf("=== (5/13) inject RX ===\n");
    (void)tInjectRx();
    printf("=== (6/13) hit per rule ===\n");
    (void)tRuleHits();
    printf("=== (7/13) policy blok RX ===\n");
    (void)tPolicyRx();
    printf("=== (8/13) netem (M3) ===\n");
    (void)tNetem();
    printf("=== (9/13) vm (M4) ===\n");
    (void)tVm();
    printf("=== (10/13) mirror (M5) ===\n");
    (void)tMirror();
    printf("=== (11/13) shm event 3s ===\n");
    (void)tShm(3);
    printf("=== (12/13) bench drain 2+2s ===\n");
    (void)tBench(2);
    printf("=== (x/13) chaos 20s ===\n");
    (void)tChaos(20);
    printf("=== (13/13) fuzz ioctl (M6) ===\n");
    (void)tFuzz(8);
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
    else if (!strcmp(cmd, "vm"))      r = tVm()        ? 0 : 11;
    else if (!strcmp(cmd, "mirror"))  r = tMirror()    ? 0 : 12;
    else if (!strcmp(cmd, "shm"))     r = tShm(atoi(argv[2]));
    else if (!strcmp(cmd, "bench"))   r = tBench(atoi(argv[2]));
    else if (!strcmp(cmd, "chaos"))   r = tChaos(atoi(argv[2]));
    else if (!strcmp(cmd, "fuzz"))    r = tFuzz(atoi(argv[2])) ? 0 : 13;
    else if (!strcmp(cmd, "fuzzraw")) r = tFuzzN(atoi(argv[2]), 1) ? 0 : 13;
    else if (!strcmp(cmd, "fuzzval")) r = tFuzzN(atoi(argv[2]), 6) ? 0 : 13;
    else if (!strcmp(cmd, "fuzz3"))   r = tFuzzN(atoi(argv[2]), 3) ? 0 : 13;
    else if (!strcmp(cmd, "fuzz5"))   r = tFuzzN(atoi(argv[2]), 5) ? 0 : 13;
    else if (!strcmp(cmd, "reset"))   { nl_reset_all(); r = 0; }
    else if (!strcmp(cmd, "full"))    r = tFull();
    else { printf("mode tidak dikenal: %s\n", cmd); r = 9; }
    nl_close();
    printf("EXIT %d\n", r);
    return r;
}
