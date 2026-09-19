#include "ws_echo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

static uint32_t load_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void sha1_block(sha1_t* s, const unsigned char* p) {
    uint32_t w[80];
    for (size_t i = 0; i < 16; i++) {
        w[i] = load_be32(p + (i * 4));
    }
    for (size_t i = 16; i < 80; i++) {
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

    for (size_t i = 0; i < 5; i++) {
        store_be32(out + (i * 4), s.h[i]);
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

/* ---- Connection: buffered reader over the reactor helpers ---- */

#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define MAX_PAYLOAD ((size_t)64 * 1024)
#define MAX_REQUEST 4096

enum { OP_CONT = 0x0, OP_TEXT = 0x1, OP_BIN = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xA };
enum { CLOSE_NORMAL = 1000, CLOSE_PROTOCOL = 1002, CLOSE_TOO_BIG = 1009 };

typedef struct {
    int fd;
    /* Bytes read past the handshake terminator: a client may pipeline its
     * first frame behind the upgrade request, and they must not be lost. */
    unsigned char carry[MAX_REQUEST];
    size_t carry_off;
    size_t carry_len;
    /* The message being reassembled from fragments, if any. */
    unsigned char* msg;
    size_t msg_len;
    int msg_op; /* OP_TEXT or OP_BIN while assembling, -1 otherwise */
} conn_t;

/* Read exactly n bytes, carried bytes first. 1 on success, 0 if the peer closed, -1 on error. */
static int conn_read_exact(conn_t* c, void* buf, size_t n) {
    size_t off = 0;
    if (c->carry_off < c->carry_len) {
        size_t take = c->carry_len - c->carry_off;
        if (take > n) {
            take = n;
        }
        memcpy(buf, c->carry + c->carry_off, take);
        c->carry_off += take;
        off = take;
    }
    while (off < n) {
        ssize_t r = cfiber_ev_read(c->fd, (char*)buf + off, n - off);
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
        hdr[h++] = (unsigned char)len;
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

static int send_close(int fd, int code) {
    unsigned char body[2] = {(unsigned char)(code >> 8), (unsigned char)code};
    return send_frame(fd, OP_CLOSE, body, sizeof body);
}

typedef struct {
    int fin;
    int opcode;
    size_t len;
} frame_hdr_t;

/*
 * Read one client frame into `payload` (unmasked in place). Returns 1 on
 * success, 0 if the peer closed, -1 on I/O error, or the close code to send
 * (> 0, >= 1000) on a protocol violation.
 */
static int recv_frame(conn_t* c, frame_hdr_t* hdr, unsigned char* payload) {
    unsigned char b[2];
    int rc = conn_read_exact(c, b, 2);
    if (rc <= 0) {
        return rc;
    }
    hdr->fin = b[0] & 0x80;
    hdr->opcode = b[0] & 0x0F;
    const int rsv = b[0] & 0x70;
    const int masked = b[1] & 0x80;
    uint64_t n = b[1] & 0x7F;

    /* RSV bits need an extension, reserved opcodes have no meaning, and a
     * client frame is always masked. */
    const int known = hdr->opcode <= OP_BIN || (hdr->opcode >= OP_CLOSE && hdr->opcode <= OP_PONG);
    if (rsv || !known || !masked) {
        return CLOSE_PROTOCOL;
    }

    if (n == 126) {
        unsigned char e[2];
        if (conn_read_exact(c, e, 2) <= 0) {
            return -1;
        }
        n = ((uint64_t)e[0] << 8) | e[1];
        if (n < 126) {
            return CLOSE_PROTOCOL; /* length must use the shortest encoding */
        }
    } else if (n == 127) {
        unsigned char e[8];
        if (conn_read_exact(c, e, 8) <= 0) {
            return -1;
        }
        n = 0;
        for (int i = 0; i < 8; i++) {
            n = (n << 8) | e[i];
        }
        if (n <= 0xFFFF || (n >> 63)) {
            return CLOSE_PROTOCOL;
        }
    }
    if (hdr->opcode >= OP_CLOSE && (!hdr->fin || n > 125)) {
        return CLOSE_PROTOCOL; /* control frames: unfragmented, at most 125 bytes */
    }
    if (n > MAX_PAYLOAD) {
        return CLOSE_TOO_BIG;
    }

    unsigned char mask[4];
    if (conn_read_exact(c, mask, 4) <= 0) {
        return -1;
    }
    if (n && conn_read_exact(c, payload, (size_t)n) <= 0) {
        return -1;
    }
    for (uint64_t i = 0; i < n; i++) {
        payload[i] ^= mask[i & 3];
    }

    hdr->len = (size_t)n;
    return 1;
}

/* ---- Handshake ---- */

/* Offset just past the first CRLFCRLF in buf, or 0 if there is none. */
static size_t find_header_end(const unsigned char* buf, size_t len) {
    for (size_t i = 3; i < len; i++) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') {
            return i + 1;
        }
    }
    return 0;
}

/* Value of the header `name` (case-insensitive, anchored at a line start),
 * trimmed of leading blanks and with `*vlen` its length; NULL if absent. */
static const char* find_header(const char* req, const char* name, size_t* vlen) {
    const size_t nlen = strlen(name);
    for (const char* line = strstr(req, "\r\n"); line; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char* v = line + nlen + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            const char* end = strstr(v, "\r\n");
            if (!end) {
                return NULL;
            }
            *vlen = (size_t)(end - v);
            return v;
        }
    }
    return NULL;
}

static int header_is(const char* req, const char* name, const char* want) {
    size_t vlen = 0;
    const char* v = find_header(req, name, &vlen);
    return v && vlen == strlen(want) && strncasecmp(v, want, vlen) == 0;
}

static void reject(int fd, const char* status) {
    char resp[128];
    int n = snprintf(resp, sizeof resp, "HTTP/1.1 %s\r\nConnection: close\r\n\r\n", status);
    if (n > 0) {
        (void)cfiber_ev_write(fd, resp, (size_t)n);
    }
}

/*
 * Read the upgrade request, validate it, answer 101. Bytes that arrived after
 * the request stay in the connection's carry buffer for the frame reader.
 */
static int do_handshake(conn_t* c) {
    unsigned char* req = c->carry;
    size_t total = 0;
    size_t hdr_end = 0;
    while (!hdr_end) {
        if (total >= MAX_REQUEST - 1) {
            reject(c->fd, "431 Request Header Fields Too Large");
            return -1;
        }
        ssize_t r = cfiber_ev_read(c->fd, req + total, MAX_REQUEST - 1 - total);
        if (r <= 0) {
            return -1;
        }
        total += (size_t)r;
        hdr_end = find_header_end(req, total);
    }
    c->carry_off = hdr_end;
    c->carry_len = total;

    /* NUL-terminate the header block for the string scans; the frame bytes
     * behind it are untouched. */
    char saved = (char)req[hdr_end - 1];
    req[hdr_end - 1] = '\0';
    const char* text = (const char*)req;

    size_t klen = 0;
    const char* key = find_header(text, "Sec-WebSocket-Key", &klen);
    const int ok = strncmp(text, "GET ", 4) == 0 && header_is(text, "Upgrade", "websocket")
                   && header_is(text, "Sec-WebSocket-Version", "13") && key && klen > 0 && klen <= 256;
    if (!ok) {
        req[hdr_end - 1] = (unsigned char)saved;
        reject(c->fd, "400 Bad Request");
        return -1;
    }

    /* accept = base64(sha1(key + magic GUID)). */
    unsigned char concat[256 + 36];
    /* NOLINTBEGIN(bugprone-not-null-terminated-result): binary concat, hashed */
    memcpy(concat, key, klen);
    memcpy(concat + klen, WS_MAGIC, 36);
    /* NOLINTEND(bugprone-not-null-terminated-result) */
    req[hdr_end - 1] = (unsigned char)saved;

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
    return cfiber_ev_write(c->fd, resp, (size_t)rn) < 0 ? -1 : 0;
}

/* ---- Fibers ---- */

/*
 * Handle one data frame. Fragments are collected until FIN and echoed as one
 * message with the first fragment's opcode. Returns 0, or the close code for a
 * violation (a data frame while a message is open, a continuation without one).
 */
static int on_data_frame(conn_t* c, const frame_hdr_t* h, const unsigned char* payload) {
    if (h->opcode == OP_CONT) {
        if (c->msg_op < 0) {
            return CLOSE_PROTOCOL;
        }
    } else {
        if (c->msg_op >= 0) {
            return CLOSE_PROTOCOL;
        }
        if (h->fin) {
            return send_frame(c->fd, h->opcode, payload, h->len) < 0 ? -1 : 0;
        }
        c->msg_op = h->opcode;
        c->msg_len = 0;
    }
    if (h->len > MAX_PAYLOAD - c->msg_len) {
        return CLOSE_TOO_BIG;
    }
    memcpy(c->msg + c->msg_len, payload, h->len);
    c->msg_len += h->len;
    if (!h->fin) {
        return 0;
    }
    const int op = c->msg_op;
    c->msg_op = -1;
    return send_frame(c->fd, op, c->msg, c->msg_len) < 0 ? -1 : 0;
}

/* One per connection: handshake, then echo every message until the peer leaves. */
static void conn_fiber(void* arg) {
    conn_t* c = calloc(1, sizeof *c);
    if (!c) {
        close((int)(intptr_t)arg);
        return;
    }
    c->fd = (int)(intptr_t)arg;
    c->msg_op = -1;

    /* Heap, not the fiber stack: two 64 KiB buffers per fiber stack would be
     * wasteful, and a static buffer would be shared between connections. */
    unsigned char* payload = malloc(MAX_PAYLOAD);
    c->msg = malloc(MAX_PAYLOAD);
    if (!payload || !c->msg || do_handshake(c) < 0) {
        goto done;
    }

    for (;;) {
        frame_hdr_t h;
        int rc = recv_frame(c, &h, payload);
        if (rc <= 0) {
            break;
        }
        if (rc > 1) {
            send_close(c->fd, rc); /* protocol violation: tell the peer why, then leave */
            break;
        }
        if (h.opcode == OP_CLOSE) {
            send_frame(c->fd, OP_CLOSE, payload, h.len); /* echo the close, then leave */
            break;
        }
        if (h.opcode == OP_PING) {
            if (send_frame(c->fd, OP_PONG, payload, h.len) < 0) {
                break;
            }
            continue;
        }
        if (h.opcode == OP_PONG) {
            continue; /* unsolicited pong: ignore */
        }
        rc = on_data_frame(c, &h, payload);
        if (rc < 0) {
            break;
        }
        if (rc > 0) {
            send_close(c->fd, rc);
            break;
        }
    }

done:
    free(payload);
    free(c->msg);
    close(c->fd);
    free(c);
}

typedef struct {
    int lfd;
    int max_conns;
} listen_args_t;

/* Accepts connections and spawns one conn_fiber each. */
static void listen_fiber(void* arg) {
    listen_args_t a = *(listen_args_t*)arg;
    free(arg);

    int one = 1;
    for (int served = 0;;) {
        int c = cfiber_ev_accept(a.lfd, NULL, NULL);
        if (c < 0) {
            /* Transient: a peer that hung up before accept, or the process being
             * out of descriptors for a moment. Anything else ends the listener. */
            if (errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                (void)cfiber_ev_sleep((uint64_t)10 * 1000 * 1000);
                continue;
            }
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
    close(a.lfd);
}

int ws_serve(cfiber_reactor_t* r, int port, int max_conns) {
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t salen = sizeof sa;
    if (bind(lfd, (struct sockaddr*)&sa, salen) < 0 || listen(lfd, SOMAXCONN) < 0
        || getsockname(lfd, (struct sockaddr*)&sa, &salen) < 0) {
        const int saved = errno;
        close(lfd);
        errno = saved;
        return -1;
    }

    listen_args_t* a = malloc(sizeof *a);
    if (!a) {
        close(lfd);
        return -1;
    }
    *a = (listen_args_t){.lfd = lfd, .max_conns = max_conns};
    if (!cfiber_reactor_spawn(r, listen_fiber, a, NULL)) {
        free(a);
        close(lfd);
        return -1;
    }
    return ntohs(sa.sin_port);
}
