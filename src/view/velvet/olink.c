/* Optic Link, the sending side (see olink.h). One background thread: announces itself, takes one
 * receiver at a time, sends the pictures it's handed (only the 32 x 32 tiles that changed,
 * zlib-packed) and queues the receiver's buttons. */
#define _GNU_SOURCE
#include "olink.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define TILE 32
#define QN 256

struct OLink {
    pthread_t th;
    pthread_mutex_t mu;
    volatile int quit;
    char name[64], trust[512];
    int lsock, usock, csock;
    int state;
    char code[5], peer[64], peerId[40];
    int tries;
    /* the picture waiting to go out, and the one the receiver has */
    uint16_t *next, *sent;
    int nw, nh, sw, sh, fresh, busy;
    char text[128];
    int textNew;
    OLinkEv q[QN];
    int qh, qt;
    /* handshake / input bytes */
    unsigned char in[512];
    int inLen;
    double lastIn, lastOut, lastBeacon;
    unsigned char *zbuf;
};

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int sendAll(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t k = send(fd, c, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        c += k; n -= (size_t)k;
    }
    return 0;
}

static int sendMsg(OLink *l, int type, const void *a, uint32_t na, const void *b, uint32_t nb) {
    unsigned char h[5] = { (unsigned char)type };
    uint32_t n = na + nb;
    memcpy(h + 1, &n, 4);
    if (sendAll(l->csock, h, 5) || (na && sendAll(l->csock, a, na)) || (nb && sendAll(l->csock, b, nb))) return -1;
    l->lastOut = now();
    return 0;
}

static int trusted(OLink *l, const char *id) {
    FILE *f = fopen(l->trust, "r");
    if (!f) return 0;
    char line[64];
    int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; ok = !strcmp(line, id); }
    fclose(f);
    return ok;
}

static void trust(OLink *l, const char *id) {
    FILE *f = fopen(l->trust, "a");
    if (f) { fprintf(f, "%s\n", id); fclose(f); }
}

static void newCode(OLink *l) {
    unsigned v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, &v, sizeof v) != sizeof v) v = (unsigned)time(NULL); close(fd); }
    snprintf(l->code, sizeof l->code, "%04u", v % 10000);
}

static void drop(OLink *l) {
    if (l->csock >= 0) close(l->csock);
    l->csock = -1;
    pthread_mutex_lock(&l->mu);
    l->state = OLS_WAITING;
    l->peer[0] = l->code[0] = 0;
    l->busy = 0;
    pthread_mutex_unlock(&l->mu);
}

static void beacon(OLink *l) {
    char b[160];
    int n = snprintf(b, sizeof b, "OLINK1 %d %s %s", OLINK_PORT, l->csock >= 0 ? "busy" : "free", l->name);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(OLINK_BEACON_PORT) };
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(l->usock, b, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
    l->lastBeacon = now();
}

/* one line of the handshake */
static void handshake(OLink *l, char *line) {
    char reply[96];
    if (l->state == OLS_WAITING && !strncmp(line, "OLINK1 HELLO ", 13)) {
        char id[40] = "", nm[64] = "";
        sscanf(line + 13, "%39s %63[^\n]", id, nm);
        pthread_mutex_lock(&l->mu);
        snprintf(l->peerId, sizeof l->peerId, "%s", id);
        snprintf(l->peer, sizeof l->peer, "%s", nm[0] ? nm : "receiver");
        pthread_mutex_unlock(&l->mu);
        if (id[0] && trusted(l, id)) goto ok;
        pthread_mutex_lock(&l->mu);
        newCode(l);
        l->tries = 0;
        l->state = OLS_PAIRING;
        pthread_mutex_unlock(&l->mu);
        if (sendAll(l->csock, "CODE\n", 5)) drop(l);
        return;
    }
    if (l->state == OLS_PAIRING && !strncmp(line, "PIN ", 4)) {
        if (!strcmp(line + 4, l->code)) { trust(l, l->peerId); goto ok; }
        if (++l->tries >= 3) { sendAll(l->csock, "NO\n", 3); drop(l); return; }
        if (sendAll(l->csock, "NO\n", 3)) drop(l);
        return;
    }
    drop(l);
    return;
ok:
    snprintf(reply, sizeof reply, "OK %s\n", l->name);
    if (sendAll(l->csock, reply, strlen(reply))) { drop(l); return; }
    pthread_mutex_lock(&l->mu);
    l->state = OLS_LIVE;
    l->code[0] = 0;
    l->sw = l->sh = 0;      /* first picture goes out whole */
    l->fresh = l->nw > 0;
    l->textNew = l->text[0] != 0;
    pthread_mutex_unlock(&l->mu);
}

static void readClient(OLink *l) {
    ssize_t k = recv(l->csock, l->in + l->inLen, sizeof l->in - (size_t)l->inLen, 0);
    if (k <= 0) { drop(l); return; }
    l->inLen += (int)k;
    l->lastIn = now();
    int p = 0;
    while (p < l->inLen && l->csock >= 0) {
        if (l->state != OLS_LIVE) {
            unsigned char *nl = memchr(l->in + p, '\n', (size_t)(l->inLen - p));
            if (!nl) { if (l->inLen - p > 200) drop(l); break; }
            *nl = 0;
            handshake(l, (char *)l->in + p);
            p = (int)(nl - l->in) + 1;
            continue;
        }
        if (l->inLen - p < 5) break;
        uint32_t n;
        memcpy(&n, l->in + p + 1, 4);
        if (n > 64) { drop(l); return; }
        if (l->inLen - p < 5 + (int)n) break;
        unsigned char t = l->in[p], *d = l->in + p + 5;
        OLinkEv e = { 0, 0, 0 };
        if (t == OL_BTN && n >= 2) e = (OLinkEv){ OL_BTN, d[0], d[1] };
        else if (t == OL_AXIS && n >= 3) { int16_t v; memcpy(&v, d + 1, 2); e = (OLinkEv){ OL_AXIS, d[0], v }; }
        if (e.type) {
            pthread_mutex_lock(&l->mu);
            if ((l->qt + 1) % QN != l->qh) { l->q[l->qt] = e; l->qt = (l->qt + 1) % QN; }
            pthread_mutex_unlock(&l->mu);
        }
        p += 5 + (int)n;
    }
    if (l->csock >= 0) { memmove(l->in, l->in + p, (size_t)(l->inLen - p)); l->inLen -= p; }
    else l->inLen = 0;
}

/* sends what changed between [sent] and [next]; called with the picture already copied */
static int sendFrame(OLink *l, uint16_t *px, int w, int h) {
    int full = w != l->sw || h != l->sh;
    if (full) {
        uint16_t wh[2] = { (uint16_t)w, (uint16_t)h };
        if (sendMsg(l, OL_FRAME, wh, 4, NULL, 0)) return -1;
    }
    uint16_t tile[TILE * TILE];
    for (int ty = 0; ty < h; ty += TILE) for (int tx = 0; tx < w; tx += TILE) {
        int tw = w - tx < TILE ? w - tx : TILE, th = h - ty < TILE ? h - ty : TILE, same = !full;
        for (int y = 0; y < th; y++) {
            const uint16_t *a = px + (size_t)(ty + y) * w + tx;
            if (same && memcmp(a, l->sent + (size_t)(ty + y) * w + tx, (size_t)tw * 2)) same = 0;
            memcpy(tile + y * tw, a, (size_t)tw * 2);
        }
        if (same) continue;
        unsigned char hd[9];
        uint16_t v[4] = { (uint16_t)tx, (uint16_t)ty, (uint16_t)tw, (uint16_t)th };
        memcpy(hd, v, 8);
        uLongf zn = compressBound(TILE * TILE * 2);
        uLong raw = (uLong)tw * th * 2;
        if (compress2(l->zbuf, &zn, (const Bytef *)tile, raw, 1) == Z_OK && zn < raw) {
            hd[8] = 1;
            if (sendMsg(l, OL_TILE, hd, 9, l->zbuf, (uint32_t)zn)) return -1;
        } else {
            hd[8] = 0;
            if (sendMsg(l, OL_TILE, hd, 9, tile, (uint32_t)raw)) return -1;
        }
    }
    if (sendMsg(l, OL_END, NULL, 0, NULL, 0)) return -1;
    memcpy(l->sent, px, (size_t)w * h * 2);
    l->sw = w; l->sh = h;
    return 0;
}

static void *run(void *arg) {
    OLink *l = arg;
    uint16_t *work = malloc(OLINK_MAXW * OLINK_MAXH * 2);
    while (!l->quit) {
        struct pollfd p[2] = { { l->lsock, POLLIN, 0 }, { l->csock, POLLIN, 0 } };
        int n = l->csock >= 0 ? 2 : 1;
        poll(p, (nfds_t)n, 15);
        double t = now();
        if (t - l->lastBeacon > 1) beacon(l);
        if (p[0].revents & POLLIN) {
            int c = accept(l->lsock, NULL, NULL);
            if (c >= 0) {
                if (l->csock >= 0) { sendAll(c, "BUSY\n", 5); close(c); }
                else {
                    int one = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    struct timeval tv = { 3, 0 };
                    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                    l->csock = c;
                    l->inLen = 0;
                    l->lastIn = t;
                }
            }
        }
        if (n == 2 && l->csock >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) readClient(l);
        if (l->csock < 0) continue;
        /* a receiver that went quiet (it pings every second), or a pairing nobody finishes */
        if (t - l->lastIn > (l->state == OLS_LIVE ? 6 : 120)) { drop(l); continue; }
        if (l->state != OLS_LIVE) continue;
        pthread_mutex_lock(&l->mu);
        int go = l->fresh, w = l->nw, h = l->nh, tn = l->textNew;
        char text[128];
        if (go) { memcpy(work, l->next, (size_t)w * h * 2); l->fresh = 0; l->busy = 1; }
        if (tn) { memcpy(text, l->text, sizeof text); l->textNew = 0; }
        pthread_mutex_unlock(&l->mu);
        int bad = 0;
        if (tn) bad |= sendMsg(l, OL_TEXT, text, (uint32_t)strlen(text), NULL, 0);
        if (go && !bad) bad |= sendFrame(l, work, w, h);
        if (!bad && t - l->lastOut > 1) bad |= sendMsg(l, OL_PING, NULL, 0, NULL, 0);
        pthread_mutex_lock(&l->mu);
        l->busy = 0;
        pthread_mutex_unlock(&l->mu);
        if (bad) drop(l);
    }
    free(work);
    return NULL;
}

OLink *olink_start(const char *name, const char *trustPath) {
    OLink *l = calloc(1, sizeof *l);
    l->csock = -1;
    snprintf(l->name, sizeof l->name, "%s", name);
    snprintf(l->trust, sizeof l->trust, "%s", trustPath);
    l->lsock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l->lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(OLINK_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (l->lsock < 0 || bind(l->lsock, (struct sockaddr *)&a, sizeof a) || listen(l->lsock, 2)) {
        if (l->lsock >= 0) close(l->lsock);
        free(l);
        return NULL;
    }
    l->usock = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(l->usock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    l->next = malloc(OLINK_MAXW * OLINK_MAXH * 2);
    l->sent = malloc(OLINK_MAXW * OLINK_MAXH * 2);
    l->zbuf = malloc(compressBound(TILE * TILE * 2));
    pthread_mutex_init(&l->mu, NULL);
    pthread_create(&l->th, NULL, run, l);
    return l;
}

void olink_stop(OLink *l) {
    if (!l) return;
    l->quit = 1;
    pthread_join(l->th, NULL);
    if (l->csock >= 0) close(l->csock);
    close(l->lsock);
    close(l->usock);
    free(l->next); free(l->sent); free(l->zbuf);
    pthread_mutex_destroy(&l->mu);
    free(l);
}

int olink_state(OLink *l, char code[5], char *peer, int peerlen) {
    pthread_mutex_lock(&l->mu);
    int s = l->state;
    if (code) memcpy(code, l->code, 5);
    if (peer && peerlen > 0) snprintf(peer, (size_t)peerlen, "%s", l->peer);
    pthread_mutex_unlock(&l->mu);
    return s;
}

int olink_wants_frame(OLink *l) {
    pthread_mutex_lock(&l->mu);
    int r = l->state == OLS_LIVE && !l->fresh && !l->busy;
    pthread_mutex_unlock(&l->mu);
    return r;
}

void olink_frame(OLink *l, const uint16_t *px, int w, int h) {
    if (w > OLINK_MAXW) w = OLINK_MAXW;
    if (h > OLINK_MAXH) h = OLINK_MAXH;
    if (w <= 0 || h <= 0) return;
    pthread_mutex_lock(&l->mu);
    memcpy(l->next, px, (size_t)w * h * 2);
    l->nw = w; l->nh = h;
    l->fresh = 1;
    pthread_mutex_unlock(&l->mu);
}

void olink_text(OLink *l, const char *s) {
    pthread_mutex_lock(&l->mu);
    if (strncmp(l->text, s, sizeof l->text - 1)) { snprintf(l->text, sizeof l->text, "%s", s); l->textNew = 1; }
    pthread_mutex_unlock(&l->mu);
}

int olink_input(OLink *l, OLinkEv *ev) {
    pthread_mutex_lock(&l->mu);
    int r = l->qh != l->qt;
    if (r) { *ev = l->q[l->qh]; l->qh = (l->qh + 1) % QN; }
    pthread_mutex_unlock(&l->mu);
    return r;
}
