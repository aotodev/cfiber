#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ws_echo.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- SHA-1 and base64: handshake boilerplate, no reactor code here ---- */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
} sha1_t;

static uint32_t rol(uint32_t v, int c) {
    return (v << c) | (v >> (32 - c));
}

static void sha1_block(sha1_t* s, const unsigned char* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

void ws_sha1(const unsigned char* data, size_t len, unsigned char out[20]) {
    sha1_t s = {.h = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}};
    s.len = (uint64_t)len * 8;

    while (len) {
        size_t take = 64 - s.n;
        if (take > len) {
            take = len;
        }
        memcpy(s.buf + s.n, data, take);
        s.n += take, data += take, len -= take;
        if (s.n == 64) {
            sha1_block(&s, s.buf);
            s.n = 0;
        }
    }

    /* Pad: 0x80, zeros, then the 64-bit bit length. */
    s.buf[s.n++] = 0x80;
    if (s.n > 56) {
        while (s.n < 64) {
            s.buf[s.n++] = 0;
        }
        sha1_block(&s, s.buf);
        s.n = 0;
    }
    while (s.n < 56) {
        s.buf[s.n++] = 0;
    }
    for (int i = 7; i >= 0; i--) {
        s.buf[s.n++] = (unsigned char)(s.len >> (i * 8));
    }
    sha1_block(&s, s.buf);

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(s.h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s.h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s.h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s.h[i]);
    }
}

int ws_base64(const unsigned char* in, size_t len, char* out, size_t out_cap) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4;
    if (out_cap < need + 1) {
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            n |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            n |= in[i + 2];
        }
        out[o++] = t[(n >> 18) & 63];
        out[o++] = t[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? t[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? t[n & 63] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

/* ---- WebSocket framing (all I/O goes through the reactor helpers) ---- */

#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define MAX_PAYLOAD (64 * 1024)

enum { OP_CONT = 0x0, OP_TEXT = 0x1, OP_BIN = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xA };

/* Read exactly n bytes. Returns 1 on success, 0 if the peer closed, -1 on error. */
static int read_exact(int fd, void* buf, size_t n) {
    for (size_t off = 0; off < n;) {
        ssize_t r = cfiber_ev_read(fd, (char*)buf + off, n - off);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 1;
}

/* Send one server frame. Server -> client frames MUST NOT be masked (RFC 6455). */
static int send_frame(int fd, int opcode, const unsigned char* payload, size_t len) {
    unsigned char hdr[10];
    size_t h = 0;
    hdr[h++] = (unsigned char)(0x80 | opcode); /* FIN + opcode */
    if (len < 126) {
        hdr[h++] = (unsigned char)len;
    } else if (len <= 0xFFFF) {
        hdr[h++] = 126;
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)(len);
    } else {
        hdr[h++] = 127;
        for (int i = 7; i >= 0; i--) {
            hdr[h++] = (unsigned char)((uint64_t)len >> (i * 8));
        }
    }
    if (cfiber_ev_write(fd, hdr, h) < 0) {
        return -1;
    }
    if (len && cfiber_ev_write(fd, payload, len) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Read one frame into `payload` (unmasked in place). Returns 1 on success,
 * 0 if the peer closed, -1 on error or protocol violation.
 */
static int recv_frame(int fd, int* opcode, unsigned char* payload, size_t* len) {
    unsigned char b[2];
    int rc = read_exact(fd, b, 2);
    if (rc <= 0) {
        return rc;
    }
    *opcode = b[0] & 0x0F;
    int masked = b[1] & 0x80;
    uint64_t n = b[1] & 0x7F;

    if (n == 126) {
        unsigned char e[2];
        if (read_exact(fd, e, 2) <= 0) {
            return -1;
        }
        n = (uint64_t)e[0] << 8 | e[1];
    } else if (n == 127) {
        unsigned char e[8];
        if (read_exact(fd, e, 8) <= 0) {
            return -1;
        }
        n = 0;
        for (int i = 0; i < 8; i++) {
            n = (n << 8) | e[i];
        }
    }
    if (n > MAX_PAYLOAD) {
        return -1;
    }

    unsigned char mask[4] = {0};
    if (masked && read_exact(fd, mask, 4) <= 0) {
        return -1;
    }
    if (n && read_exact(fd, payload, (size_t)n) <= 0) {
        return -1;
    }
    if (masked) {
        for (uint64_t i = 0; i < n; i++) {
            payload[i] ^= mask[i & 3];
        }
    }

    *len = (size_t)n;
    return 1;
}

/* ---- Handshake ---- */

static int do_handshake(int fd) {
    /* Read request headers until the terminating CRLFCRLF. */
    char req[4096];
    size_t total = 0;
    while (total < sizeof(req) - 1) {
        ssize_t r = cfiber_ev_read(fd, req + total, sizeof(req) - 1 - total);
        if (r <= 0) {
            return -1;
        }
        total += (size_t)r;
        req[total] = '\0';
        if (strstr(req, "\r\n\r\n")) {
            break;
        }
    }

    /* Locate Sec-WebSocket-Key (header names are case-insensitive). */
    const char* key = NULL;
    for (char* p = req; *p; p++) {
        if (strncasecmp(p, "Sec-WebSocket-Key:", 18) == 0) {
            key = p + 18;
            break;
        }
    }
    if (!key) {
        return -1;
    }
    while (*key == ' ' || *key == '\t') {
        key++;
    }
    const char* end = strstr(key, "\r\n");
    if (!end) {
        return -1;
    }
    size_t klen = (size_t)(end - key);
    if (klen == 0 || klen > 256) {
        return -1;
    }

    /* accept = base64(sha1(key + magic GUID)). */
    unsigned char concat[256 + 36];
    memcpy(concat, key, klen);
    memcpy(concat + klen, WS_MAGIC, 36);

    unsigned char digest[20];
    ws_sha1(concat, klen + 36, digest);

    char accept[64];
    if (ws_base64(digest, 20, accept, sizeof accept) < 0) {
        return -1;
    }

    char resp[256];
    int rn = snprintf(resp,
                      sizeof resp,
                      "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n",
                      accept);
    if (rn < 0 || (size_t)rn >= sizeof resp) {
        return -1;
    }
    return cfiber_ev_write(fd, resp, (size_t)rn) < 0 ? -1 : 0;
}

/* ---- Fibers ---- */

/* One per connection: handshake, then echo every frame until the peer leaves. */
static void conn_fiber(void* arg) {
    int fd = (int)(intptr_t)arg;

    if (do_handshake(fd) < 0) {
        close(fd);
        return;
    }

    /* Heap, not the fiber stack: 64 KiB per fiber stack would be wasteful, and a
     * static buffer would be corrupted by concurrent connections. */
    unsigned char* payload = malloc(MAX_PAYLOAD);
    if (!payload) {
        close(fd);
        return;
    }

    for (;;) {
        int op;
        size_t len;
        if (recv_frame(fd, &op, payload, &len) <= 0) {
            break;
        }
        if (op == OP_CLOSE) {
            send_frame(fd, OP_CLOSE, payload, len); /* echo the close, then leave */
            break;
        }
        if (op == OP_PING) {
            if (send_frame(fd, OP_PONG, payload, len) < 0) {
                break;
            }
            continue;
        }
        if (op == OP_PONG) {
            continue; /* unsolicited pong: ignore */
        }
        /* text / binary / continuation: echo it straight back */
        if (send_frame(fd, op == OP_CONT ? OP_TEXT : op, payload, len) < 0) {
            break;
        }
    }

    free(payload);
    close(fd);
}

typedef struct {
    int port;
    int max_conns;
} listen_args_t;

/* Accepts connections and spawns one conn_fiber each. */
static void listen_fiber(void* arg) {
    listen_args_t a = *(listen_args_t*)arg;
    free(arg);

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        return;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)a.port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr*)&sa, sizeof sa) < 0) {
        perror("bind");
        close(lfd);
        return;
    }
    if (listen(lfd, 128) < 0) {
        perror("listen");
        close(lfd);
        return;
    }
    fprintf(stderr, "[ws] listening on 127.0.0.1:%d\n", a.port);

    for (int served = 0;;) {
        int c = cfiber_ev_accept(lfd, NULL, NULL);
        if (c < 0) {
            break;
        }
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (!cfiber_ev_spawn(conn_fiber, (void*)(intptr_t)c, NULL)) {
            close(c);
            continue;
        }
        if (a.max_conns > 0 && ++served >= a.max_conns) {
            break;
        }
    }
    close(lfd);
}

int ws_serve(cfiber_reactor_t* r, int port, int max_conns) {
    listen_args_t* a = malloc(sizeof *a);
    if (!a) {
        return -1;
    }
    *a = (listen_args_t){.port = port, .max_conns = max_conns};
    if (!cfiber_reactor_spawn(r, listen_fiber, a, NULL)) {
        free(a);
        return -1;
    }
    return 0;
}
