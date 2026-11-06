/*
 * nlctl.c -- CLI operasional ndislwf via nlbridge.dll.
 *
 * Pemakaian:
 *   nlctl show                        state kernel + adapter + stats/rulehits/delay
 *   nlctl enum                        daftar adapter (idx, mac, running, nama)
 *   nlctl stats [idx]                 stats global atau per adapter
 *   nlctl mode <idx> <0|rx|tx|txrx>   pasang/cabut capture per adapter
 *   nlctl block  <dir> [proto] [ip[/p]] [port] [mac]   satu rule BLOCK (EX count=1)
 *   nlctl allow  ...                  satu rule ALLOW (passthrough pilih)
 *   nlctl delay  ... <ms>             satu rule DELAY
 *   nlctl drop   ... <pct>            satu rule DROP_PCT
 *   nlctl rate   ... <kbps>           satu rule RATE
 *   nlctl clear                       kosongkan EX/POLICY lama + matikan VM
 *   nlctl reset                       clear + mode 0 semua adapter
 *   nlctl load <file>                 ruleset multi-baris ke tabel EX
 *   nlctl cap <idx> <dir> <out> [s]   rekam ring ke dump (5 s default)
 *   nlctl send <dump> [rx|to <idx>]   replay dump; arah mengikuti rekaman
 *   nlctl mirror <src> <dst> <dir>    pasang/cabut mirror antar adapter
 *   nlctl pcap <dump>                 konversi dump ke pcap (Wireshark)
 *   nlctl replay <pcap> rx|to <idx>   replay pcap Wireshark ke driver
 *   nlctl vm <off|file>               pasang program VM mini-BPF dari teks
 *
 * Format program VM: satu insn per baris; '#' = komentar; label ":nama"
 * atau "nama:".
 * ldxw/ldxh/ldxb/ldw/ldh/ldb + abs/ind/imm/len/mem; st/stx <n>; tax/txa;
 * alu <op> <k|x>; jmp eq|gt|ge|set <k|x> <tgt1> <tgt2> (tgt = nomor insn
 * absolut atau label, hanya maju; tgt1=true tgt2=false); ret
 * a|allow|block|delay <ms>|drop <pct>|rate <kbps>; enabled tx|rx|txrx.
 * Program menimpa SEMUA tabel (EX/POLICY); ldxb abs k = X = byte frame[k].
 *
 * Form rule: <dir> = tx|rx|txrx; [proto] = any|tcp|udp;
 * [ip[/p]] = a.b.c.d[/prefix] (maksudnya dst; prefix bawaan /32);
 * [port] = N atau N-M (dihiraukan bila proto=any); [mac] = aa:bb:cc:dd:ee:ff
 * (menimpa match ip, dst-mac). File ruleset: satu baris satu rule, format
 * "block|allow|delay <dir> [proto] [ip] [port] [mac]" (+ nilai di ujung untuk
 * delay/drop/rate); '#' = komentar; baris kosong dilewati; first-match.
 *
 * Kode keluar: 0 ok, 1 error, 2 salah pemakaian.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "nlbridge.h"
#pragma comment(lib, "nlbridge.lib")

C_ASSERT(sizeof(nl_policy_t)      == 456);
C_ASSERT(sizeof(nl_policy_ex_t)   == 712);
C_ASSERT(sizeof(nl_debug_state_t) == 48);

#define NLCTL_OK    0
#define NLCTL_ERR   1
#define NLCTL_USAGE 2

static nl_adapter_t g_ad[NL_MAX_ADAPTERS];
static int          g_nad = 0;

static int die_nl(const char *what, int r)
{
    fprintf(stderr, "%s: ret=%d last=%lu\n", what, r, (unsigned long)GetLastError());
    return NLCTL_ERR;
}

static int refresh_enum(void)
{
    g_nad = 0;
    if (nl_enum(g_ad, NL_MAX_ADAPTERS, &g_nad) != 0) return die_nl("nl_enum", -1);
    return NLCTL_OK;
}

static void print_ip(uint32_t ip, uint32_t mask)
{
    printf("%u.%u.%u.%u/%u",
           (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
           (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF),
           (unsigned)(
               (mask == 0u) ? 0u :
               (mask == 0xFFFFFFFFu) ? 32u :
               (mask == 0x000000FFu) ? 8u :
               (mask == 0x0000FFFFu) ? 16u :
               (mask == 0x00FFFFFFu) ? 24u : 32u));
}

static const char *dirstr(uint32_t en)
{
    switch (en) {
    case NL_POLICY_EN_TX:                    return "tx";
    case NL_POLICY_EN_RX:                    return "rx";
    case NL_POLICY_EN_TX | NL_POLICY_EN_RX:  return "txrx";
    }
    return "off";
}

static const char *actionstr(uint32_t a)
{
    switch (a) {
    case NL_ACTION_ALLOW:    return "allow";
    case NL_ACTION_BLOCK:    return "block";
    case NL_ACTION_DELAY:    return "delay";
    case NL_ACTION_DROP_PCT: return "drop%";
    case NL_ACTION_RATE:     return "rate";
    }
    return "?";
}

static uint32_t pkt_dword(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int parse_proto(const char *s, uint32_t *out)
{
    if (_stricmp(s, "any") == 0 || strcmp(s, "0") == 0)  { *out = NL_PROTO_ANY; return 0; }
    if (_stricmp(s, "tcp") == 0 || strcmp(s, "6") == 0)  { *out = NL_PROTO_TCP; return 0; }
    if (_stricmp(s, "udp") == 0 || strcmp(s, "17") == 0) { *out = NL_PROTO_UDP; return 0; }
    return -1;
}

static int parse_dir(const char *s, uint32_t *out)
{
    if (_stricmp(s, "tx") == 0)   { *out = NL_POLICY_EN_TX; return 0; }
    if (_stricmp(s, "rx") == 0)   { *out = NL_POLICY_EN_RX; return 0; }
    if (_stricmp(s, "txrx") == 0 ||
        _stricmp(s, "rxtx") == 0 ||
        _stricmp(s, "both") == 0) { *out = NL_POLICY_EN_TX | NL_POLICY_EN_RX; return 0; }
    if (_stricmp(s, "none") == 0 ||
        _stricmp(s, "off") == 0)  { *out = 0; return 0; }
    return -1;
}

static int parse_ip(const char *s, uint32_t *ip, uint32_t *mask)
{
    unsigned a, b, c, d;
    char tail[16];
    uint32_t hostmask = 0xFFFFFFFFu;
    int p = 32;
    /* 4  = a.b.c.d tanpa prefix; 5 = +/N; 6 = ada token asing setelah prefix */
    int n = sscanf(s, "%u.%u.%u.%u/%d%15s", &a, &b, &c, &d, &p, tail);

    if (n <= 3 || n == 6) return -1;
    if (n >= 5) {
        if (p < 0 || p > 32) return -1;
        hostmask = (p == 0) ? 0u : (0xFFFFFFFFu << (32u - (uint32_t)p));
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    {
        uint8_t mb[4];
        mb[0] = (uint8_t)(hostmask >> 24);
        mb[1] = (uint8_t)(hostmask >> 16);
        mb[2] = (uint8_t)(hostmask >> 8);
        mb[3] = (uint8_t)hostmask;
        *ip   = pkt_dword((const uint8_t[4]){ (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d });
        *mask = pkt_dword(mb);
    }
    return 0;
}

static int parse_port(const char *s, uint16_t *pmin, uint16_t *pmax)
{
    unsigned a = 0, b = 0;
    char tail[8];

    if (sscanf(s, "%u-%u%7s", &a, &b, tail) == 2) {
        if (a > 65535 || b > 65535 || a > b) return -1;
        *pmin = (uint16_t)a;
        *pmax = (uint16_t)b;
        return 0;
    }
    if (sscanf(s, "%u%7s", &a, tail) == 1) {
        if (a == 0 || a > 65535) return -1;
        *pmin = (uint16_t)a;
        *pmax = (uint16_t)a;
        return 0;
    }
    if (sscanf(s, "%7s", tail) == 1) return -1;
    *pmin = 0;
    *pmax = 0;
    return 0;
}

static int parse_mac(const char *s, uint8_t *mac)
{
    unsigned v[6];
    char tail[4];
    int i;

    if (sscanf(s, "%x:%x:%x:%x:%x:%x%3s", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], tail) != 6)
        return -1;
    for (i = 0; i < 6; i++)
        if (v[i] > 0xFF) return -1;
    for (i = 0; i < 6; i++)
        mac[i] = (uint8_t)v[i];
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "pemakaian: nlctl <perintah> [argumen]\n"
        "  show | enum | stats [idx]\n"
        "  mode <idx> <0|rx|tx|txrx>\n"
        "  block <dir> [proto] [ip[/p]] [port] [mac]\n"
        "  allow <...> | delay <...> <ms> | drop <...> <pct> | rate <...> <kbps>\n"
        "  clear | reset\n"
        "  load <file>\n"
        "  cap <idx> <tx|rx|txrx> <out.bin> [detik=5]\n"
        "  send <dump.bin> [rx | to <idx>]\n"
        "  mirror <src> <dst> <tx|rx|txrx|off>\n"
        "  pcap <dump.bin>              konversi dump ke pcap Wireshark\n"
        "  replay <pcap> rx             kirim frame pcap ke jalur TCP/IP\n"
        "  replay <pcap> to <idx>       kirim frame pcap ke adapter\n"
        "  vm <off|fmt>                 matikan/pasang program VM (mini-BPF)\n"
        "    fmt: satu insn per baris (# komentar; label \":nama\")\n"
        "    enabled tx|rx|txrx              arah evaluasi (default txrx)\n"
        "    ldw|ldb|ldxw|ldxh|ldxb abs|ind|imm <k>   abs: Data[k]; ind: Data[X+k]\n"
        "    ldw len | ldw mem <n>           A = Len | M[n]\n"
        "    st <n> | stx <n>                M[n] = A | X      tax | txa\n"
        "    alu add|sub|mul|div|or|and|lsh|rsh|neg <k|x>\n"
        "    jmp eq|gt|ge|set <k|x> <tgt1> <tgt2> | jmp a <tgt>\n"
        "         tgt   = nomor insn absolut (mulai 0) atau label; maju saja\n"
        "    ret a|allow|block|delay <ms>|drop <pct>|rate <kbps>\n");
    return NLCTL_USAGE;
}

static uint32_t need_idx(const char *s)
{
    long v = atol(s);
    if ((v <= 0 && s[0] != '0') || strlen(s) == 0) return 0xFFFFFFFFu;
    return (uint32_t)v;
}

static int cmd_show(void)
{
    uint32_t magic = 0, ver = 0;
    nl_stats_t  st;
    nl_rule_hits_t  hits;
    nl_delay_stat_t dly;
    nl_debug_state_t dbg;
    int r, i;

    r = nl_version(&magic, &ver);
    if (r != 0) return die_nl("nl_version", r);
    printf("  version magic=0x%08X ver=0x%08X\n", magic, ver);

    memset(&dbg, 0, sizeof(dbg));
    r = nl_debug_state(&dbg);
    if (r != 0) return die_nl("nl_debug_state", r);
    printf("  kernel: gen=%llu policy(en=%u cnt=%u) ex(en=%u cnt=%u) "
           "vm(a=%u en=%u cnt=%u) mirror=%u injq=%lu\n",
           (unsigned long long)dbg.flow_gen,
           dbg.policy_enabled, dbg.policy_count, dbg.ex_enabled, dbg.ex_count,
           dbg.vm_active, dbg.vm_en, dbg.vm_count, dbg.mirror_pairs,
           (unsigned long)dbg.inject_queue_len);

    memset(&st, 0, sizeof(st));
    r = nl_stats(NULL, &st);
    if (r != 0) return die_nl("nl_stats", r);
    printf("  stats: active=%u abi=%u rx=%llu tx=%llu cap_rx=%llu cap_tx=%llu "
           "inj=%llu drop=%llu\n",
           st.active_units, st.abi_ver,
           (unsigned long long)st.rx_total, (unsigned long long)st.tx_total,
           (unsigned long long)st.rx_captured, (unsigned long long)st.tx_captured,
           (unsigned long long)st.injected, (unsigned long long)st.dropped);

    memset(&hits, 0, sizeof(hits));
    r = nl_rule_hits(&hits);
    if (r != 0) return die_nl("nl_rule_hits", r);
    printf("  hits: ");
    for (i = 0; i < NL_POLICY_MAX_RULES; i++)
        printf("%s%llu", (i == 0) ? "" : ",", (unsigned long long)hits.hits[i]);
    printf("  block_tx=%llu block_rx=%llu\n",
           (unsigned long long)hits.block_tx, (unsigned long long)hits.block_rx);

    memset(&dly, 0, sizeof(dly));
    r = nl_delay_stat(&dly);
    if (r != 0) return die_nl("nl_delay_stat", r);
    printf("  delay: pending=%llu fired=%llu\n",
           (unsigned long long)dly.pending, (unsigned long long)dly.fired);

    if (refresh_enum() != NLCTL_OK) return NLCTL_ERR;
    for (i = 0; i < g_nad; i++) {
        printf("  ad[%u]: %s mac=%02X-%02X-%02X-%02X-%02X-%02X\n",
               g_ad[i].index, (g_ad[i].flags & 1u) ? "up" : "down",
               g_ad[i].mac[0], g_ad[i].mac[1], g_ad[i].mac[2],
               g_ad[i].mac[3], g_ad[i].mac[4], g_ad[i].mac[5]);
    }
    return NLCTL_OK;
}

static int cmd_enum(void)
{
    int i;

    if (refresh_enum() != NLCTL_OK) return NLCTL_ERR;
    for (i = 0; i < g_nad; i++) {
        wprintf(L"  [%u] %s medium=%u %s\n",
                g_ad[i].index, g_ad[i].ifname, g_ad[i].medium,
                (g_ad[i].flags & 1u) ? L"up" : L"down");
    }
    return NLCTL_OK;
}

static int cmd_stats(int argc, char **argv)
{
    uint32_t idx = 0xFFFFFFFFu;
    nl_stats_t st;
    int r;

    if (argc >= 3) {
        idx = need_idx(argv[2]);
        if (idx == 0xFFFFFFFFu) return usage();
    }
    memset(&st, 0, sizeof(st));
    if (idx == 0xFFFFFFFFu) {
        r = nl_stats(NULL, &st);
        if (r != 0) return die_nl("nl_stats", r);
    } else {
        r = nl_stats(&idx, &st);
        if (r != 0) return die_nl("nl_stats(idx)", r);
    }
    printf("  active=%u abi=%u rx=%llu tx=%llu cap_rx=%llu cap_tx=%llu "
           "inj=%llu drop=%llu\n",
           st.active_units, st.abi_ver,
           (unsigned long long)st.rx_total, (unsigned long long)st.tx_total,
           (unsigned long long)st.rx_captured, (unsigned long long)st.tx_captured,
           (unsigned long long)st.injected, (unsigned long long)st.dropped);
    return NLCTL_OK;
}

static int cmd_mode(int argc, char **argv)
{
    uint32_t idx, en;
    int r;

    if (argc != 4) return usage();
    idx = need_idx(argv[2]);
    if (idx == 0xFFFFFFFFu) return usage();
    if (parse_dir(argv[3], &en) != 0) return usage();
    r = nl_set_mode(idx, en);
    if (r != 0) return die_nl("nl_set_mode", r);
    printf("  mode [%u] = %s\n", idx, dirstr(en));
    return NLCTL_OK;
}

/* Susun satu rule EX dari argumen; param di akhir untuk aksi bernilai. */
static int build_rule(int argc, char **argv, uint32_t action,
                      uint32_t action_param, nl_policy_rule_ex_t *ru,
                      uint32_t *en_out)
{
    int i = 2;
    int has_match = 0;
    uint32_t en = 0;

    memset(ru, 0, sizeof(*ru));
    ru->action       = NL_ACTION_BLOCK;
    ru->r.proto      = NL_PROTO_ANY;
    ru->r.match      = NL_MATCH_DST;
    ru->r.port_min   = 0;
    ru->r.port_max   = 0;
    ru->delay_ms     = 0;
    ru->drop_pct     = 0;
    ru->rate_kbps    = 0;

    if (i >= argc) return -1;
    if (parse_dir(argv[i], &en) != 0) return -1;
    i++;

    while (i < argc) {
        char *a = argv[i];
        uint32_t ip, mask;

        if (parse_proto(a, &ru->r.proto) == 0) { i++; continue; }
        if (a[0] >= '0' && a[0] <= '9' && strchr(a, '.') != NULL) {
            if (parse_ip(a, &ip, &mask) != 0) return -1;
            has_match   = 1;
            ru->r.match = NL_MATCH_DST;
            ru->r.ip    = ip;
            ru->r.mask  = mask;
            i++;
            continue;
        }
        if (strchr(a, ':') != NULL) {
            if (parse_mac(a, ru->r.dst_mac) != 0) return -1;
            has_match   = 1;
            ru->r.match = NL_MATCH_DSTMAC;
            i++;
            continue;
        }
        if (parse_port(a, &ru->r.port_min, &ru->r.port_max) == 0 &&
            (ru->r.proto == NL_PROTO_TCP || ru->r.proto == NL_PROTO_UDP)) {
            has_match = 1;
            i++;
            continue;
        }
        return -1;
    }
    if (!has_match) return -1;
    ru->action = action;
    switch (action) {
    case NL_ACTION_DELAY:    ru->delay_ms  = action_param; break;
    case NL_ACTION_DROP_PCT: ru->drop_pct  = action_param; break;
    case NL_ACTION_RATE:     ru->rate_kbps = action_param; break;
    }
    *en_out = en;
    return 0;
}

static int cmd_rule(int argc, char **argv, uint32_t action, uint32_t param)
{
    nl_policy_ex_t ex;
    nl_policy_t pol;
    nl_vm_prog_t vm;
    int r;

    memset(&ex, 0, sizeof(ex));
    if (build_rule(argc, argv, action, param, &ex.rules[0], &ex.enabled) != 0)
        return usage();
    ex.count = 1;
    memset(&pol, 0, sizeof(pol));
    memset(&vm, 0, sizeof(vm));
    r = nl_vm_set(&vm);
    if (r != 0) return die_nl("nl_vm_set", r);
    r = nl_policy_set(&pol);
    if (r != 0) return die_nl("nl_policy_set", r);
    r = nl_policy_ex_set(&ex);
    if (r != 0) return die_nl("nl_policy_ex_set", r);
    printf("  rule: %s dir=%s proto=%u ip=", actionstr(action), dirstr(ex.enabled),
           ex.rules[0].r.proto);
    print_ip(ex.rules[0].r.ip, ex.rules[0].r.mask);
    printf(" port=%u-%u mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
           (unsigned)ex.rules[0].r.port_min, (unsigned)ex.rules[0].r.port_max,
           ex.rules[0].r.dst_mac[0], ex.rules[0].r.dst_mac[1],
           ex.rules[0].r.dst_mac[2], ex.rules[0].r.dst_mac[3],
           ex.rules[0].r.dst_mac[4], ex.rules[0].r.dst_mac[5]);
    return NLCTL_OK;
}

#define NL_CAP_MAGIC 0x4C434C4Eu          /* 'NCLC' */

typedef struct nl_cap_hdr {
    uint32_t magic;
    uint32_t ver;
    uint32_t adapter_index;
    uint32_t pad;
} nl_cap_hdr_t;

/* rekam penuh frame tetap 1524 B agar file bisa read-loop tanpa seek */
typedef struct nl_cap_frame {
    uint32_t len;
    uint32_t flags;                 /* NL_RING_* arah asal */
    uint8_t  frame[NL_FRAME_MAX];
} nl_cap_frame_t;

C_ASSERT(sizeof(nl_cap_hdr_t) == 16);
C_ASSERT(sizeof(nl_cap_frame_t) == 1524);

static int cmd_cap(int argc, char **argv)
{
    static nl_ring_entry_t buf[256];
    nl_cap_hdr_t ch;
    nl_cap_frame_t fr;
    uint32_t idx, en, secs;
    unsigned long n = 0;
    FILE *f;
    ULONGLONG t0, t1;
    int got, k;
    int r;

    if (argc < 5 || argc > 6) return usage();
    idx = need_idx(argv[2]);
    if (idx == 0xFFFFFFFFu) return usage();
    if (parse_dir(argv[3], &en) != 0 || en == 0) return usage();
    secs = 5;
    if (argc == 6) {
        secs = (uint32_t)atol(argv[5]);
        if (secs == 0 || secs > 3600) return usage();
    }
    f = fopen(argv[4], "wb");
    if (f == NULL) {
        fprintf(stderr, "cap: buka %s gagal last=%lu\n", argv[4],
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }
    memset(&ch, 0, sizeof(ch));
    ch.magic = NL_CAP_MAGIC;
    ch.ver = 1;
    ch.adapter_index = idx;
    fwrite(&ch, sizeof(ch), 1, f);
    r = nl_set_mode(idx, en);
    if (r != 0) { fclose(f); return die_nl("nl_set_mode", r); }
    t0 = t1 = GetTickCount64();
    while (t1 - t0 < (ULONGLONG)secs * 1000ULL) {
        got = 0;
        r = nl_read_ring(&idx, buf, 256, &got);
        if (r != 0) { nl_set_mode(idx, 0); fclose(f); return die_nl("nl_read_ring", r); }
        for (k = 0; k < got; k++) {
            memset(&fr, 0, sizeof(fr));
            fr.len = buf[k].len;
            fr.flags = buf[k].flags;
            memcpy(fr.frame, buf[k].data, buf[k].len);
            fwrite(&fr, sizeof(fr), 1, f);
            n++;
        }
        Sleep(50);
        t1 = GetTickCount64();
    }
    if (nl_set_mode(idx, 0) != 0 || fclose(f) != 0) {
        fprintf(stderr, "cap: kembalikan mode/tutup file gagal\n");
        return NLCTL_ERR;
    }
    printf("  cap: %lu frame ke %s (ad[%u], %us)\n", n, argv[4], idx, (unsigned)secs);
    return NLCTL_OK;
}

static int cmd_send(int argc, char **argv)
{
    nl_cap_hdr_t ch;
    nl_cap_frame_t fr;
    uint32_t idx = 0xFFFFFFFFu, rx_all = 0;
    FILE *f;
    unsigned long n = 0;
    int r;

    if (argc < 3 || argc > 5) return usage();
    if (argc == 4) {
        if (_stricmp(argv[3], "rx") != 0) return usage();
        rx_all = 1;
    }
    if (argc == 5) {
        if (_stricmp(argv[3], "to") != 0) return usage();
        idx = need_idx(argv[4]);
        if (idx == 0xFFFFFFFFu) return usage();
    }
    f = fopen(argv[2], "rb");
    if (f == NULL) {
        fprintf(stderr, "send: buka %s gagal last=%lu\n", argv[2],
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }
    memset(&ch, 0, sizeof(ch));
    if (fread(&ch, sizeof(ch), 1, f) != 1 || ch.magic != NL_CAP_MAGIC || ch.ver != 1) {
        fclose(f);
        fprintf(stderr, "send: %s bukan dump nlctl\n", argv[2]);
        return NLCTL_ERR;
    }
    if (argc == 3) idx = ch.adapter_index;
    memset(&fr, 0, sizeof(fr));
    while (fread(&fr, sizeof(fr), 1, f) == 1) {
        if (fr.len == 0 || fr.len > NL_FRAME_MAX) break;
        if (rx_all || (fr.flags & NL_RING_RX)) {
            r = nl_inject_rx(fr.frame, fr.len);
            if (r != 0) { fclose(f); return die_nl("nl_inject_rx", r); }
        } else {
            r = nl_inject_to(idx, fr.frame, fr.len);
            if (r != 0) { fclose(f); return die_nl("nl_inject_to", r); }
        }
        n++;
    }
    fclose(f);
    if (rx_all)
        printf("  send: %lu frame replay dari %s (rx semua)\n", n, argv[2]);
    else
        printf("  send: %lu frame replay dari %s (ad[%u])\n", n, argv[2], idx);
    return NLCTL_OK;
}

static int cmd_load(int argc, char **argv)
{
    nl_policy_ex_t ex;
    nl_policy_t pol;
    nl_vm_prog_t vm;
    FILE *f;
    char line[512];
    int num_rule = 0;
    int r;

    if (argc != 3) return usage();
    f = fopen(argv[2], "r");
    if (f == NULL) {
        fprintf(stderr, "load: tidak bisa buka %s last=%lu\n", argv[2],
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }
    memset(&ex, 0, sizeof(ex));
    while (fgets(line, sizeof(line), f) != NULL) {
        char *tok[24];
        char *shv[26];
        char *t;
        char *cut;
        int ntok = 0;
        int nrule;
        int i;
        uint32_t action = NL_ACTION_BLOCK;
        uint32_t param = 0;
        uint32_t en;
        long num;

        if ((cut = strchr(line, '#')) != NULL) *cut = 0;
        while ((cut = strpbrk(line, "\r\n")) != NULL) *cut = 0;
        t = strtok(line, " \t");
        while (t != NULL && ntok < 24) { tok[ntok++] = t; t = strtok(NULL, " \t"); }
        if (ntok == 0) continue;
        if (_stricmp(tok[0], "block") == 0)      action = NL_ACTION_BLOCK;
        else if (_stricmp(tok[0], "allow") == 0) action = NL_ACTION_ALLOW;
        else if (_stricmp(tok[0], "delay") == 0) action = NL_ACTION_DELAY;
        else if (_stricmp(tok[0], "drop") == 0)  action = NL_ACTION_DROP_PCT;
        else if (_stricmp(tok[0], "rate") == 0)  action = NL_ACTION_RATE;
        else {
            fclose(f);
            fprintf(stderr, "load: baris %d: perintah '%s' tak dikenal\n",
                    num_rule + 1, tok[0]);
            return NLCTL_ERR;
        }
        if (ntok < 2) {
            fclose(f);
            fprintf(stderr, "load: baris %d: tanpa <dir>\n", num_rule + 1);
            return NLCTL_ERR;
        }
        nrule = ntok - 1;
        if (action == NL_ACTION_DELAY || action == NL_ACTION_DROP_PCT ||
            action == NL_ACTION_RATE) {
            if (nrule < 2) {
                fclose(f);
                fprintf(stderr, "load: baris %d: tanpa nilai aksi\n", num_rule + 1);
                return NLCTL_ERR;
            }
            num = atol(tok[ntok - 1]);
            if (action == NL_ACTION_DELAY && (num <= 0 || num > NL_DELAY_MAX_MS)) {
                fclose(f);
                fprintf(stderr, "load: baris %d: delay ms tak sah\n", num_rule + 1);
                return NLCTL_ERR;
            }
            if (action == NL_ACTION_DROP_PCT && (num < 0 || num > 100)) {
                fclose(f);
                fprintf(stderr, "load: baris %d: drop pct tak sah\n", num_rule + 1);
                return NLCTL_ERR;
            }
            if (action == NL_ACTION_RATE && num <= 0) {
                fclose(f);
                fprintf(stderr, "load: baris %d: rate kbps tak sah\n", num_rule + 1);
                return NLCTL_ERR;
            }
            param = (uint32_t)num;
            nrule--;
        }
        nrule++;
        if (ex.count >= NL_POLICY_MAX_RULES) {
            fclose(f);
            fprintf(stderr, "load: lebih dari %u rule\n", (unsigned)NL_POLICY_MAX_RULES);
            return NLCTL_ERR;
        }
        shv[0] = "load";
        shv[1] = tok[0];
        for (i = 1; i < nrule; i++) shv[1 + i] = tok[i];
        if (build_rule(1 + nrule, shv, action, param, &ex.rules[ex.count], &en) != 0) {
            fclose(f);
            fprintf(stderr, "load: baris %d: rule tak sah\n", num_rule + 1);
            return NLCTL_ERR;
        }
        ex.enabled |= en;
        ex.count++;
        num_rule++;
    }
    fclose(f);
    if (ex.count == 0) {
        fprintf(stderr, "load: tidak ada rule di %s\n", argv[2]);
        return NLCTL_ERR;
    }
    memset(&pol, 0, sizeof(pol));
    memset(&vm, 0, sizeof(vm));
    r = nl_vm_set(&vm);
    if (r != 0) return die_nl("nl_vm_set", r);
    r = nl_policy_set(&pol);
    if (r != 0) return die_nl("nl_policy_set", r);
    r = nl_policy_ex_set(&ex);
    if (r != 0) return die_nl("nl_policy_ex_set", r);
    printf("  load: %d rule dipasang dari %s (first-match)\n", num_rule, argv[2]);
    return NLCTL_OK;
}

static int cmd_mirror(int argc, char **argv)
{
    nl_mirror_cfg_t mi;
    uint32_t src, dst, fl = 0;
    int r;

    if (argc != 5) return usage();
    src = need_idx(argv[2]);
    dst = need_idx(argv[3]);
    if (src == 0xFFFFFFFFu || dst == 0xFFFFFFFFu) return usage();
    if (parse_dir(argv[4], &fl) != 0) return usage();
    if (fl == 0) dst = src;
    memset(&mi, 0, sizeof(mi));
    mi.src_index = src;
    mi.dst_index = dst;
    mi.flags = fl;
    r = nl_mirror_set(&mi);
    if (r != 0) return die_nl("nl_mirror_set", r);
    printf("  mirror ad[%u] -> ad[%u] arah=%s\n", src, dst, dirstr(fl));
    return NLCTL_OK;
}

static int pcw_u32(FILE *f, uint32_t v)
{
    uint8_t b[4];

    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    return (fwrite(b, 1, 4, f) == 4) ? 0 : -1;
}

static int pcrd_u32(FILE *f, uint32_t *v)
/* baca u32 little-endian dari file pcap */
{
    uint8_t b[4];

    if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
    return 0;
}

static int cmd_replay(int argc, char **argv)
/* isi pcap klasik, kirim frame ke driver; arah diwajibkan: rx | to <idx> */
{
    FILE *f;
    uint32_t hdr[6];
    uint32_t n = 0, skip = 0;
    uint32_t dst = 0xFFFFFFFFu;
    uint32_t u32, incl, orig;
    uint8_t fr[1514];
    int fail = 0;

    if ((argc == 4 && _stricmp(argv[3], "rx") == 0)) {
        dst = 0x80000000u;
    } else if (argc == 5 && _stricmp(argv[3], "to") == 0) {
        dst = need_idx(argv[4]);
        if (dst == 0xFFFFFFFFu) return usage();
    } else return usage();

    f = fopen(argv[2], "rb");
    if (f == NULL) {
        fprintf(stderr, "replay: tidak bisa buka %s last=%lu\n", argv[2],
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }
    if (fread(hdr, 1, 24, f) != 24 || hdr[0] != 0xA1B2C3D4 || hdr[5] != 1) {
        fclose(f);
        fprintf(stderr, "replay: %s bukan pcap klasik Ethernet yang sah\n", argv[2]);
        return NLCTL_ERR;
    }

    for (;;) {
        if (pcrd_u32(f, &u32) != 0) break;
        if (pcrd_u32(f, &u32) != 0 || pcrd_u32(f, &incl) != 0 ||
            pcrd_u32(f, &orig) != 0) {
            fprintf(stderr, "replay: header record %u terpotong\n", n + skip + 1);
            fail = 1;
            break;
        }
        if (incl == 0 || incl > orig || orig > 1514) {
            skip++;
            continue;
        }
        if (fread(fr, 1, incl, f) != incl) {
            fprintf(stderr, "replay: %s terpotong di frame ke-%u\n", argv[2], n + 1);
            fail = 1;
            break;
        }
        if (dst == 0x80000000u) u32 = nl_inject_rx(fr, incl);
        else                    u32 = nl_inject_to(dst, fr, incl);
        if (u32 != 0) {
            fprintf(stderr, "  replay: inject gagal ret=%lu di frame ke-%u\n",
                    (unsigned long)u32, n + 1);
            fail = 1;
            break;
        }
        n++;
    }
    fclose(f);

    if (fail) {
        if (n != 0) fprintf(stderr, "replay: %u frame sudah terkirim\n", n);
        return NLCTL_ERR;
    }
    if (n == 0) {
        fprintf(stderr, "replay: %s nol frame sah (%u record dilewat)\n", argv[2], skip);
        return NLCTL_ERR;
    }
    if (dst == 0x80000000u)
        printf("  replay: %u frame dari %s -> rx (TCP/IP)\n", n, argv[2]);
    else
        printf("  replay: %u frame dari %s -> ad[%u]\n", n, argv[2], dst);
    if (skip != 0) printf("  replay: %u record dilewat\n", skip);
    return NLCTL_OK;
}

static int cmd_pcap(int argc, char **argv)
{
    nl_cap_hdr_t ch;
    nl_cap_frame_t fr;
    FILE *in, *out;
    unsigned long n = 0;
    unsigned long tgt = 0;
    uint8_t h[24];
    long long ts = 0;
    char nm[512];

    if (argc != 3) return usage();
    in = fopen(argv[2], "rb");
    if (in == NULL) {
        fprintf(stderr, "pcap: buka %s gagal last=%lu\n", argv[2],
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }
    if (fread(&ch, sizeof(ch), 1, in) != 1 || ch.magic != NL_CAP_MAGIC || ch.ver != 1) {
        fclose(in);
        fprintf(stderr, "pcap: %s bukan dump nlctl\n", argv[2]);
        return NLCTL_ERR;
    }
    strcpy(nm, argv[2]);
    strcat(nm, ".pcap");
    out = fopen(nm, "wb");
    if (out == NULL) {
        fclose(in);
        fprintf(stderr, "pcap: tulis %s gagal last=%lu\n", nm,
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }
    memset(h, 0, sizeof(h));
    h[0] = 0xD4; h[1] = 0xC3; h[2] = 0xB2; h[3] = 0xA1;   /* LE dari 0xA1B2C3D4 */
    h[4] = 2; h[5] = 0;                                    /* vmaj 2 (u16) */
    h[6] = 4; h[7] = 0;                                    /* vmin 4 (u16) */
    h[16] = 0xFF; h[17] = 0xFF;                            /* snaplen 65535 */
    h[20] = 1;                                             /* DLT Ethernet */
    if (fwrite(h, 1, sizeof(h), out) == sizeof(h)) {
        memset(&fr, 0, sizeof(fr));
        while (fread(&fr, sizeof(fr), 1, in) == 1) {
            if (fr.len == 0 || fr.len > NL_FRAME_MAX) break;
            if (pcw_u32(out, (uint32_t)(ts / 1000)) == 0 &&
                pcw_u32(out, (uint32_t)(ts % 1000)) == 0 &&
                pcw_u32(out, (uint32_t)fr.len) == 0 &&
                pcw_u32(out, (uint32_t)fr.len) == 0 &&
                fwrite(fr.frame, 1, fr.len, out) == fr.len) {
                n++;
                ts++;
                continue;
            }
            fprintf(stderr, "pcap: tulis terputus di frame %lu\n", n);
            tgt = 1;
            break;
        }
    } else {
        fprintf(stderr, "pcap: tulis header pcap gagal\n");
        tgt = 1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        fprintf(stderr, "pcap: tutup %s gagal\n", nm);
        return NLCTL_ERR;
    }
    if (tgt != 0) {
        remove(nm);
        return NLCTL_ERR;
    }
    if (n == 0) {
        fprintf(stderr, "pcap: tak ada frame yang tertulis\n");
        remove(nm);
        return NLCTL_ERR;
    }
    printf("  pcap: %lu frame dari %s ke %s\n", n, argv[2], nm);
    return NLCTL_OK;
}

#define VM_LBL_MAX  16
#define VM_LBL_SZ   32

typedef struct _vm_label {
    char    name[VM_LBL_SZ];
    int     idx;
} vm_label_t;

static int       vm_lbn = 0;
static vm_label_t g_lbl[VM_LBL_MAX];

static int vm_lbl_find(const char *nm)
/* pass 2: tabel label sudah penuh */
{
    int i;

    for (i = 0; i < vm_lbn; i++)
        if (_stricmp(g_lbl[i].name, nm) == 0) return g_lbl[i].idx;
    return -1;
}

static char *vm_label_name(char *tok)
/* label ":nama" atau "nama:"; NULL bila bukan label */
{
    size_t n = strlen(tok);

    if (n < 2) return NULL;
    if (tok[0] == ':') return tok + 1;
    if (tok[n - 1] == ':') { tok[n - 1] = 0; return tok; }
    return NULL;
}

static uint16_t vm_aluop(const char *s)
{
    if (_stricmp(s, "add") == 0) return NL_VM_OP_ADD;
    if (_stricmp(s, "sub") == 0) return NL_VM_OP_SUB;
    if (_stricmp(s, "mul") == 0) return NL_VM_OP_MUL;
    if (_stricmp(s, "div") == 0) return NL_VM_OP_DIV;
    if (_stricmp(s, "or")  == 0) return NL_VM_OP_OR;
    if (_stricmp(s, "and") == 0) return NL_VM_OP_AND;
    if (_stricmp(s, "lsh") == 0) return NL_VM_OP_LSH;
    if (_stricmp(s, "rsh") == 0) return NL_VM_OP_RSH;
    if (_stricmp(s, "neg") == 0) return NL_VM_OP_NEG;
    return 0xFFFF;
}

static uint16_t vm_jmpop(const char *s)
{
    if (_stricmp(s, "a")   == 0) return NL_VM_JA;
    if (_stricmp(s, "eq")  == 0) return NL_VM_JEQ;
    if (_stricmp(s, "gt")  == 0) return NL_VM_JGT;
    if (_stricmp(s, "ge")  == 0) return NL_VM_JGE;
    if (_stricmp(s, "set") == 0) return NL_VM_JSET;
    return 0xFFFF;
}

static uint16_t vm_mnemonic(const char *s, int *isx)
{
    static const struct { const char *nm; uint16_t cl; } w[6] = {
        { "ldw",  NL_VM_CL_LD  }, { "ldh",  NL_VM_CL_LD | NL_VM_SZ_H },
        { "ldb",  NL_VM_CL_LD | NL_VM_SZ_B },
        { "ldxw", NL_VM_CL_LDX }, { "ldxh", NL_VM_CL_LDX | NL_VM_SZ_H },
        { "ldxb", NL_VM_CL_LDX | NL_VM_SZ_B },
    };
    int i;

    *isx = 0;
    for (i = 0; i < 6; i++) {
        if (_stricmp(s, w[i].nm) == 0) {
            *isx = (w[i].cl & NL_VM_CL_LDX) ? 1 : 0;
            return w[i].cl;
        }
    }
    return 0xFFFF;
}

static int vm_num(const char *s, uint32_t *k)
{
    char *end;

    *k = (uint32_t)strtoul(s, &end, 0);
    if (end == s || (end != NULL && *end != 0)) return -1;
    return 0;
}

static int vm_target(const char *s, int cur, uint8_t *out)
/* nomor insn absolut (mulai 0) atau label; lompatan maju saja (jt/jf u8) */
{
    long rel;
    int t;

    if (s[0] >= '0' && s[0] <= '9') {
        t = (int)strtoul(s, NULL, 0);
        if (t < 0 || t > NL_VM_MAX_INSNS) return -1;
    } else {
        t = vm_lbl_find(s);
        if (t < 0) return -1;
    }
    rel = (long)t - (cur + 1);
    if (rel < 0 || rel > 255) return -1;
    *out = (uint8_t)rel;
    return 0;
}

static int cmd_vmasm(int argc, char **argv)
{
    nl_vm_prog_t p;
    nl_vm_insn_t ins[NL_VM_MAX_INSNS];
    FILE *f;
    char line[512];
    int nline = 0;
    int cnt = 0;
    uint32_t en = NL_POLICY_EN_TX | NL_POLICY_EN_RX;
    int badflag = 0;
    int i, r;

    if (argc != 3) return usage();
    if (_stricmp(argv[2], "off") == 0) {
        memset(&p, 0, sizeof(p));
        r = nl_vm_set(&p);
        if (r != 0) return die_nl("nl_vm_set(off)", r);
        printf("  vm: nonaktif (kembali ke tabel EX)\n");
        return NLCTL_OK;
    }

    f = fopen(argv[2], "r");
    if (f == NULL) {
        fprintf(stderr, "vm: tidak bisa buka %s last=%lu\n", argv[2],
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }

    /* pass 1: urut insn + semua label (untuk lompatan maju tak berurut) */
    vm_lbn = 0;
    cnt = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *cut, *t;
        char *tok[16];
        char *lbln;
        int ntok = 0;

        nline++;
        if ((cut = strchr(line, '#')) != NULL) *cut = 0;
        while ((cut = strpbrk(line, "\r\n")) != NULL) *cut = 0;
        t = strtok(line, " \t");
        while (t != NULL && ntok < 16) { tok[ntok++] = t; t = strtok(NULL, " \t"); }
        if (ntok == 0) continue;
        if ((lbln = vm_label_name(tok[0])) != NULL) {
            if (strlen(lbln) >= VM_LBL_SZ || vm_lbn >= VM_LBL_MAX) {
                fclose(f);
                fprintf(stderr, "vm: baris %d: label terlalu banyak/panjang\n", nline);
                return NLCTL_ERR;
            }
            strcpy(g_lbl[vm_lbn].name, lbln);
            g_lbl[vm_lbn].idx = cnt;
            vm_lbn++;
            continue;
        }
        if (_stricmp(tok[0], "enabled") == 0) continue;
        cnt++;
        if (cnt > NL_VM_MAX_INSNS) {
            fclose(f);
            fprintf(stderr, "vm: program lebih dari %u insn\n", (unsigned)NL_VM_MAX_INSNS);
            return NLCTL_ERR;
        }
    }
    if (cnt == 0) {
        fclose(f);
        fprintf(stderr, "vm: tak ada insn di %s\n", argv[2]);
        return NLCTL_ERR;
    }

    /* pass 2: rakit dgn seluruh label sudah diketahui */
    rewind(f);
    nline = 0;
    cnt = 0;
    while (fgets(line, sizeof(line), f) != NULL && badflag == 0) {
        char *cut, *t;
        char *tok[16];
        int ntok = 0;

        nline++;
        if ((cut = strchr(line, '#')) != NULL) *cut = 0;
        while ((cut = strpbrk(line, "\r\n")) != NULL) *cut = 0;
        t = strtok(line, " \t");
        while (t != NULL && ntok < 16) { tok[ntok++] = t; t = strtok(NULL, " \t"); }
        if (ntok == 0) continue;
        if (vm_label_name(tok[0]) != NULL) continue;
        if (_stricmp(tok[0], "enabled") == 0) {
            if (ntok != 2 || parse_dir(tok[1], &en) != 0) {
                fprintf(stderr, "vm: baris %d: arah tak sah\n", nline);
                badflag = 1;
            }
            continue;
        }
        {
            nl_vm_insn_t in;
            uint16_t code = 0;
            uint32_t k = 0;

            memset(&in, 0, sizeof(in));
            if (_stricmp(tok[0], "tax") == 0) {
                if (ntok != 1) goto vbad;
                code = NL_VM_CL_MISC | NL_VM_TAX;
            } else if (_stricmp(tok[0], "txa") == 0) {
                if (ntok != 1) goto vbad;
                code = NL_VM_CL_MISC | NL_VM_TXA;
            } else if (_stricmp(tok[0], "st") == 0 || _stricmp(tok[0], "stx") == 0) {
                if (ntok != 2 || vm_num(tok[1], &k) != 0 || k >= NL_VM_SCRATCH) goto vbad;
                code = (_stricmp(tok[0], "st") == 0 ? NL_VM_CL_ST : NL_VM_CL_STX)
                       | NL_VM_MD_MEM;
            } else if (_stricmp(tok[0], "alu") == 0) {
                uint16_t op;
                if (ntok < 3) goto vbad;
                op = vm_aluop(tok[1]);
                if (op == 0xFFFF) goto vbad;
                if (_stricmp(tok[2], "x") == 0) code = NL_VM_CL_ALU | op | NL_VM_SRC_X;
                else if (op == NL_VM_OP_NEG) goto vbad;
                else {
                    if (vm_num(tok[2], &k) != 0) goto vbad;
                    code = NL_VM_CL_ALU | op;
                }
            } else if (_stricmp(tok[0], "jmp") == 0) {
                uint16_t op;
                if (ntok < 3) goto vbad;
                op = vm_jmpop(tok[1]);
                if (op == 0xFFFF) goto vbad;
                if (op == NL_VM_JA) {
                    if (ntok != 3 || vm_target(tok[2], cnt, &in.jt) != 0) goto vbad;
                } else {
                    if (ntok != 5) goto vbad;
                    if (_stricmp(tok[2], "x") == 0) op |= NL_VM_SRC_X;
                    else { if (vm_num(tok[2], &k) != 0) goto vbad; }
                    if (vm_target(tok[3], cnt, &in.jt) != 0 ||
                        vm_target(tok[4], cnt, &in.jf) != 0) goto vbad;
                }
                code = NL_VM_CL_JMP | op;
            } else if (_stricmp(tok[0], "ret") == 0) {
                if (ntok < 2) goto vbad;
                if (_stricmp(tok[1], "a") == 0) {
                    code = NL_VM_CL_RET | NL_VM_RTA;
                } else if (_stricmp(tok[1], "allow") == 0) {
                    code = NL_VM_CL_RET | NL_VM_RTK; k = NL_ACTION_ALLOW;
                } else if (_stricmp(tok[1], "block") == 0) {
                    code = NL_VM_CL_RET | NL_VM_RTK; k = NL_ACTION_BLOCK;
                } else if (ntok == 3) {
                    long long v = atoll(tok[2]);
                    if (_stricmp(tok[1], "delay") == 0) {
                        if (v <= 0 || v > NL_DELAY_MAX_MS) goto vbad;
                    } else if (_stricmp(tok[1], "drop") == 0) {
                        if (v < 0 || v > 100) goto vbad;
                    } else if (_stricmp(tok[1], "rate") == 0) {
                        if (v <= 0) goto vbad;
                    } else goto vbad;
                    code = NL_VM_CL_RET | NL_VM_RTK;
                    k = (_stricmp(tok[1], "delay") == 0 ? NL_ACTION_DELAY :
                         _stricmp(tok[1], "drop") == 0 ? NL_ACTION_DROP_PCT : NL_ACTION_RATE)
                        | ((uint32_t)v << 8);
                } else goto vbad;
            } else {
                uint16_t cl, md;
                int isx = 0;

                cl = vm_mnemonic(tok[0], &isx);
                if (cl == 0xFFFF || ntok < 2) goto vbad;
                if (_stricmp(tok[1], "len") == 0) {
                    if (isx || ntok != 2) goto vbad;
                    md = NL_VM_MD_LEN;
                } else if (_stricmp(tok[1], "mem") == 0) {
                    if (isx || ntok != 3) goto vbad;
                    if (vm_num(tok[2], &k) != 0 || k >= NL_VM_SCRATCH) goto vbad;
                    md = NL_VM_MD_MEM;
                } else {
                    if (ntok != 3) goto vbad;
                    if (_stricmp(tok[1], "abs") == 0) md = NL_VM_MD_ABS;
                    else if (_stricmp(tok[1], "ind") == 0) md = NL_VM_MD_IND;
                    else if (_stricmp(tok[1], "imm") == 0) md = NL_VM_MD_IMM;
                    else goto vbad;
                    if (vm_num(tok[2], &k) != 0) goto vbad;
                }
                code = cl | md;
            }

            in.code = code;
            in.k = k;
            ins[cnt] = in;
            cnt++;
            continue;
vbad:
            fprintf(stderr, "vm: baris %d: format tak sah\n", nline);
            badflag = 1;
        }
    }
    {
        int fe = ferror(f);
        fclose(f);
        if (badflag || fe) return NLCTL_ERR;
    }

    memset(&p, 0, sizeof(p));
    p.count = (uint32_t)cnt;
    p.enabled = en;
    for (i = 0; i < cnt; i++) p.insns[i] = ins[i];
    r = nl_vm_set(&p);
    if (r != 0) return die_nl("nl_vm_set", r);
    printf("  vm: program %u insn (enabled=%s) di pasang\n", cnt, dirstr(en));
    return NLCTL_OK;
}

static int cmd_clear(int argc, char **argv)
{
    nl_policy_t pol;
    nl_policy_ex_t ex;
    nl_vm_prog_t vm;
    int r;
    (void)argc;
    (void)argv;

    memset(&pol, 0, sizeof(pol));
    memset(&ex, 0, sizeof(ex));
    memset(&vm, 0, sizeof(vm));
    r = nl_vm_set(&vm);
    if (r != 0) return die_nl("nl_vm_set", r);
    r = nl_policy_ex_set(&ex);
    if (r != 0) return die_nl("nl_policy_ex_set", r);
    r = nl_policy_set(&pol);
    if (r != 0) return die_nl("nl_policy_set", r);
    printf("  clear: EX/VM/POLICY kosong\n");
    return NLCTL_OK;
}

static int cmd_reset(int argc, char **argv)
{
    int i, r;
    (void)argc;
    (void)argv;

    if (cmd_clear(argc, argv) != NLCTL_OK) return NLCTL_ERR;
    if (refresh_enum() != NLCTL_OK) return NLCTL_ERR;
    for (i = 0; i < g_nad; i++) {
        r = nl_set_mode(g_ad[i].index, 0);
        if (r != 0) return die_nl("nl_set_mode(0)", r);
    }
    printf("  reset: mode 0 semua adapter\n");
    return NLCTL_OK;
}

int
main(int argc, char **argv)
{
    int r;

    if (argc < 2) return usage();
    if (_stricmp(argv[1], "pcap") == 0) return cmd_pcap(argc, argv);
    if (nl_open() != 0) {
        fprintf(stderr, "nl_open gagal: device \\\\.\\ndislwf tidak terbuka (driver terpasang?) last=%lu\n",
                (unsigned long)GetLastError());
        return NLCTL_ERR;
    }
    if (_stricmp(argv[1], "show") == 0)        r = cmd_show();
    else if (_stricmp(argv[1], "enum") == 0)   r = cmd_enum();
    else if (_stricmp(argv[1], "stats") == 0)  r = cmd_stats(argc, argv);
    else if (_stricmp(argv[1], "mode") == 0)   r = cmd_mode(argc, argv);
    else if (_stricmp(argv[1], "block") == 0)  r = cmd_rule(argc, argv, NL_ACTION_BLOCK, 0);
    else if (_stricmp(argv[1], "allow") == 0)  r = cmd_rule(argc, argv, NL_ACTION_ALLOW, 0);
    else if (_stricmp(argv[1], "delay") == 0) {
        uint32_t ms;
        if (argc < 3) return usage();
        ms = (uint32_t)atol(argv[argc - 1]);
        if (ms == 0 || ms > NL_DELAY_MAX_MS) return usage();
        r = cmd_rule(argc - 1, argv, NL_ACTION_DELAY, ms);
    } else if (_stricmp(argv[1], "drop") == 0) {
        uint32_t pct;
        if (argc < 3) return usage();
        pct = (uint32_t)atol(argv[argc - 1]);
        if (pct > 100) return usage();
        r = cmd_rule(argc - 1, argv, NL_ACTION_DROP_PCT, pct);
    } else if (_stricmp(argv[1], "rate") == 0) {
        uint32_t kbps = (uint32_t)atol(argv[argc - 1]);
        if (argc < 3 || kbps == 0) return usage();
        r = cmd_rule(argc - 1, argv, NL_ACTION_RATE, kbps);
    } else if (_stricmp(argv[1], "clear") == 0) r = cmd_clear(argc, argv);
    else if (_stricmp(argv[1], "reset") == 0)   r = cmd_reset(argc, argv);
    else if (_stricmp(argv[1], "load") == 0)    r = cmd_load(argc, argv);
    else if (_stricmp(argv[1], "cap") == 0)     r = cmd_cap(argc, argv);
    else if (_stricmp(argv[1], "send") == 0)    r = cmd_send(argc, argv);
    else if (_stricmp(argv[1], "mirror") == 0)  r = cmd_mirror(argc, argv);
    else if (_stricmp(argv[1], "pcap") == 0)    r = cmd_pcap(argc, argv);
    else if (_stricmp(argv[1], "replay") == 0)  r = cmd_replay(argc, argv);
    else if (_stricmp(argv[1], "vm") == 0)      r = cmd_vmasm(argc, argv);
    else r = usage();
    nl_close();
    return r;
}
