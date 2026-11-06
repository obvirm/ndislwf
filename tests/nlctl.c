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
        "  send <dump.bin> [rx | to <idx>]\n");
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
    else r = usage();
    nl_close();
    return r;
}
