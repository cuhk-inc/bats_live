/* 实时视频用的 BATS 传输。
 *
 * 一帧是一个 generation。这一帧需要的 batch 一次发完，不等确认。
 * 中继凑满 8 个包立刻 recode；没凑满的，安静 flush-ms 之后用手头的包发出去。
 * 目的端在播放时间之前译出整帧才播出。到点还没译出，丢掉这一帧，下一帧继续。
 *
 * 输入是一串帧：4 字节大端长度，后面是这一帧的字节。
 * 默认输出在长度前面再加 generation 和 pts，序号跳了就是掉帧。
 * --plain-out 时输出和输入一样，只有长度和字节。
 *
 * 包格式（大端），固定头 40 字节，后面是 T 字节载荷：
 *   0  magic u32  'BLIV'
 *   4  version u8
 *   5  type u8
 *   6  flow u16
 *   8  generation u32
 *  12  batch_id u16
 *  14  K u16
 *  16  T u16
 *  18  n_batches u16
 *  20  frame_bytes u32
 *  24  pts_ms u32
 *  28  crc32 u32     原始帧的 CRC，不含补零
 *  32  coeff[8]
 *  40  payload[T]
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "bats_codec.h"
#include "psi_line.h"
#include "psi_live.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>

/* tcc 的头里没有 WinSock。下面只声明这个程序用到的部分，布局按 64 位 Windows。 */
typedef uintptr_t Sock;
#define SOCK_BAD ((Sock)(~(uintptr_t)0))

struct in_addr {
    unsigned long s_addr;
};
struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};
struct sockaddr_in {
    short sin_family;
    unsigned short sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
struct timeval {
    long tv_sec;
    long tv_usec;
};
typedef struct {
    unsigned int fd_count;
    uintptr_t fd_array[64];
} fd_set;
#define FD_ZERO(s) ((s)->fd_count = 0)
#define FD_SET(fd, s) ((void)((s)->fd_array[(s)->fd_count++] = (uintptr_t)(fd)))

#define AF_INET 2
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define INADDR_NONE 0xfffffffful
#ifndef FIONBIO
#define FIONBIO 0x8004667eL
#endif

typedef struct {
    unsigned short wVersion;
    unsigned short wHighVersion;
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
    char szDescription[257];
    char szSystemStatus[129];
} WSADATA;

__declspec(dllimport) int __stdcall WSAStartup(unsigned short, WSADATA *);
__declspec(dllimport) int __stdcall WSACleanup(void);
__declspec(dllimport) int __stdcall WSAGetLastError(void);
__declspec(dllimport) uintptr_t __stdcall socket(int, int, int);
__declspec(dllimport) int __stdcall closesocket(uintptr_t);
__declspec(dllimport) int __stdcall bind(uintptr_t, const struct sockaddr *, int);
__declspec(dllimport) int __stdcall sendto(uintptr_t, const char *, int, int, const struct sockaddr *,
                                          int);
__declspec(dllimport) int __stdcall recv(uintptr_t, char *, int, int);
__declspec(dllimport) int __stdcall setsockopt(uintptr_t, int, int, const char *, int);
__declspec(dllimport) int __stdcall ioctlsocket(uintptr_t, long, unsigned long *);
__declspec(dllimport) int __stdcall getsockname(uintptr_t, struct sockaddr *, int *);
__declspec(dllimport) int __stdcall select(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
__declspec(dllimport) unsigned long __stdcall inet_addr(const char *);
__declspec(dllimport) unsigned short __stdcall htons(unsigned short);
__declspec(dllimport) unsigned short __stdcall ntohs(unsigned short);
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#endif

#define LIVE_MAGIC 0x424C4956u
#define LIVE_VERSION 1u
#define LIVE_TYPE_DATA 1u
#define LIVE_HDR 40
#define LIVE_MAX_T 4096
#define LIVE_MAX_K 2048
#define LIVE_MAX_SYMS 8192
#define LIVE_MAX_IN 16
#define LIVE_BUCKETS 512

static volatile int live_stop;

typedef struct {
    uint16_t flow;
    uint32_t gen;
    uint16_t batch;
    int K;
    int T;
    int n_batches;
    uint32_t frame_bytes;
    uint32_t pts;
    uint32_t crc;
    uint8_t coeff[BATS_M];
} LiveHdr;

typedef struct {
    int pct;
    uint32_t state;
    unsigned long dropped;
} Loss;

typedef int (*PktSend)(void *ctx, const uint8_t *pkt, int len);

typedef struct {
    int role; /* 1 source, 2 relay, 3 dest */
    const char *input;
    const char *output;
    const char *to_host;
    int to_port;
    const char *bind_host;
    int listen_port;
    int fps;
    int T;
    int overhead_pct;
    int flush_ms;
    int delay_ms;
    int coalesce_ms;
    int loss_pct;
    int idle_sec;
    int max_k;
    int window;
    int frame_bytes;
    int plain_out;
    int log;
    uint16_t flow;
    uint64_t seed;
    uint64_t recode_seed;
    const char *psi_path;
} LiveConfig;

typedef struct {
    uint32_t batch_id;
    uint8_t coeff[BATS_M];
    uint8_t *payload;
} Symbol;

typedef struct {
    int used;
    uint32_t id;
    int K;
    int T;
    int n_batches;
    uint32_t frame_bytes;
    uint32_t pts;
    uint32_t crc;
    uint64_t playout;
    uint64_t last_rx;
    int dirty;
    int decoded;
    int failed;
    int n_sym;
    int cap;
    int sym_limit;
    Symbol *syms;
    uint8_t *image;
} Gen;

typedef int (*FrameOut)(void *ctx, uint32_t gen, uint32_t pts, const uint8_t *p, uint32_t n);

typedef struct {
    int delay_ms;
    int coalesce_ms;
    int window;
    int log;
    uint16_t flow;
    uint64_t seed;
    int have_base;
    uint64_t base_local;
    uint32_t base_pts;
    uint32_t next_id;
    uint64_t last_rx;
    unsigned long emitted;
    unsigned long dropped;
    int overflow_logged;
    Gen *gens;
    int ngen;
    int gcap;
    FrameOut out;
    void *out_ctx;
} Dest;

typedef struct {
    int used;
    uint16_t flow;
    uint32_t gen;
    uint16_t batch;
    int K;
    int T;
    int n_batches;
    uint32_t frame_bytes;
    uint32_t pts;
    uint32_t crc;
    int n;
    int sent_n;
    int forwards;
    uint64_t last_ms;
    uint8_t coeff[LIVE_MAX_IN * BATS_M];
    uint8_t *payload;
} Bucket;

typedef struct {
    int flush_ms;
    int window;
    uint64_t recode_state;
    uint32_t newest_gen;
    int have_gen;
    Loss loss;
    unsigned long sent_pkts;
    PktSend send;
    void *send_ctx;
    Bucket b[LIVE_BUCKETS];
} Relay;

typedef struct {
    uint8_t *data;
    int len;
} Pkt;

typedef struct {
    Pkt *p;
    int n;
    int cap;
} PktList;

typedef struct {
    uint32_t gen;
    uint32_t pts;
    uint32_t n;
    uint8_t *p;
} OutFrame;

typedef struct {
    OutFrame *f;
    int n;
    int cap;
} OutList;

typedef struct {
    FILE *fp;
    int indexed;
} FileOut;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "bats_live: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void *live_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        log_msg("内存不足");
        exit(1);
    }
    return p;
}

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    if (x == 0) {
        x = 1;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int should_drop(Loss *loss) {
    if (!loss || loss->pct <= 0) {
        return 0;
    }
    if (loss->pct >= 100) {
        loss->dropped++;
        return 1;
    }
    if ((int)(xorshift32(&loss->state) % 100u) < loss->pct) {
        loss->dropped++;
        return 1;
    }
    return 0;
}

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
    unsigned i;
    if (crc_ready) {
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        int k;
        for (k = 0; k < 8; k++) {
            if (c & 1u) {
                c = 0xEDB88320u ^ (c >> 1);
            } else {
                c >>= 1;
            }
        }
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_ieee(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    crc_init();
    for (i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void wr_u16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static unsigned rd_u16(const uint8_t *p) {
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static uint32_t rd_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int batches_for(int K, int overhead_pct) {
    int packets;
    int batches;
    if (K < 1) {
        return 1;
    }
    if (overhead_pct < 0) {
        overhead_pct = 0;
    }
    /* 默认度数下，无丢包大约要 K + K/2 个包再加一批。overhead_pct 是在这之上再多发的比例。 */
    packets = K + K / 2 + BATS_M;
    packets = (packets * (100 + overhead_pct) + 99) / 100;
    if (packets < K) {
        packets = K;
    }
    batches = (packets + BATS_M - 1) / BATS_M;
    if (batches < 1) {
        batches = 1;
    }
    if (batches > 65535) {
        batches = 65535;
    }
    return batches;
}

static int build_data_packet(uint8_t *dst, int cap, const LiveHdr *h, const uint8_t *payload) {
    if (h->T < 1 || h->T > LIVE_MAX_T || cap < LIVE_HDR + h->T) {
        return -1;
    }
    memset(dst, 0, (size_t)LIVE_HDR);
    wr_u32(dst + 0, LIVE_MAGIC);
    dst[4] = (uint8_t)LIVE_VERSION;
    dst[5] = (uint8_t)LIVE_TYPE_DATA;
    wr_u16(dst + 6, h->flow);
    wr_u32(dst + 8, h->gen);
    wr_u16(dst + 12, h->batch);
    wr_u16(dst + 14, (unsigned)h->K);
    wr_u16(dst + 16, (unsigned)h->T);
    wr_u16(dst + 18, (unsigned)h->n_batches);
    wr_u32(dst + 20, h->frame_bytes);
    wr_u32(dst + 24, h->pts);
    wr_u32(dst + 28, h->crc);
    memcpy(dst + 32, h->coeff, BATS_M);
    memcpy(dst + LIVE_HDR, payload, (size_t)h->T);
    return LIVE_HDR + h->T;
}

static int unpack_packet(const uint8_t *pkt, int len, LiveHdr *h, const uint8_t **payload) {
    if (len < LIVE_HDR || rd_u32(pkt) != LIVE_MAGIC || pkt[4] != LIVE_VERSION ||
        pkt[5] != LIVE_TYPE_DATA) {
        return -1;
    }
    memset(h, 0, sizeof(*h));
    h->flow = (uint16_t)rd_u16(pkt + 6);
    h->gen = rd_u32(pkt + 8);
    h->batch = (uint16_t)rd_u16(pkt + 12);
    h->K = (int)rd_u16(pkt + 14);
    h->T = (int)rd_u16(pkt + 16);
    h->n_batches = (int)rd_u16(pkt + 18);
    h->frame_bytes = rd_u32(pkt + 20);
    h->pts = rd_u32(pkt + 24);
    h->crc = rd_u32(pkt + 28);
    memcpy(h->coeff, pkt + 32, BATS_M);
    if (h->K < 1 || h->K > LIVE_MAX_K || h->T < 1 || h->T > LIVE_MAX_T || h->n_batches < 1) {
        return -1;
    }
    if (h->frame_bytes == 0 || (uint64_t)h->frame_bytes > (uint64_t)h->K * (uint64_t)h->T) {
        return -1;
    }
    if (len < LIVE_HDR + h->T) {
        return -1;
    }
    *payload = pkt + LIVE_HDR;
    return 0;
}

static int install_psi(const char *path) {
    if (path) {
        if (!psi_load(path)) {
            return -1;
        }
        return 0;
    }
    {
        if (!psi_set(psi_line_w, psi_line_D)) {
            return -1;
        }
    }
    return 0;
}

static int emit_frame(const uint8_t *src, uint32_t frame_bytes, int T, int K, int n_batches,
                      uint64_t seed, uint32_t gen, uint32_t pts, uint16_t flow, Loss *loss,
                      PktSend send, void *ctx, unsigned long *sent) {
    uint8_t *coeff;
    uint8_t *payload;
    uint8_t *pkt;
    uint32_t crc;
    int b;
    int j;
    crc = crc32_ieee(src, frame_bytes);
    coeff = live_alloc((size_t)BATS_M * BATS_M);
    payload = live_alloc((size_t)BATS_M * (size_t)T);
    pkt = live_alloc((size_t)LIVE_HDR + (size_t)T);
    for (b = 0; b < n_batches; b++) {
        if (bats_encode(src, K, T, seed, (uint32_t)b, coeff, payload) != 0) {
            free(coeff);
            free(payload);
            free(pkt);
            return -1;
        }
        for (j = 0; j < BATS_M; j++) {
            LiveHdr h;
            int len;
            memset(&h, 0, sizeof(h));
            h.flow = flow;
            h.gen = gen;
            h.batch = (uint16_t)b;
            h.K = K;
            h.T = T;
            h.n_batches = n_batches;
            h.frame_bytes = frame_bytes;
            h.pts = pts;
            h.crc = crc;
            memcpy(h.coeff, coeff + (size_t)j * BATS_M, BATS_M);
            len = build_data_packet(pkt, LIVE_HDR + T, &h, payload + (size_t)j * (size_t)T);
            if (len < 0) {
                free(coeff);
                free(payload);
                free(pkt);
                return -1;
            }
            if (should_drop(loss)) {
                continue;
            }
            if (send(ctx, pkt, len) != 0) {
                free(coeff);
                free(payload);
                free(pkt);
                return -1;
            }
            if (sent) {
                (*sent)++;
            }
        }
    }
    free(coeff);
    free(payload);
    free(pkt);
    return 0;
}

static void gen_clear_symbols(Gen *g) {
    int i;
    for (i = 0; i < g->n_sym; i++) {
        free(g->syms[i].payload);
        g->syms[i].payload = NULL;
    }
    free(g->syms);
    g->syms = NULL;
    g->n_sym = 0;
    g->cap = 0;
}

static void gen_free(Gen *g) {
    gen_clear_symbols(g);
    free(g->image);
    g->image = NULL;
}

static uint64_t playout_of(const Dest *d, uint32_t pts) {
    int32_t delta = (int32_t)(pts - d->base_pts);
    if (delta < 0) {
        return d->base_local;
    }
    return d->base_local + (uint64_t)delta + (uint64_t)d->delay_ms;
}

static Gen *dest_find(Dest *d, uint32_t id) {
    int i;
    for (i = 0; i < d->ngen; i++) {
        if (d->gens[i].used && d->gens[i].id == id) {
            return &d->gens[i];
        }
    }
    return NULL;
}

static void dest_discard_before(Dest *d, uint32_t id) {
    int i = 0;
    while (i < d->ngen) {
        if (d->gens[i].used && d->gens[i].id < id) {
            gen_free(&d->gens[i]);
            if (i != d->ngen - 1) {
                d->gens[i] = d->gens[d->ngen - 1];
            }
            memset(&d->gens[d->ngen - 1], 0, sizeof(Gen));
            d->ngen--;
            continue;
        }
        i++;
    }
}

static Gen *dest_create(Dest *d, const LiveHdr *h, uint64_t now) {
    Gen *g;
    int lim;
    if (d->ngen == d->gcap) {
        int ncap = d->gcap ? d->gcap * 2 : 8;
        Gen *ng = (Gen *)realloc(d->gens, (size_t)ncap * sizeof(Gen));
        if (!ng) {
            log_msg("内存不足");
            exit(1);
        }
        memset(ng + d->gcap, 0, (size_t)(ncap - d->gcap) * sizeof(Gen));
        d->gens = ng;
        d->gcap = ncap;
    }
    g = &d->gens[d->ngen++];
    memset(g, 0, sizeof(*g));
    g->used = 1;
    g->id = h->gen;
    g->K = h->K;
    g->T = h->T;
    g->n_batches = h->n_batches;
    g->frame_bytes = h->frame_bytes;
    g->pts = h->pts;
    g->crc = h->crc;
    g->playout = playout_of(d, h->pts);
    g->last_rx = now;
    lim = h->n_batches * BATS_M * 2;
    if (lim < h->K * 2) {
        lim = h->K * 2;
    }
    if (lim > LIVE_MAX_SYMS) {
        lim = LIVE_MAX_SYMS;
    }
    g->sym_limit = lim;
    return g;
}

static int dest_try(Dest *d, Gen *g) {
    BatsSymbol *view;
    uint8_t *dst;
    int i;
    int ok;
    if (g->decoded || g->failed) {
        return g->decoded ? 1 : -1;
    }
    if (g->n_sym < g->K || g->n_sym < 1) {
        return 0;
    }
    view = (BatsSymbol *)live_alloc((size_t)g->n_sym * sizeof(BatsSymbol));
    dst = (uint8_t *)live_alloc((size_t)g->K * (size_t)g->T);
    for (i = 0; i < g->n_sym; i++) {
        view[i].batch_id = g->syms[i].batch_id;
        memcpy(view[i].coeff, g->syms[i].coeff, BATS_M);
        view[i].payload = g->syms[i].payload;
    }
    ok = bats_decode(view, g->n_sym, g->K, g->T, d->seed, dst, NULL, NULL);
    free(view);
    if (ok < 0) {
        free(dst);
        g->failed = 1;
        gen_clear_symbols(g);
        return -1;
    }
    if (ok == 0) {
        free(dst);
        return 0;
    }
    if (crc32_ieee(dst, g->frame_bytes) != g->crc) {
        free(dst);
        g->failed = 1;
        gen_clear_symbols(g);
        if (d->log) {
            log_msg("gen=%u 译码结果和 CRC 不一致", g->id);
        }
        return -1;
    }
    g->image = (uint8_t *)live_alloc(g->frame_bytes);
    memcpy(g->image, dst, g->frame_bytes);
    free(dst);
    gen_clear_symbols(g);
    g->decoded = 1;
    return 1;
}

static void dest_slide(Dest *d, uint32_t gen) {
    uint32_t keep;
    uint32_t from;
    if (d->window < 1) {
        d->window = 1;
    }
    if (gen < d->next_id + (uint32_t)d->window) {
        return;
    }
    keep = gen - (uint32_t)d->window + 1u;
    from = d->next_id;
    if (keep > from) {
        d->dropped += (unsigned long)(keep - from);
        d->next_id = keep;
        dest_discard_before(d, keep);
        if (d->log) {
            log_msg("跳过缺席的帧 %u..%u", from, keep - 1u);
        }
    }
}

static int dest_packet(Dest *d, const uint8_t *pkt, int len, uint64_t now) {
    LiveHdr h;
    const uint8_t *payload;
    Gen *g;
    int i;
    uint8_t *copy;
    if (unpack_packet(pkt, len, &h, &payload) != 0) {
        return 0;
    }
    if (h.flow != d->flow || h.batch >= h.n_batches) {
        return 0;
    }
    dest_slide(d, h.gen);
    if (h.gen < d->next_id) {
        return 0;
    }
    if (!d->have_base) {
        d->have_base = 1;
        d->base_local = now;
        d->base_pts = h.pts;
    }
    d->last_rx = now;
    g = dest_find(d, h.gen);
    if (!g) {
        g = dest_create(d, &h, now);
    } else if (g->K != h.K || g->T != h.T || g->frame_bytes != h.frame_bytes || g->crc != h.crc ||
               g->n_batches != h.n_batches) {
        return 0;
    }
    if (g->decoded || g->failed) {
        return 0;
    }
    for (i = 0; i < g->n_sym; i++) {
        if (g->syms[i].batch_id == h.batch && memcmp(g->syms[i].coeff, h.coeff, BATS_M) == 0) {
            return 0;
        }
    }
    if (g->n_sym >= g->sym_limit) {
        return 0;
    }
    if (g->n_sym == g->cap) {
        int ncap = g->cap ? g->cap * 2 : 16;
        Symbol *ns = (Symbol *)realloc(g->syms, (size_t)ncap * sizeof(Symbol));
        if (!ns) {
            log_msg("内存不足");
            exit(1);
        }
        g->syms = ns;
        g->cap = ncap;
    }
    copy = (uint8_t *)live_alloc((size_t)h.T);
    memcpy(copy, payload, (size_t)h.T);
    g->syms[g->n_sym].batch_id = h.batch;
    memcpy(g->syms[g->n_sym].coeff, h.coeff, BATS_M);
    g->syms[g->n_sym].payload = copy;
    g->n_sym++;
    g->last_rx = now;
    g->dirty = 1;
    return 1;
}

static int dest_index(Dest *d, uint32_t id) {
    int i;
    for (i = 0; i < d->ngen; i++) {
        if (d->gens[i].used && d->gens[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int dest_quiet(Dest *d, uint64_t now) {
    int i;
    for (i = 0; i < d->ngen; i++) {
        Gen *g = &d->gens[i];
        if (!g->used || !g->dirty || g->decoded || g->failed) {
            continue;
        }
        if (now >= g->last_rx + (uint64_t)d->coalesce_ms) {
            dest_try(d, g);
            g->dirty = 0;
        }
    }
    return 0;
}

static int dest_pump(Dest *d, uint64_t now) {
    dest_quiet(d, now);
    for (;;) {
        int idx;
        Gen *g;
        Gen *later;
        uint32_t later_id;
        uint64_t later_play;
        uint32_t from;
        idx = dest_index(d, d->next_id);
        if (idx < 0) {
            later = NULL;
            later_id = 0;
            later_play = 0;
            {
                int i;
                for (i = 0; i < d->ngen; i++) {
                    if (!d->gens[i].used || d->gens[i].id <= d->next_id) {
                        continue;
                    }
                    if (!later || d->gens[i].id < later_id) {
                        later = &d->gens[i];
                        later_id = d->gens[i].id;
                        later_play = d->gens[i].playout;
                    }
                }
            }
            if (!later || now < later_play) {
                break;
            }
            from = d->next_id;
            d->dropped += (unsigned long)(later_id - from);
            d->next_id = later_id;
            if (d->log) {
                log_msg("跳过缺席的帧 %u..%u", from, later_id - 1u);
            }
            continue;
        }
        g = &d->gens[idx];
        if (now < g->playout) {
            break;
        }
        if (!g->decoded && !g->failed) {
            dest_try(d, g);
        }
        if (g->decoded) {
            if (d->out(d->out_ctx, g->id, g->pts, g->image, g->frame_bytes) != 0) {
                return -1;
            }
            if (d->log) {
                log_msg("播出 gen=%u pts=%u 字节=%u", g->id, g->pts, g->frame_bytes);
            }
            d->emitted++;
        } else {
            if (d->log) {
                log_msg("丢掉 gen=%u 符号=%d K=%d", g->id, g->n_sym, g->K);
            }
            d->dropped++;
        }
        gen_free(&d->gens[idx]);
        if (idx != d->ngen - 1) {
            d->gens[idx] = d->gens[d->ngen - 1];
        }
        memset(&d->gens[d->ngen - 1], 0, sizeof(Gen));
        d->ngen--;
        d->next_id++;
    }
    return 0;
}

static int dest_wait_ms(Dest *d, uint64_t now) {
    int wait = 50;
    int i;
    for (i = 0; i < d->ngen; i++) {
        Gen *g = &d->gens[i];
        int w;
        uint64_t due;
        if (!g->used) {
            continue;
        }
        if (g->dirty && !g->decoded && !g->failed) {
            due = g->last_rx + (uint64_t)d->coalesce_ms;
            w = due <= now ? 0 : (int)(due - now);
            if (w < wait) {
                wait = w;
            }
        }
        if (g->id >= d->next_id) {
            w = g->playout <= now ? 0 : (int)(g->playout - now);
            if (w < wait) {
                wait = w;
            }
        }
    }
    return wait;
}

static void dest_free(Dest *d) {
    int i;
    for (i = 0; i < d->ngen; i++) {
        gen_free(&d->gens[i]);
    }
    free(d->gens);
    d->gens = NULL;
    d->ngen = 0;
}

static int file_out(void *ctx, uint32_t gen, uint32_t pts, const uint8_t *p, uint32_t n) {
    FileOut *fo = (FileOut *)ctx;
    uint8_t b[4];
    if (fo->indexed) {
        wr_u32(b, gen);
        if (fwrite(b, 1, 4, fo->fp) != 4) {
            return -1;
        }
        wr_u32(b, pts);
        if (fwrite(b, 1, 4, fo->fp) != 4) {
            return -1;
        }
    }
    wr_u32(b, n);
    if (fwrite(b, 1, 4, fo->fp) != 4) {
        return -1;
    }
    if (n && fwrite(p, 1, n, fo->fp) != n) {
        return -1;
    }
    if (fflush(fo->fp) != 0) {
        return -1;
    }
    return 0;
}

static void bucket_clear(Bucket *b) {
    free(b->payload);
    memset(b, 0, sizeof(*b));
}

static Bucket *relay_find(Relay *r, uint16_t flow, uint32_t gen, uint16_t batch, int create) {
    int i;
    int free_i = -1;
    for (i = 0; i < LIVE_BUCKETS; i++) {
        if (!r->b[i].used) {
            if (free_i < 0) {
                free_i = i;
            }
            continue;
        }
        if (r->b[i].flow == flow && r->b[i].gen == gen && r->b[i].batch == batch) {
            return &r->b[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (free_i < 0) {
        for (i = 0; i < LIVE_BUCKETS; i++) {
            if (!r->b[i].used) {
                continue;
            }
            if (r->have_gen && r->newest_gen > r->b[i].gen &&
                r->newest_gen - r->b[i].gen > (uint32_t)r->window) {
                bucket_clear(&r->b[i]);
                free_i = i;
                break;
            }
        }
    }
    if (free_i < 0) {
        return NULL;
    }
    memset(&r->b[free_i], 0, sizeof(r->b[free_i]));
    r->b[free_i].used = 1;
    return &r->b[free_i];
}

static int relay_forward(Relay *r, Bucket *b) {
    uint8_t *coeff_out;
    uint8_t *payload_out;
    uint8_t *pkt;
    int n_out = 0;
    int j;
    int rc;
    if (b->n <= b->sent_n || b->n <= 0) {
        return 0;
    }
    if (b->forwards >= 3) {
        b->sent_n = b->n;
        return 0;
    }
    coeff_out = (uint8_t *)live_alloc((size_t)BATS_M * BATS_M);
    payload_out = (uint8_t *)live_alloc((size_t)BATS_M * (size_t)b->T);
    pkt = (uint8_t *)live_alloc((size_t)LIVE_HDR + (size_t)b->T);
    rc = bats_recode(b->coeff, b->payload, b->n, b->T, &r->recode_state, coeff_out, payload_out,
                     &n_out);
    if (rc != 0 || n_out != BATS_M) {
        free(coeff_out);
        free(payload_out);
        free(pkt);
        return -1;
    }
    for (j = 0; j < n_out; j++) {
        LiveHdr h;
        int len;
        memset(&h, 0, sizeof(h));
        h.flow = b->flow;
        h.gen = b->gen;
        h.batch = b->batch;
        h.K = b->K;
        h.T = b->T;
        h.n_batches = b->n_batches;
        h.frame_bytes = b->frame_bytes;
        h.pts = b->pts;
        h.crc = b->crc;
        memcpy(h.coeff, coeff_out + (size_t)j * BATS_M, BATS_M);
        len = build_data_packet(pkt, LIVE_HDR + b->T, &h, payload_out + (size_t)j * (size_t)b->T);
        if (len < 0) {
            free(coeff_out);
            free(payload_out);
            free(pkt);
            return -1;
        }
        if (should_drop(&r->loss)) {
            continue;
        }
        if (r->send(r->send_ctx, pkt, len) != 0) {
            free(coeff_out);
            free(payload_out);
            free(pkt);
            return -1;
        }
        r->sent_pkts++;
    }
    b->sent_n = b->n;
    b->forwards++;
    free(coeff_out);
    free(payload_out);
    free(pkt);
    return 0;
}

static void relay_expire(Relay *r) {
    int i;
    if (!r->have_gen || r->window < 1) {
        return;
    }
    for (i = 0; i < LIVE_BUCKETS; i++) {
        if (!r->b[i].used) {
            continue;
        }
        if (r->newest_gen > r->b[i].gen && r->newest_gen - r->b[i].gen > (uint32_t)r->window) {
            bucket_clear(&r->b[i]);
        }
    }
}

static int relay_packet(Relay *r, const uint8_t *pkt, int len, uint64_t now) {
    LiveHdr h;
    const uint8_t *payload;
    Bucket *b;
    int i;
    if (unpack_packet(pkt, len, &h, &payload) != 0 || h.batch >= h.n_batches) {
        return 0;
    }
    if (!r->have_gen || h.gen > r->newest_gen) {
        r->newest_gen = h.gen;
        r->have_gen = 1;
    }
    relay_expire(r);
    if (r->have_gen && r->newest_gen > h.gen && r->newest_gen - h.gen > (uint32_t)r->window) {
        return 0;
    }
    b = relay_find(r, h.flow, h.gen, h.batch, 1);
    if (!b) {
        return 0;
    }
    if (b->n == 0) {
        b->flow = h.flow;
        b->gen = h.gen;
        b->batch = h.batch;
        b->K = h.K;
        b->T = h.T;
        b->n_batches = h.n_batches;
        b->frame_bytes = h.frame_bytes;
        b->pts = h.pts;
        b->crc = h.crc;
        b->payload = (uint8_t *)live_alloc((size_t)LIVE_MAX_IN * (size_t)h.T);
    } else if (b->K != h.K || b->T != h.T || b->crc != h.crc || b->frame_bytes != h.frame_bytes) {
        return 0;
    }
    for (i = 0; i < b->n; i++) {
        if (memcmp(b->coeff + (size_t)i * BATS_M, h.coeff, BATS_M) == 0) {
            return 0;
        }
    }
    if (b->n >= LIVE_MAX_IN) {
        return 0;
    }
    memcpy(b->coeff + (size_t)b->n * BATS_M, h.coeff, BATS_M);
    memcpy(b->payload + (size_t)b->n * (size_t)b->T, payload, (size_t)h.T);
    b->n++;
    b->last_ms = now;
    if (b->n >= BATS_M && b->n > b->sent_n) {
        return relay_forward(r, b);
    }
    return 0;
}

static int relay_flush(Relay *r, uint64_t now) {
    int i;
    for (i = 0; i < LIVE_BUCKETS; i++) {
        Bucket *b = &r->b[i];
        if (!b->used || b->n <= b->sent_n) {
            continue;
        }
        if (now - b->last_ms >= (uint64_t)r->flush_ms) {
            if (relay_forward(r, b) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int relay_wait_ms(Relay *r, uint64_t now) {
    int wait = 50;
    int i;
    for (i = 0; i < LIVE_BUCKETS; i++) {
        Bucket *b = &r->b[i];
        uint64_t due;
        int w;
        if (!b->used || b->n <= b->sent_n) {
            continue;
        }
        due = b->last_ms + (uint64_t)r->flush_ms;
        w = due <= now ? 0 : (int)(due - now);
        if (w < wait) {
            wait = w;
        }
    }
    return wait;
}

static void relay_free(Relay *r) {
    int i;
    for (i = 0; i < LIVE_BUCKETS; i++) {
        if (r->b[i].used) {
            bucket_clear(&r->b[i]);
        }
    }
}

static uint64_t now_ms(void) {
#ifdef _WIN32
    /* GetTickCount 大约 49 天绕回一次。这里把绕回累进到 64 位。 */
    static uint32_t last;
    static uint64_t hi;
    static int ready;
    uint32_t t = (uint32_t)GetTickCount();
    if (!ready) {
        last = t;
        ready = 1;
    } else if (t < last) {
        hi += 0x100000000ull;
    }
    last = t;
    return hi + (uint64_t)t;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
#endif
}

static void sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void sleep_until(uint64_t deadline) {
    for (;;) {
        uint64_t n;
        int left;
        if (live_stop) {
            return;
        }
        n = now_ms();
        if (n >= deadline) {
            return;
        }
        left = (int)(deadline - n);
        if (left > 20) {
            left = 20;
        }
        sleep_ms(left);
    }
}

static void set_binary(FILE *fp) {
#ifdef _WIN32
    _setmode(_fileno(fp), _O_BINARY);
#else
    (void)fp;
#endif
}

static int net_start(void) {
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        log_msg("WSAStartup 失败");
        return -1;
    }
#endif
    return 0;
}

static void net_stop(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static void sock_close(Sock s) {
    if (s == SOCK_BAD) {
        return;
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static int sock_error_again(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static int sock_nonblock(Sock s) {
#ifdef _WIN32
    unsigned long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

static int udp_bind(Sock *out, const char *host, int port) {
    Sock s;
    struct sockaddr_in addr;
    int on = 1;
    int rcv = 1 << 20;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_BAD) {
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof(rcv));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&rcv, sizeof(rcv));
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host ? host : "0.0.0.0");
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        sock_close(s);
        return -1;
    }
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_close(s);
        return -1;
    }
    if (sock_nonblock(s) != 0) {
        sock_close(s);
        return -1;
    }
    *out = s;
    return 0;
}

static int udp_peer(struct sockaddr_in *addr, const char *host, int port) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    addr->sin_addr.s_addr = inet_addr(host);
    if (addr->sin_addr.s_addr == INADDR_NONE) {
        return -1;
    }
    return 0;
}

static int udp_local_port(Sock s) {
    struct sockaddr_in addr;
#ifdef _WIN32
    int alen = (int)sizeof(addr);
#else
    socklen_t alen = (socklen_t)sizeof(addr);
#endif
    memset(&addr, 0, sizeof(addr));
    if (getsockname(s, (struct sockaddr *)&addr, &alen) != 0) {
        return -1;
    }
    return (int)ntohs(addr.sin_port);
}

typedef struct {
    Sock fd;
    struct sockaddr_in peer;
} UdpOut;

static int udp_send(void *ctx, const uint8_t *pkt, int len) {
    UdpOut *u = (UdpOut *)ctx;
    int n = (int)sendto(u->fd, (const char *)pkt, len, 0, (struct sockaddr *)&u->peer,
                        sizeof(u->peer));
    if (n == len) {
        return 0;
    }
    return -1;
}

static int udp_recv(Sock s, uint8_t *buf, int cap) {
    int n = (int)recv(s, (char *)buf, cap, 0);
    if (n < 0) {
        if (sock_error_again()) {
            return 0;
        }
        return -1;
    }
    return n;
}

static int wait_fd(Sock s, int wait_ms) {
    fd_set rfds;
    struct timeval tv;
    int rc;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
#ifdef _WIN32
    rc = select(0, &rfds, NULL, NULL, &tv);
#else
    rc = select(s + 1, &rfds, NULL, NULL, &tv);
#endif
    if (rc < 0 && sock_error_again()) {
        return 0;
    }
    return rc;
}

static int read_full(FILE *fp, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = fread(p + got, 1, n - got, fp);
        if (r == 0) {
            return -1;
        }
        got += r;
    }
    return 0;
}

static int read_prefix(FILE *fp, uint32_t *out, int *eof) {
    uint8_t b[4];
    size_t r = fread(b, 1, 4, fp);
    if (r == 0) {
        *eof = 1;
        return 0;
    }
    if (r < 4) {
        return -1;
    }
    *out = rd_u32(b);
    *eof = 0;
    return 0;
}

static int run_source(LiveConfig *cfg) {
    FILE *fp = stdin;
    UdpOut udp;
    Loss loss;
    uint8_t *frame = NULL;
    uint32_t frame_cap = 0;
    uint32_t gen = 0;
    unsigned long sent = 0;
    unsigned long frames = 0;
    uint64_t origin;
    int own = 0;
    uint32_t max_frame;
    memset(&udp, 0, sizeof(udp));
    memset(&loss, 0, sizeof(loss));
    loss.pct = cfg->loss_pct;
    loss.state = (uint32_t)cfg->seed ^ 0xA5A5A5A5u;
    if (cfg->T < 1 || cfg->T > LIVE_MAX_T || cfg->max_k < 1 || cfg->max_k > LIVE_MAX_K) {
        log_msg("T 或 max-k 不合法");
        return 1;
    }
    if (!cfg->to_host || cfg->to_port <= 0) {
        log_msg("源需要 --to 主机:端口");
        return 1;
    }
    max_frame = (uint32_t)cfg->max_k * (uint32_t)cfg->T;
    if (cfg->frame_bytes < 0 || (cfg->frame_bytes > 0 && (uint32_t)cfg->frame_bytes > max_frame)) {
        log_msg("frame-bytes 超过 max-k * T");
        return 1;
    }
    if (net_start() != 0) {
        return 1;
    }
    udp.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp.fd == SOCK_BAD || udp_peer(&udp.peer, cfg->to_host, cfg->to_port) != 0) {
        log_msg("打开发送套接字失败");
        sock_close(udp.fd);
        net_stop();
        return 1;
    }
    if (cfg->input && strcmp(cfg->input, "-") != 0) {
        fp = fopen(cfg->input, "rb");
        if (!fp) {
            log_msg("打不开输入 %s", cfg->input);
            sock_close(udp.fd);
            net_stop();
            return 1;
        }
        own = 1;
    } else {
        set_binary(stdin);
    }
    origin = now_ms();
    log_msg("源 T=%d 冗余=%d%% 帧率=%d 发往 %s:%d", cfg->T, cfg->overhead_pct, cfg->fps,
            cfg->to_host, cfg->to_port);
    while (!live_stop) {
        uint32_t nbytes = 0;
        int eof = 0;
        int K;
        int n_batches;
        uint32_t pts;
        uint8_t *src;
        if (cfg->frame_bytes > 0) {
            nbytes = (uint32_t)cfg->frame_bytes;
        } else if (read_prefix(fp, &nbytes, &eof) != 0) {
            log_msg("输入在长度处被截断");
            break;
        } else if (eof) {
            break;
        }
        if (nbytes == 0) {
            gen++;
            continue;
        }
        if (nbytes > max_frame) {
            log_msg("帧 %u 有 %u 字节，超过 %u，这一帧不发", gen, nbytes, max_frame);
            if (cfg->frame_bytes == 0) {
                uint8_t junk[4096];
                uint32_t left = nbytes;
                int bad = 0;
                while (left) {
                    uint32_t chunk = left > sizeof(junk) ? (uint32_t)sizeof(junk) : left;
                    if (read_full(fp, junk, chunk) != 0) {
                        bad = 1;
                        break;
                    }
                    left -= chunk;
                }
                if (bad) {
                    log_msg("跳过大帧时输入结束");
                    break;
                }
            }
            gen++;
            continue;
        }
        if (nbytes > frame_cap) {
            free(frame);
            frame = (uint8_t *)live_alloc(nbytes);
            frame_cap = nbytes;
        }
        if (read_full(fp, frame, nbytes) != 0) {
            log_msg("输入在帧数据处被截断");
            break;
        }
        K = (int)((nbytes + (uint32_t)cfg->T - 1u) / (uint32_t)cfg->T);
        n_batches = batches_for(K, cfg->overhead_pct);
        if (cfg->fps > 0) {
            pts = (uint32_t)((uint64_t)gen * 1000ull / (uint64_t)cfg->fps);
            sleep_until(origin + (uint64_t)gen * 1000ull / (uint64_t)cfg->fps);
            if (live_stop) {
                break;
            }
        } else {
            pts = (uint32_t)(now_ms() - origin);
        }
        src = (uint8_t *)live_alloc((size_t)K * (size_t)cfg->T);
        memcpy(src, frame, nbytes);
        if (emit_frame(src, nbytes, cfg->T, K, n_batches, cfg->seed, gen, pts, cfg->flow, &loss,
                       udp_send, &udp, &sent) != 0) {
            log_msg("发送 gen=%u 失败", gen);
            free(src);
            free(frame);
            if (own) {
                fclose(fp);
            }
            sock_close(udp.fd);
            net_stop();
            return 1;
        }
        free(src);
        frames++;
        if (cfg->log) {
            log_msg("发出 gen=%u 字节=%u K=%d batch=%d pts=%u", gen, nbytes, K, n_batches, pts);
        }
        gen++;
    }
    free(frame);
    if (own) {
        fclose(fp);
    }
    sock_close(udp.fd);
    net_stop();
    log_msg("源结束 帧=%lu 包=%lu 模拟丢弃=%lu", frames, sent, loss.dropped);
    return 0;
}

static int run_relay(LiveConfig *cfg) {
    Relay relay;
    UdpOut udp;
    Sock fd = SOCK_BAD;
    uint8_t *buf;
    uint64_t last_rx = 0;
    memset(&relay, 0, sizeof(relay));
    memset(&udp, 0, sizeof(udp));
    if (!cfg->to_host || cfg->to_port <= 0 || cfg->listen_port <= 0) {
        log_msg("中继需要 --listen 和 --to");
        return 1;
    }
    relay.flush_ms = cfg->flush_ms;
    relay.window = cfg->window > 0 ? cfg->window : 1;
    relay.recode_state = cfg->recode_seed ? cfg->recode_seed : 0xB175E1ULL;
    relay.loss.pct = cfg->loss_pct;
    relay.loss.state = (uint32_t)cfg->recode_seed ^ 0x5A5A5A5Au;
    relay.send = udp_send;
    relay.send_ctx = &udp;
    if (net_start() != 0) {
        return 1;
    }
    if (udp_bind(&fd, cfg->bind_host, cfg->listen_port) != 0 ||
        udp_peer(&udp.peer, cfg->to_host, cfg->to_port) != 0) {
        log_msg("中继套接字失败");
        sock_close(fd);
        net_stop();
        return 1;
    }
    udp.fd = fd;
    buf = (uint8_t *)live_alloc((size_t)LIVE_HDR + LIVE_MAX_T);
    log_msg("中继 听 %s:%d 转发 %s:%d 冲刷=%dms", cfg->bind_host ? cfg->bind_host : "0.0.0.0",
            cfg->listen_port, cfg->to_host, cfg->to_port, cfg->flush_ms);
    while (!live_stop) {
        uint64_t now = now_ms();
        int wait = relay_wait_ms(&relay, now);
        int rc;
        if (cfg->idle_sec > 0 && last_rx > 0) {
            uint64_t idle_at = last_rx + (uint64_t)cfg->idle_sec * 1000ull;
            int iw = idle_at <= now ? 0 : (int)(idle_at - now);
            if (iw < wait) {
                wait = iw;
            }
            if (now >= idle_at) {
                break;
            }
        }
        rc = wait_fd(fd, wait);
        now = now_ms();
        if (rc > 0) {
            for (;;) {
                int n = udp_recv(fd, buf, LIVE_HDR + LIVE_MAX_T);
                if (n == 0) {
                    break;
                }
                if (n < 0) {
                    log_msg("中继接收失败");
                    free(buf);
                    relay_free(&relay);
                    sock_close(fd);
                    net_stop();
                    return 1;
                }
                last_rx = now;
                if (relay_packet(&relay, buf, n, now) != 0) {
                    log_msg("中继转发失败");
                    free(buf);
                    relay_free(&relay);
                    sock_close(fd);
                    net_stop();
                    return 1;
                }
            }
        } else if (rc < 0) {
            log_msg("中继 select 失败");
            free(buf);
            relay_free(&relay);
            sock_close(fd);
            net_stop();
            return 1;
        }
        now = now_ms();
        if (relay_flush(&relay, now) != 0) {
            log_msg("中继冲刷失败");
            free(buf);
            relay_free(&relay);
            sock_close(fd);
            net_stop();
            return 1;
        }
    }
    free(buf);
    relay_free(&relay);
    sock_close(fd);
    net_stop();
    log_msg("中继结束 转发包=%lu 模拟丢弃=%lu", relay.sent_pkts, relay.loss.dropped);
    return 0;
}

static int run_dest(LiveConfig *cfg) {
    Dest dest;
    FileOut fo;
    Sock fd = SOCK_BAD;
    FILE *fp = stdout;
    int own = 0;
    uint8_t *buf;
    memset(&dest, 0, sizeof(dest));
    memset(&fo, 0, sizeof(fo));
    if (cfg->listen_port <= 0) {
        log_msg("目的需要 --listen");
        return 1;
    }
    if (cfg->output && strcmp(cfg->output, "-") != 0) {
        fp = fopen(cfg->output, "wb");
        if (!fp) {
            log_msg("打不开输出 %s", cfg->output);
            return 1;
        }
        own = 1;
    } else {
        set_binary(stdout);
    }
    fo.fp = fp;
    fo.indexed = !cfg->plain_out;
    dest.delay_ms = cfg->delay_ms;
    dest.coalesce_ms = cfg->coalesce_ms;
    dest.window = cfg->window > 0 ? cfg->window : 1;
    dest.log = cfg->log;
    dest.flow = cfg->flow;
    dest.seed = cfg->seed;
    dest.out = file_out;
    dest.out_ctx = &fo;
    if (net_start() != 0) {
        if (own) {
            fclose(fp);
        }
        return 1;
    }
    if (udp_bind(&fd, cfg->bind_host, cfg->listen_port) != 0) {
        log_msg("目的套接字失败");
        net_stop();
        if (own) {
            fclose(fp);
        }
        return 1;
    }
    buf = (uint8_t *)live_alloc((size_t)LIVE_HDR + LIVE_MAX_T);
    log_msg("目的 听 %s:%d 播放延迟=%dms", cfg->bind_host ? cfg->bind_host : "0.0.0.0",
            cfg->listen_port, cfg->delay_ms);
    while (!live_stop) {
        uint64_t now = now_ms();
        int wait = dest_wait_ms(&dest, now);
        int rc;
        if (cfg->idle_sec > 0 && dest.last_rx > 0) {
            uint64_t idle_at = dest.last_rx + (uint64_t)cfg->idle_sec * 1000ull;
            int iw = idle_at <= now ? 0 : (int)(idle_at - now);
            if (iw < wait) {
                wait = iw;
            }
            if (now >= idle_at) {
                break;
            }
        }
        rc = wait_fd(fd, wait);
        now = now_ms();
        if (rc > 0) {
            for (;;) {
                int n = udp_recv(fd, buf, LIVE_HDR + LIVE_MAX_T);
                if (n == 0) {
                    break;
                }
                if (n < 0) {
                    log_msg("目的接收失败");
                    free(buf);
                    dest_free(&dest);
                    sock_close(fd);
                    net_stop();
                    if (own) {
                        fclose(fp);
                    }
                    return 1;
                }
                dest_packet(&dest, buf, n, now);
            }
        } else if (rc < 0) {
            log_msg("目的 select 失败");
            free(buf);
            dest_free(&dest);
            sock_close(fd);
            net_stop();
            if (own) {
                fclose(fp);
            }
            return 1;
        }
        now = now_ms();
        if (dest_pump(&dest, now) != 0) {
            log_msg("写出帧失败");
            free(buf);
            dest_free(&dest);
            sock_close(fd);
            net_stop();
            if (own) {
                fclose(fp);
            }
            return 1;
        }
    }
    if (dest_pump(&dest, now_ms()) != 0) {
        log_msg("写出帧失败");
    }
    log_msg("目的结束 播出=%lu 丢掉=%lu", dest.emitted, dest.dropped);
    free(buf);
    dest_free(&dest);
    sock_close(fd);
    net_stop();
    if (own) {
        fclose(fp);
    }
    return 0;
}

static int capture_send(void *ctx, const uint8_t *pkt, int len) {
    PktList *pl = (PktList *)ctx;
    Pkt *item;
    if (pl->n == pl->cap) {
        int ncap = pl->cap ? pl->cap * 2 : 16;
        Pkt *np = (Pkt *)realloc(pl->p, (size_t)ncap * sizeof(Pkt));
        if (!np) {
            return -1;
        }
        pl->p = np;
        pl->cap = ncap;
    }
    item = &pl->p[pl->n++];
    item->data = (uint8_t *)live_alloc((size_t)len);
    memcpy(item->data, pkt, (size_t)len);
    item->len = len;
    return 0;
}

static void pkt_list_free(PktList *pl) {
    int i;
    for (i = 0; i < pl->n; i++) {
        free(pl->p[i].data);
    }
    free(pl->p);
    pl->p = NULL;
    pl->n = 0;
    pl->cap = 0;
}

static int list_out(void *ctx, uint32_t gen, uint32_t pts, const uint8_t *p, uint32_t n) {
    OutList *o = (OutList *)ctx;
    OutFrame *f;
    if (o->n == o->cap) {
        int ncap = o->cap ? o->cap * 2 : 8;
        OutFrame *nf = (OutFrame *)realloc(o->f, (size_t)ncap * sizeof(OutFrame));
        if (!nf) {
            return -1;
        }
        o->f = nf;
        o->cap = ncap;
    }
    f = &o->f[o->n++];
    f->gen = gen;
    f->pts = pts;
    f->n = n;
    f->p = (uint8_t *)live_alloc(n ? n : 1);
    if (n) {
        memcpy(f->p, p, n);
    }
    return 0;
}

static void out_list_free(OutList *o) {
    int i;
    for (i = 0; i < o->n; i++) {
        free(o->f[i].p);
    }
    free(o->f);
    o->f = NULL;
    o->n = 0;
}

static int decode_list(PktList *pl, int K, int T, uint64_t seed, uint8_t *dst) {
    BatsSymbol *syms;
    int i;
    int ok;
    if (pl->n < 1) {
        return 0;
    }
    syms = (BatsSymbol *)live_alloc((size_t)pl->n * sizeof(BatsSymbol));
    for (i = 0; i < pl->n; i++) {
        LiveHdr h;
        const uint8_t *payload;
        if (unpack_packet(pl->p[i].data, pl->p[i].len, &h, &payload) != 0) {
            free(syms);
            return -1;
        }
        syms[i].batch_id = h.batch;
        memcpy(syms[i].coeff, h.coeff, BATS_M);
        syms[i].payload = payload;
    }
    ok = bats_decode(syms, pl->n, K, T, seed, dst, NULL, NULL);
    free(syms);
    return ok;
}

static int fill_frame(uint8_t *frame, int n, int salt) {
    int i;
    for (i = 0; i < n; i++) {
        frame[i] = (uint8_t)(i * 17 + salt);
    }
    return 0;
}

static int selftest_logic(void) {
    enum { T = 32, FRAME = 256, K = 8 };
    uint8_t frame[FRAME];
    uint8_t dst[K * T];
    uint8_t src[K * T];
    PktList all;
    PktList relayed;
    Relay relay;
    Dest dest;
    OutList outs;
    LiveConfig cfg;
    Loss loss;
    const uint8_t crc_sample[] = "123456789";
    int b;
    int ok;
    int need = -1;
    int i;
    memset(&all, 0, sizeof(all));
    memset(&relayed, 0, sizeof(relayed));
    memset(&relay, 0, sizeof(relay));
    memset(&dest, 0, sizeof(dest));
    memset(&outs, 0, sizeof(outs));
    memset(&cfg, 0, sizeof(cfg));
    memset(&loss, 0, sizeof(loss));
    if (batches_for(32, 25) != 9 || batches_for(8, 0) != 3) {
        log_msg("selftest batch 数不对");
        return 1;
    }
    if (crc32_ieee(crc_sample, 9) != 0xCBF43926u) {
        log_msg("selftest CRC 不对");
        return 1;
    }
    fill_frame(frame, FRAME, 3);
    memset(src, 0, sizeof(src));
    memcpy(src, frame, FRAME);
    if (emit_frame(src, FRAME, T, K, 1, 0xB175ULL, 0, 0, 0, NULL, capture_send, &all, NULL) != 0) {
        log_msg("selftest 组包失败");
        return 1;
    }
    memset(dst, 0, sizeof(dst));
    ok = decode_list(&all, K, T, 0xB175ULL, dst);
    if (ok < 0 || (ok == 1 && memcmp(dst, frame, FRAME) != 0)) {
        log_msg("selftest 单个 batch 的结果不对");
        pkt_list_free(&all);
        return 1;
    }
    pkt_list_free(&all);
    for (b = 1; b <= 64; b++) {
        unsigned long sent = 0;
        memset(dst, 0, sizeof(dst));
        if (emit_frame(src, FRAME, T, K, b, 0xB175ULL, 0, 0, 0, NULL, capture_send, &all, &sent) !=
            0) {
            log_msg("selftest 编码失败");
            pkt_list_free(&all);
            return 1;
        }
        ok = decode_list(&all, K, T, 0xB175ULL, dst);
        if (ok < 0) {
            log_msg("selftest 译码参数失败");
            pkt_list_free(&all);
            return 1;
        }
        if (ok == 1) {
            if (memcmp(dst, frame, FRAME) != 0) {
                log_msg("selftest 译码字节不一致");
                pkt_list_free(&all);
                return 1;
            }
            need = b;
            break;
        }
        pkt_list_free(&all);
    }
    if (need < 1) {
        log_msg("selftest 64 个 batch 仍未译出");
        pkt_list_free(&all);
        return 1;
    }
    pkt_list_free(&all);
    {
        enum { K2 = 32, T2 = 16, F2 = K2 * T2 };
        uint8_t frame2[F2];
        uint8_t src2[F2];
        uint8_t dst2[F2];
        int nb = batches_for(K2, 25);
        fill_frame(frame2, F2, 21);
        memset(src2, 0, sizeof(src2));
        memcpy(src2, frame2, F2);
        if (emit_frame(src2, F2, T2, K2, nb, 0xB175ULL, 0, 0, 0, NULL, capture_send, &all, NULL) !=
                0 ||
            decode_list(&all, K2, T2, 0xB175ULL, dst2) != 1 || memcmp(dst2, frame2, F2) != 0) {
            log_msg("selftest 默认 25%% 冗余没能译出 K=%d（batch=%d）", K2, nb);
            pkt_list_free(&all);
            return 1;
        }
        pkt_list_free(&all);
    }

    relay.flush_ms = 2;
    relay.window = 64;
    relay.recode_state = 0xB175E1ULL;
    relay.send = capture_send;
    relay.send_ctx = &relayed;
    for (b = 0; b < 80; b++) {
        uint8_t coeff[BATS_M * BATS_M];
        uint8_t payload[BATS_M * T];
        int j;
        int before;
        if (bats_encode(src, K, T, 0xB175ULL, (uint32_t)b, coeff, payload) != 0) {
            log_msg("selftest recode 编码失败");
            relay_free(&relay);
            pkt_list_free(&relayed);
            return 1;
        }
        for (j = 0; j < BATS_M - 1; j++) {
            LiveHdr h;
            uint8_t pkt[LIVE_HDR + T];
            int len;
            memset(&h, 0, sizeof(h));
            h.gen = 0;
            h.batch = (uint16_t)b;
            h.K = K;
            h.T = T;
            h.n_batches = 80;
            h.frame_bytes = FRAME;
            h.crc = crc32_ieee(frame, FRAME);
            memcpy(h.coeff, coeff + (size_t)j * BATS_M, BATS_M);
            len = build_data_packet(pkt, (int)sizeof(pkt), &h, payload + (size_t)j * T);
            if (len < 0 || relay_packet(&relay, pkt, len, 5000) != 0) {
                log_msg("selftest 中继收包失败");
                relay_free(&relay);
                pkt_list_free(&relayed);
                return 1;
            }
        }
        before = relayed.n;
        if (relay_flush(&relay, 5000) != 0 || relayed.n != before) {
            log_msg("selftest 未到冲刷时间不该转发");
            relay_free(&relay);
            pkt_list_free(&relayed);
            return 1;
        }
        if (relay_flush(&relay, 5002) != 0 || relayed.n != before + BATS_M) {
            log_msg("selftest 缺包 batch 应转发 8 个包");
            relay_free(&relay);
            pkt_list_free(&relayed);
            return 1;
        }
        memset(dst, 0, sizeof(dst));
        ok = decode_list(&relayed, K, T, 0xB175ULL, dst);
        if (ok == 1) {
            if (memcmp(dst, frame, FRAME) != 0) {
                log_msg("selftest 中继后的字节不一致");
                relay_free(&relay);
                pkt_list_free(&relayed);
                return 1;
            }
            break;
        }
    }
    if (ok != 1) {
        log_msg("selftest 部分 batch 经中继后未能译出");
        relay_free(&relay);
        pkt_list_free(&relayed);
        return 1;
    }
    relay_free(&relay);
    pkt_list_free(&relayed);

    cfg.delay_ms = 50;
    cfg.coalesce_ms = 1;
    cfg.window = 64;
    cfg.seed = 0xB175ULL;
    cfg.log = 0;
    dest.delay_ms = cfg.delay_ms;
    dest.coalesce_ms = cfg.coalesce_ms;
    dest.window = cfg.window;
    dest.seed = cfg.seed;
    dest.out = list_out;
    dest.out_ctx = &outs;
    if (emit_frame(src, FRAME, T, K, need, cfg.seed, 0, 0, 0, NULL, capture_send, &all, NULL) != 0) {
        log_msg("selftest 播放用的帧编码失败");
        return 1;
    }
    for (i = 0; i < all.n; i++) {
        dest_packet(&dest, all.p[i].data, all.p[i].len, 1000);
    }
    if (dest_pump(&dest, 1000) != 0 || outs.n != 0) {
        log_msg("selftest 播放时间未到不该出画");
        pkt_list_free(&all);
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    if (dest_pump(&dest, 1050) != 0 || outs.n != 1 || outs.f[0].gen != 0 ||
        memcmp(outs.f[0].p, frame, FRAME) != 0) {
        log_msg("selftest 到点应播出第 0 帧");
        pkt_list_free(&all);
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    pkt_list_free(&all);
    fill_frame(frame, FRAME, 9);
    memset(src, 0, sizeof(src));
    memcpy(src, frame, FRAME);
    if (emit_frame(src, FRAME, T, K, need, cfg.seed, 2, 80, 0, NULL, capture_send, &all, NULL) != 0) {
        log_msg("selftest 第 2 帧编码失败");
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    for (i = 0; i < all.n; i++) {
        dest_packet(&dest, all.p[i].data, all.p[i].len, 1100);
    }
    if (dest_pump(&dest, 1100) != 0 || outs.n != 1) {
        log_msg("selftest 第 2 帧不该提前播出");
        pkt_list_free(&all);
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    if (dest_pump(&dest, 1130) != 0 || outs.n != 2 || outs.f[1].gen != 2 || dest.dropped < 1 ||
        memcmp(outs.f[1].p, frame, FRAME) != 0) {
        log_msg("selftest 缺席的第 1 帧应被跳过，第 2 帧应播出");
        pkt_list_free(&all);
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    pkt_list_free(&all);
    fill_frame(frame, FRAME, 11);
    memset(src, 0, sizeof(src));
    memcpy(src, frame, FRAME);
    if (emit_frame(src, FRAME, T, K, need, cfg.seed, 3, 120, 0, NULL, capture_send, &all, NULL) !=
        0) {
        log_msg("selftest CRC 帧编码失败");
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    for (i = 0; i < all.n; i++) {
        all.p[i].data[28] ^= 0x01u;
        dest_packet(&dest, all.p[i].data, all.p[i].len, 1300);
    }
    if (dest_pump(&dest, 1300) != 0 || outs.n != 2) {
        log_msg("selftest CRC 不一致的帧不该播出");
        pkt_list_free(&all);
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    pkt_list_free(&all);
    dest_free(&dest);
    out_list_free(&outs);
    (void)loss;
    log_msg("逻辑自测通过，无损译码用了 %d 个 batch", need);
    return 0;
}

static int selftest_udp(void) {
    enum { T = 32, FRAME = 256, K = 8, DELAY = 200 };
    uint8_t frame[FRAME];
    uint8_t src[K * T];
    uint8_t buf[LIVE_HDR + T];
    PktList all;
    Dest dest;
    OutList outs;
    Sock fd = SOCK_BAD;
    UdpOut udp;
    int port;
    int i;
    int got = 0;
    uint64_t t0;
    memset(&all, 0, sizeof(all));
    memset(&dest, 0, sizeof(dest));
    memset(&outs, 0, sizeof(outs));
    memset(&udp, 0, sizeof(udp));
    fill_frame(frame, FRAME, 4);
    memset(src, 0, sizeof(src));
    memcpy(src, frame, FRAME);
    if (emit_frame(src, FRAME, T, K, 32, 0xB175ULL, 7, 40, 0, NULL, capture_send, &all, NULL) !=
            0 ||
        all.n < 1) {
        log_msg("selftest UDP 组包失败");
        return 1;
    }
    if (net_start() != 0) {
        pkt_list_free(&all);
        return 1;
    }
    if (udp_bind(&fd, "127.0.0.1", 0) != 0) {
        log_msg("selftest UDP 绑定失败");
        net_stop();
        pkt_list_free(&all);
        return 1;
    }
    port = udp_local_port(fd);
    if (port <= 0 || udp_peer(&udp.peer, "127.0.0.1", port) != 0) {
        log_msg("selftest UDP 端口失败");
        sock_close(fd);
        net_stop();
        pkt_list_free(&all);
        return 1;
    }
    udp.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp.fd == SOCK_BAD) {
        sock_close(fd);
        net_stop();
        pkt_list_free(&all);
        return 1;
    }
    dest.delay_ms = DELAY;
    dest.coalesce_ms = 1;
    dest.window = 64;
    dest.seed = 0xB175ULL;
    dest.out = list_out;
    dest.out_ctx = &outs;
    for (i = 0; i < all.n; i++) {
        int n;
        if (udp_send(&udp, all.p[i].data, all.p[i].len) != 0) {
            log_msg("selftest UDP 发送失败");
            sock_close(udp.fd);
            sock_close(fd);
            net_stop();
            pkt_list_free(&all);
            dest_free(&dest);
            return 1;
        }
        if (wait_fd(fd, 200) <= 0) {
            log_msg("selftest UDP 没收包");
            sock_close(udp.fd);
            sock_close(fd);
            net_stop();
            pkt_list_free(&all);
            dest_free(&dest);
            return 1;
        }
        n = udp_recv(fd, buf, (int)sizeof(buf));
        if (n <= 0) {
            log_msg("selftest UDP 接收失败");
            sock_close(udp.fd);
            sock_close(fd);
            net_stop();
            pkt_list_free(&all);
            dest_free(&dest);
            return 1;
        }
        dest_packet(&dest, buf, n, now_ms());
        got++;
    }
    t0 = dest.base_local;
    sleep_until(t0 + (uint64_t)DELAY + 30ull);
    if (dest_pump(&dest, now_ms()) != 0 || outs.n != 1 || outs.f[0].gen != 7 ||
        memcmp(outs.f[0].p, frame, FRAME) != 0) {
        log_msg("selftest UDP 播出不正确，收到 %d / %d", got, all.n);
        sock_close(udp.fd);
        sock_close(fd);
        net_stop();
        pkt_list_free(&all);
        dest_free(&dest);
        out_list_free(&outs);
        return 1;
    }
    sock_close(udp.fd);
    sock_close(fd);
    net_stop();
    pkt_list_free(&all);
    dest_free(&dest);
    out_list_free(&outs);
    log_msg("UDP 自测通过");
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "用法:\n"
            "  bats_live --role source --to 主机:端口 [选项]\n"
            "  bats_live --role relay --listen 端口 --to 主机:端口 [选项]\n"
            "  bats_live --role dest --listen 端口 [选项]\n"
            "  bats_live --role selftest\n"
            "\n"
            "源从 --input 读帧，默认标准输入。每帧是 4 字节大端长度加载荷。\n"
            "目的把译出的帧写到 --output，默认标准输出。默认每帧前面有 generation 和 pts。\n"
            "\n"
            "  --fps N             源按这个帧率发送，0 表示来一帧发一帧\n"
            "  --t N               载荷字节，默认 1200，应能放进 MTU\n"
            "  --overhead-pct N    在基础发送量之上再多发的比例，默认 25\n"
            "  --flush-ms N        中继没凑满 8 个包时的冲刷，默认 2\n"
            "  --delay-ms N        目的从第一包算起的播放等待，默认 80\n"
            "  --coalesce-ms N     目的安静这么久再译，默认 1\n"
            "  --window N          同时留着的帧数，默认 64\n"
            "  --max-k N           一帧最多多少个源包，默认 1024\n"
            "  --frame-bytes N     输入是定长裸帧，不带长度前缀\n"
            "  --seed N            编码种子，默认 0xB175\n"
            "  --recode-seed N     这一跳中继的混合种子\n"
            "  --flow N            流编号，默认 0\n"
            "  --loss-pct N        发送前按这个百分比丢掉包，用来做实验\n"
            "  --idle-sec N        中继和目的这么久没有包就退出，0 表示一直跑\n"
            "  --psi 文件          换一份度数分布，源和目的要用同一份\n"
            "  --plain-out         输出只有长度和字节\n"
            "  --bind 地址         监听地址，默认 0.0.0.0\n");
}

static int parse_i(const char *s, int *out) {
    char *end = NULL;
    long v;
    if (!s || !*s) {
        return -1;
    }
    v = strtol(s, &end, 0);
    if (end == s || *end) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int parse_u64(const char *s, uint64_t *out) {
    uint64_t v = 0;
    int base = 10;
    int any = 0;
    if (!s || !*s) {
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; *s; s++) {
        int dig;
        if (*s >= '0' && *s <= '9') {
            dig = *s - '0';
        } else if (*s >= 'a' && *s <= 'f') {
            dig = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'F') {
            dig = *s - 'A' + 10;
        } else {
            return -1;
        }
        if (dig >= base) {
            return -1;
        }
        if (v > (UINT64_MAX - (uint64_t)dig) / (uint64_t)base) {
            return -1;
        }
        v = v * (uint64_t)base + (uint64_t)dig;
        any = 1;
    }
    if (!any) {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_host_port(const char *s, char *host, size_t host_cap, int *port) {
    const char *colon = strrchr(s, ':');
    size_t n;
    if (!colon || colon == s) {
        return -1;
    }
    n = (size_t)(colon - s);
    if (n == 0 || n + 1 > host_cap) {
        return -1;
    }
    memcpy(host, s, n);
    host[n] = 0;
    if (parse_i(colon + 1, port) != 0 || *port <= 0 || *port > 65535) {
        return -1;
    }
    return 0;
}

static const char *need_val(int *i, int argc, char **argv) {
    if (*i + 1 >= argc) {
        log_msg("%s 缺少参数", argv[*i]);
        exit(2);
    }
    *i += 1;
    return argv[*i];
}

int main(int argc, char **argv) {
    LiveConfig cfg;
    int i;
    char host[256];
    int port = 0;
    memset(&cfg, 0, sizeof(cfg));
    cfg.T = 1200;
    cfg.overhead_pct = 25;
    cfg.flush_ms = 2;
    cfg.delay_ms = 80;
    cfg.coalesce_ms = 1;
    cfg.max_k = 1024;
    cfg.window = 64;
    cfg.seed = 0xB175ULL;
    cfg.recode_seed = 0xB175E1ULL;
    cfg.log = 1;
    cfg.bind_host = "0.0.0.0";
    if (argc < 2) {
        usage();
        return 2;
    }
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "--role") == 0) {
            v = need_val(&i, argc, argv);
            if (strcmp(v, "source") == 0) {
                cfg.role = 1;
            } else if (strcmp(v, "relay") == 0) {
                cfg.role = 2;
            } else if (strcmp(v, "dest") == 0) {
                cfg.role = 3;
            } else if (strcmp(v, "selftest") == 0) {
                cfg.role = 4;
            } else {
                log_msg("未知角色 %s", v);
                return 2;
            }
        } else if (strcmp(a, "--to") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_host_port(v, host, sizeof(host), &port) != 0) {
                log_msg("--to 要写成 主机:端口");
                return 2;
            }
            cfg.to_host = live_alloc(strlen(host) + 1);
            memcpy((char *)cfg.to_host, host, strlen(host) + 1);
            cfg.to_port = port;
        } else if (strcmp(a, "--bind") == 0) {
            cfg.bind_host = need_val(&i, argc, argv);
        } else if (strcmp(a, "--listen") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.listen_port) != 0 || cfg.listen_port <= 0 ||
                cfg.listen_port > 65535) {
                log_msg("--listen 不合法");
                return 2;
            }
        } else if (strcmp(a, "--input") == 0) {
            cfg.input = need_val(&i, argc, argv);
        } else if (strcmp(a, "--output") == 0) {
            cfg.output = need_val(&i, argc, argv);
        } else if (strcmp(a, "--fps") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.fps) != 0 || cfg.fps < 0) {
                log_msg("--fps 不合法");
                return 2;
            }
        } else if (strcmp(a, "--t") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.T) != 0 || cfg.T < 1 || cfg.T > LIVE_MAX_T) {
                log_msg("--t 不合法");
                return 2;
            }
        } else if (strcmp(a, "--overhead-pct") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.overhead_pct) != 0 || cfg.overhead_pct < 0 ||
                cfg.overhead_pct > 1000) {
                log_msg("--overhead-pct 不合法");
                return 2;
            }
        } else if (strcmp(a, "--flush-ms") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.flush_ms) != 0 || cfg.flush_ms < 0) {
                log_msg("--flush-ms 不合法");
                return 2;
            }
        } else if (strcmp(a, "--delay-ms") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.delay_ms) != 0 || cfg.delay_ms < 0) {
                log_msg("--delay-ms 不合法");
                return 2;
            }
        } else if (strcmp(a, "--coalesce-ms") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.coalesce_ms) != 0 || cfg.coalesce_ms < 0) {
                log_msg("--coalesce-ms 不合法");
                return 2;
            }
        } else if (strcmp(a, "--window") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.window) != 0 || cfg.window < 1) {
                log_msg("--window 不合法");
                return 2;
            }
        } else if (strcmp(a, "--max-k") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.max_k) != 0 || cfg.max_k < 1 || cfg.max_k > LIVE_MAX_K) {
                log_msg("--max-k 不合法");
                return 2;
            }
        } else if (strcmp(a, "--frame-bytes") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.frame_bytes) != 0 || cfg.frame_bytes < 0) {
                log_msg("--frame-bytes 不合法");
                return 2;
            }
        } else if (strcmp(a, "--seed") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_u64(v, &cfg.seed) != 0) {
                log_msg("--seed 不合法");
                return 2;
            }
        } else if (strcmp(a, "--recode-seed") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_u64(v, &cfg.recode_seed) != 0 || cfg.recode_seed == 0) {
                log_msg("--recode-seed 不合法");
                return 2;
            }
        } else if (strcmp(a, "--flow") == 0) {
            int flow = 0;
            v = need_val(&i, argc, argv);
            if (parse_i(v, &flow) != 0 || flow < 0 || flow > 65535) {
                log_msg("--flow 不合法");
                return 2;
            }
            cfg.flow = (uint16_t)flow;
        } else if (strcmp(a, "--loss-pct") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.loss_pct) != 0 || cfg.loss_pct < 0 || cfg.loss_pct > 100) {
                log_msg("--loss-pct 不合法");
                return 2;
            }
        } else if (strcmp(a, "--idle-sec") == 0) {
            v = need_val(&i, argc, argv);
            if (parse_i(v, &cfg.idle_sec) != 0 || cfg.idle_sec < 0) {
                log_msg("--idle-sec 不合法");
                return 2;
            }
        } else if (strcmp(a, "--psi") == 0) {
            cfg.psi_path = need_val(&i, argc, argv);
        } else if (strcmp(a, "--plain-out") == 0) {
            cfg.plain_out = 1;
        } else if (strcmp(a, "--quiet") == 0) {
            cfg.log = 0;
        } else {
            log_msg("未知参数 %s", a);
            return 2;
        }
    }
    if (cfg.role == 0) {
        usage();
        return 2;
    }
    bats_init();
    if (cfg.role != 2 && install_psi(cfg.psi_path) != 0) {
        return 1;
    }
    if (cfg.role == 4) {
        if (selftest_logic() != 0) {
            return 1;
        }
        if (selftest_udp() != 0) {
            return 1;
        }
        log_msg("selftest 通过");
        return 0;
    }
    if (cfg.role == 1) {
        return run_source(&cfg);
    }
    if (cfg.role == 2) {
        return run_relay(&cfg);
    }
    return run_dest(&cfg);
}
