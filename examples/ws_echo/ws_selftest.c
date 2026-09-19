/*
 * In-process self-test for the WebSocket echo example: server and clients run as
 * fibers on one reactor over real loopback sockets. Verifies the handshake
 * crypto vector, echo correctness, fragment reassembly, a frame pipelined behind
 * the upgrade request, protocol-violation closes, and 300 concurrent connections
 * multiplexed by a single epoll loop. Ports are ephemeral, every client read is
 * bounded by a deadline, and a client that cannot finish counts as a failure
 * rather than a hang. Exit code is non-zero on any failure.
 */
#include "ws_echo.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IO_TIMEOUT_NS (5LL * 1000 * 1000 * 1000)

static int g_ok_count = 0;
static int g_client_failures = 0;
static int failures = 0;
static int g_port = 0; /* the current scenario's server port */

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  ok   - %s\n", msg);                                                                              \
        } else {                                                                                                       \
            printf("  FAIL - %s\n", msg);                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ---- Handshake crypto against the published RFC 6455 example vector ---- */

static void test_handshake_vector(void) {
    const char* key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t klen = strlen(key), mlen = strlen(magic);

    unsigned char concat[64], digest[20];
    /* NOLINTBEGIN(bugprone-not-null-terminated-result): binary concat, hashed */
    memcpy(concat, key, klen);
    memcpy(concat + klen, magic, mlen);
    /* NOLINTEND(bugprone-not-null-terminated-result) */
    ws_sha1(concat, klen + mlen, digest);

    char accept[64];
    CHECK(ws_base64(digest, 20, accept, sizeof accept) > 0, "base64 of the digest fits");
    CHECK(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "Sec-WebSocket-Accept matches RFC 6455 vector");
}

/* ---- Minimal client-side framing (mirror of the server's, masked) ---- */

/* Bounded: a lost or withheld frame fails the client instead of parking it forever. */
static int read_exact(int fd, void* buf, size_t n) {
    for (size_t off = 0; off < n;) {
        ssize_t r = cfiber_ev_read_timed(fd, (char*)buf + off, n - off, IO_TIMEOUT_NS);
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

/* Build a client frame header. fin/rsv/opcode as given, length up to 65535. */
static size_t build_header(unsigned char* hdr, int fin, int rsv, int opcode, int masked, size_t len) {
    size_t h = 0;
    hdr[h++] = (unsigned char)((fin ? 0x80 : 0) | (rsv & 0x70) | opcode);
    unsigned char m = masked ? 0x80 : 0;
    if (len < 126) {
        hdr[h++] = (unsigned char)(m | len);
    } else {
        hdr[h++] = (unsigned char)(m | 126);
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)len;
    }
    return h;
}

static const unsigned char MASK[4] = {0x12, 0x34, 0x56, 0x78};

/* Serialise a masked frame into out (cap >= len + 8). Returns its size. */
static size_t build_frame(unsigned char* out, int fin, int opcode, const unsigned char* p, size_t len) {
    size_t h = build_header(out, fin, 0, opcode, 1, len);
    memcpy(out + h, MASK, 4);
    h += 4;
    for (size_t i = 0; i < len; i++) {
        out[h + i] = p[i] ^ MASK[i & 3];
    }
    return h + len;
}

/* Client -> server frames MUST be masked (RFC 6455). Payloads here are small. */
static int client_send_frame(int fd, int fin, int opcode, const unsigned char* p, size_t len) {
    unsigned char frame[512];
    if (len + 8 > sizeof frame) {
        return -1;
    }
    size_t n = build_frame(frame, fin, opcode, p, len);
    return cfiber_ev_write(fd, frame, n) < 0 ? -1 : 0;
}

static int client_send(int fd, int opcode, const unsigned char* p, size_t len) {
    return client_send_frame(fd, 1, opcode, p, len);
}

/* Server -> client frames are never masked. Returns 0, or -1 on error / timeout. */
static int client_recv(int fd, int* opcode, unsigned char* p, size_t cap, size_t* len) {
    unsigned char b[2];
    if (read_exact(fd, b, 2) < 0) {
        return -1;
    }
    *opcode = b[0] & 0x0F;
    if (b[1] & 0x80) {
        return -1; /* the server must not mask */
    }
    uint64_t n = b[1] & 0x7F;
    if (n == 126) {
        unsigned char e[2];
        if (read_exact(fd, e, 2) < 0) {
            return -1;
        }
        n = ((uint64_t)e[0] << 8) | e[1];
    } else if (n == 127) {
        unsigned char e[8];
        if (read_exact(fd, e, 8) < 0) {
            return -1;
        }
        n = 0;
        for (int i = 0; i < 8; i++) {
            n = (n << 8) | e[i];
        }
    }
    if (n > cap) {
        return -1;
    }
    if (n && read_exact(fd, p, (size_t)n) < 0) {
        return -1;
    }
    *len = (size_t)n;
    return 0;
}

/* Reads a close frame and returns its status code, or -1. */
static int client_recv_close(int fd) {
    int op;
    size_t len;
    unsigned char body[125];
    if (client_recv(fd, &op, body, sizeof body, &len) < 0 || op != 0x8 || len < 2) {
        return -1;
    }
    return (body[0] << 8) | body[1];
}

/* Connect a non-blocking loopback socket to `port`. Returns the fd or -1. */
static int client_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (cfiber_ev_connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int upgrade_request(char* req, size_t cap, const char* path) {
    return snprintf(req,
                    cap,
                    "GET %s HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n\r\n",
                    path);
}

/* Read the 101 response headers into `resp`, one byte at a time so nothing
 * that follows them (an echoed frame) is consumed by mistake. */
static int read_upgrade_response(int fd, char* resp, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t r = cfiber_ev_read_timed(fd, resp + total, 1, IO_TIMEOUT_NS);
        if (r <= 0) {
            return -1;
        }
        total += (size_t)r;
        resp[total] = '\0';
        if (total >= 4 && memcmp(resp + total - 4, "\r\n\r\n", 4) == 0) {
            return 0;
        }
    }
    return -1;
}

/* Send the upgrade request and read the response headers into `resp`. */
static int client_upgrade(int fd, const char* path, char* resp, size_t cap) {
    char req[256];
    int n = upgrade_request(req, sizeof req, path);
    if (n < 0 || cfiber_ev_write(fd, req, (size_t)n) < 0) {
        return -1;
    }
    return read_upgrade_response(fd, resp, cap);
}

/* Connect and complete the handshake; -1 on any failure. */
static int connect_and_upgrade(int port) {
    int fd = client_connect(port);
    if (fd < 0) {
        return -1;
    }
    char resp[2048];
    if (client_upgrade(fd, "/", resp, sizeof resp) < 0 || !strstr(resp, " 101 ")) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- Scenario 1: one connection, the assertion suite ---- */

static void client_fiber(void* arg) {
    (void)arg;
    int fd = client_connect(g_port);
    CHECK(fd >= 0, "client connected (cfiber_ev_connect parked until writable, then resumed)");
    if (fd < 0) {
        return;
    }

    char resp[2048];
    CHECK(client_upgrade(fd, "/chat", resp, sizeof resp) == 0 && strstr(resp, " 101 "),
          "server returned 101 Switching Protocols");
    CHECK(strstr(resp, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
          "server computed the correct accept key live");

    int op;
    size_t rlen;
    unsigned char buf[512];

    /* Text echo round-trips of varying small sizes. */
    const char* msgs[] = {"Hello, fibers!", "x", "", "ping-me"};
    for (size_t m = 0; m < sizeof(msgs) / sizeof(msgs[0]); m++) {
        size_t len = strlen(msgs[m]);
        int rc = client_send(fd, 0x1, (const unsigned char*)msgs[m], len);
        char label[64];
        snprintf(label, sizeof label, "echo #%zu matches (\"%s\")", m, msgs[m]);
        CHECK(rc == 0 && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0 && op == 0x1 && rlen == len
                  && memcmp(buf, msgs[m], len) == 0,
              label);
    }

    /* A 200-byte payload exercises the extended (16-bit) length path. */
    unsigned char big[200];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (unsigned char)((i * 7) + 1);
    }
    CHECK(client_send(fd, 0x2, big, sizeof big) == 0 && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0 && op == 0x2
              && rlen == sizeof big && memcmp(buf, big, sizeof big) == 0,
          "200-byte binary echo matches (16-bit length path)");

    /* ping -> pong. */
    CHECK(client_send(fd, 0x9, (const unsigned char*)"hi", 2) == 0 && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0
              && op == 0xA && rlen == 2 && memcmp(buf, "hi", 2) == 0,
          "ping answered with matching pong");

    /* Fragmented text: TEXT(FIN=0) + CONT(FIN=0) + CONT(FIN=1) comes back as one message. */
    CHECK(client_send_frame(fd, 0, 0x1, (const unsigned char*)"Hel", 3) == 0
              && client_send_frame(fd, 0, 0x0, (const unsigned char*)"lo, ", 4) == 0
              && client_send_frame(fd, 1, 0x0, (const unsigned char*)"world", 5) == 0
              && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0 && op == 0x1 && rlen == 12
              && memcmp(buf, "Hello, world", 12) == 0,
          "fragmented text reassembled into one TEXT message");

    /* Fragmented binary keeps its opcode, with a ping interleaved between fragments. */
    unsigned char bin[3] = {0x00, 0xFF, 0x7F};
    CHECK(client_send_frame(fd, 0, 0x2, bin, 2) == 0 && client_send(fd, 0x9, NULL, 0) == 0
              && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0 && op == 0xA && rlen == 0
              && client_send_frame(fd, 1, 0x0, bin + 2, 1) == 0 && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0
              && op == 0x2 && rlen == 3 && memcmp(buf, bin, 3) == 0,
          "fragmented binary reassembled as BINARY, control frame interleaved");

    client_send(fd, 0x8, NULL, 0); /* close */
    CHECK(client_recv(fd, &op, buf, sizeof buf, &rlen) == 0 && op == 0x8, "server echoed the close");
    close(fd);
}

/* ---- Scenario 2: a frame pipelined behind the upgrade request ---- */

static void pipelined_client(void* arg) {
    (void)arg;
    int fd = client_connect(g_port);
    if (fd < 0) {
        CHECK(0, "pipelined client connected");
        return;
    }

    /* request + first frame in a single write: the server must not drop the frame */
    unsigned char wire[512];
    int n = upgrade_request((char*)wire, sizeof wire, "/");
    size_t total = (size_t)n + build_frame(wire + n, 1, 0x1, (const unsigned char*)"early", 5);
    char resp[2048];
    int op;
    size_t rlen;
    unsigned char buf[64];
    CHECK(n > 0 && cfiber_ev_write(fd, wire, total) == (ssize_t)total
              && read_upgrade_response(fd, resp, sizeof resp) == 0 && strstr(resp, " 101 ")
              && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0 && op == 0x1 && rlen == 5
              && memcmp(buf, "early", 5) == 0,
          "frame pipelined behind the upgrade request is echoed");
    client_send(fd, 0x8, NULL, 0);
    close(fd);
}

/* ---- Scenario 3: protocol violations are answered with a close frame ---- */

typedef struct {
    const char* what;
    unsigned char frame[160];
    size_t len;
    int expect_code;
} violation_t;

static void violation_client(void* arg) {
    violation_t* v = arg;
    int fd = connect_and_upgrade(g_port);
    if (fd < 0) {
        CHECK(0, v->what);
        return;
    }
    int code = -1;
    if (cfiber_ev_write(fd, v->frame, v->len) == (ssize_t)v->len) {
        code = client_recv_close(fd);
    }
    char label[96];
    snprintf(label, sizeof label, "%s: closed with %d", v->what, v->expect_code);
    CHECK(code == v->expect_code, label);
    close(fd);
}

/* ---- Scenario 4: many concurrent connections ---- */

static void concurrency_client(void* arg) {
    long id = (long)(intptr_t)arg;

    int fd = connect_and_upgrade(g_port);
    if (fd < 0) {
        g_client_failures++;
        return;
    }

    char msg[32];
    int mlen = snprintf(msg, sizeof msg, "client-%ld", id);
    int op;
    size_t rlen;
    unsigned char buf[256];
    if (client_send(fd, 0x1, (const unsigned char*)msg, (size_t)mlen) == 0
        && client_recv(fd, &op, buf, sizeof buf, &rlen) == 0 && rlen == (size_t)mlen
        && memcmp(buf, msg, (size_t)mlen) == 0) {
        g_ok_count++;
    } else {
        g_client_failures++;
    }

    client_send(fd, 0x8, NULL, 0);
    close(fd);
}

/* Single-threaded test program: exit() from a helper is fine here. */
// NOLINTBEGIN(concurrency-mt-unsafe)
static cfiber_reactor_t* make_reactor(size_t stack) {
    cfiber_reactor_t* r = cfiber_reactor_create((cfiber_reactor_config_t){.stack_size = stack, .stack_cache = 64});
    if (!r) {
        perror("cfiber_reactor_create");
        exit(1);
    }
    return r;
}

/* Serve max_conns connections on an ephemeral port; exits the process on failure. */
static int serve_or_die(cfiber_reactor_t* r, int max_conns) {
    int port = ws_serve(r, 0, max_conns);
    if (port < 0) {
        perror("ws_serve");
        exit(1);
    }
    return port;
}
// NOLINTEND(concurrency-mt-unsafe)

int main(void) {
    printf("handshake crypto:\n");
    test_handshake_vector();

    printf("live echo over one event loop (server + client fibers, loopback):\n");
    {
        cfiber_reactor_t* r = make_reactor((size_t)128 * 1024);
        g_port = serve_or_die(r, /*max_conns=*/2);
        CHECK(cfiber_reactor_spawn(r, client_fiber, NULL, NULL), "spawned the echo client");
        CHECK(cfiber_reactor_spawn(r, pipelined_client, NULL, NULL), "spawned the pipelining client");
        CHECK(cfiber_reactor_run(r) == 0, "run completed");
        cfiber_reactor_destroy(r);
    }

    printf("protocol violations:\n");
    {
        static violation_t cases[5];
        size_t n = 0;
        /* unmasked client frame */
        cases[n].what = "unmasked frame";
        cases[n].expect_code = 1002;
        cases[n].len = build_header(cases[n].frame, 1, 0, 0x1, 0, 2);
        memcpy(cases[n].frame + cases[n].len, "hi", 2);
        cases[n].len += 2;
        n++;
        /* RSV bit set */
        cases[n].what = "RSV bit set";
        cases[n].expect_code = 1002;
        cases[n].len = build_header(cases[n].frame, 1, 0x40, 0x1, 1, 0);
        memcpy(cases[n].frame + cases[n].len, MASK, 4);
        cases[n].len += 4;
        n++;
        /* reserved opcode */
        cases[n].what = "reserved opcode 0x3";
        cases[n].expect_code = 1002;
        cases[n].len = build_header(cases[n].frame, 1, 0, 0x3, 1, 0);
        memcpy(cases[n].frame + cases[n].len, MASK, 4);
        cases[n].len += 4;
        n++;
        /* oversized control frame: a 126-byte ping */
        static unsigned char ping[126];
        cases[n].what = "126-byte ping";
        cases[n].expect_code = 1002;
        cases[n].len = build_frame(cases[n].frame, 1, 0x9, ping, sizeof ping);
        n++;
        /* continuation with no message open */
        cases[n].what = "continuation without a message";
        cases[n].expect_code = 1002;
        cases[n].len = build_frame(cases[n].frame, 1, 0x0, (const unsigned char*)"x", 1);
        n++;

        cfiber_reactor_t* r = make_reactor((size_t)128 * 1024);
        g_port = serve_or_die(r, (int)n);
        for (size_t i = 0; i < n; i++) {
            CHECK(cfiber_reactor_spawn(r, violation_client, &cases[i], NULL), "spawned a violation client");
        }
        CHECK(cfiber_reactor_run(r) == 0, "run completed");
        cfiber_reactor_destroy(r);
    }

    printf("concurrency: 300 client fibers, one thread, one epoll loop:\n");
    {
        enum { N = 300 };
        cfiber_reactor_t* r = make_reactor((size_t)64 * 1024);
        g_port = serve_or_die(r, N);
        int spawned = 0;
        for (long i = 0; i < N; i++) {
            spawned += cfiber_reactor_spawn(r, concurrency_client, (void*)(intptr_t)i, NULL) ? 1 : 0;
        }
        CHECK(spawned == N, "spawned every client");
        CHECK(cfiber_reactor_run(r) == 0, "run completed");
        cfiber_reactor_destroy(r);

        char label[96];
        snprintf(label,
                 sizeof label,
                 "all %d echoes correct (got %d, %d client failures)",
                 N,
                 g_ok_count,
                 g_client_failures);
        CHECK(g_ok_count == N && g_client_failures == 0, label);
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
