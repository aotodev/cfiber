/*
 * In-process self-test for the WebSocket echo example: server and clients run as
 * fibers on one reactor over real loopback sockets. Verifies the handshake
 * crypto vector, echo correctness, and 300 concurrent connections multiplexed by
 * a single epoll loop. Exit code is non-zero on any failure.
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

#define PORT 48080
#define PORT2 48081

static int g_ok_count = 0;
static int failures = 0;

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
    memcpy(concat, key, klen);
    memcpy(concat + klen, magic, mlen);
    ws_sha1(concat, klen + mlen, digest);

    char accept[64];
    ws_base64(digest, 20, accept, sizeof accept);
    CHECK(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "Sec-WebSocket-Accept matches RFC 6455 vector");
}

/* ---- Minimal client-side framing (mirror of the server's, masked) ---- */

static int read_exact(int fd, void* buf, size_t n) {
    for (size_t off = 0; off < n;) {
        ssize_t r = cfiber_ev_read(fd, (char*)buf + off, n - off);
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

/* Client -> server frames MUST be masked (RFC 6455). Payloads here are small. */
static int client_send(int fd, int opcode, const unsigned char* p, size_t len) {
    unsigned char hdr[8];
    size_t h = 0;
    hdr[h++] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        hdr[h++] = (unsigned char)(0x80 | len);
    } else if (len <= 0xFFFF) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)len;
    } else {
        return -1;
    }

    const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(hdr + h, mask, 4);
    h += 4;
    if (cfiber_ev_write(fd, hdr, h) < 0) {
        return -1;
    }

    unsigned char masked[256];
    if (len > sizeof masked) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        masked[i] = p[i] ^ mask[i & 3];
    }
    return (len && cfiber_ev_write(fd, masked, len) < 0) ? -1 : 0;
}

/* Server -> client frames are never masked. */
static int client_recv(int fd, int* opcode, unsigned char* p, size_t* len) {
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
        n = (uint64_t)e[0] << 8 | e[1];
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
    if (n && read_exact(fd, p, (size_t)n) < 0) {
        return -1;
    }
    *len = (size_t)n;
    return 0;
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

/* Send the upgrade request and read the 101 response headers into `resp`. */
static int client_upgrade(int fd, const char* path, char* resp, size_t cap) {
    char req[256];
    int n = snprintf(req,
                     sizeof req,
                     "GET %s HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     path);
    if (cfiber_ev_write(fd, req, (size_t)n) < 0) {
        return -1;
    }
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t r = cfiber_ev_read(fd, resp + total, cap - 1 - total);
        if (r <= 0) {
            return -1;
        }
        total += (size_t)r;
        resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n")) {
            return 0;
        }
    }
    return -1;
}

/* Drives the full assertion suite against the server over one connection. */
static void client_fiber(void* arg) {
    (void)arg;
    int fd = client_connect(PORT);
    CHECK(fd >= 0, "client connected (cfiber_ev_connect parked until writable, then resumed)");
    if (fd < 0) {
        return;
    }

    char resp[2048];
    CHECK(client_upgrade(fd, "/chat", resp, sizeof resp) == 0 && strstr(resp, "101"),
          "server returned 101 Switching Protocols");
    CHECK(strstr(resp, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
          "server computed the correct accept key live");

    /* Text echo round-trips of varying small sizes. */
    const char* msgs[] = {"Hello, fibers!", "x", "", "ping-me"};
    for (size_t m = 0; m < sizeof(msgs) / sizeof(msgs[0]); m++) {
        size_t len = strlen(msgs[m]);
        client_send(fd, 0x1, (const unsigned char*)msgs[m], len);

        int op;
        size_t rlen;
        unsigned char buf[256];
        if (client_recv(fd, &op, buf, &rlen) < 0) {
            CHECK(0, "recv echo");
            break;
        }
        char label[64];
        snprintf(label, sizeof label, "echo #%zu matches (\"%s\")", m, msgs[m]);
        CHECK(op == 0x1 && rlen == len && memcmp(buf, msgs[m], len) == 0, label);
    }

    /* A 200-byte payload exercises the extended (16-bit) length path. */
    unsigned char big[200];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (unsigned char)(i * 7 + 1);
    }
    client_send(fd, 0x2, big, sizeof big);
    int op;
    size_t rlen;
    unsigned char buf[256];
    client_recv(fd, &op, buf, &rlen);
    CHECK(op == 0x2 && rlen == sizeof big && memcmp(buf, big, sizeof big) == 0,
          "200-byte binary echo matches (16-bit length path)");

    /* ping -> pong. */
    client_send(fd, 0x9, (const unsigned char*)"hi", 2);
    client_recv(fd, &op, buf, &rlen);
    CHECK(op == 0xA && rlen == 2 && memcmp(buf, "hi", 2) == 0, "ping answered with matching pong");

    client_send(fd, 0x8, NULL, 0); /* close */
    close(fd);
}

/*
 * One handshake + one echo round-trip, then bump the shared counter. Many of
 * these run concurrently, all multiplexed by a single epoll loop.
 */
static void concurrency_client(void* arg) {
    long id = (long)(intptr_t)arg;

    int fd = client_connect(PORT2);
    if (fd < 0) {
        return;
    }
    char resp[2048];
    if (client_upgrade(fd, "/", resp, sizeof resp) < 0) {
        close(fd);
        return;
    }

    char msg[32];
    int mlen = snprintf(msg, sizeof msg, "client-%ld", id);
    client_send(fd, 0x1, (const unsigned char*)msg, (size_t)mlen);

    int op;
    size_t rlen;
    unsigned char buf[256];
    if (client_recv(fd, &op, buf, &rlen) == 0 && rlen == (size_t)mlen && memcmp(buf, msg, (size_t)mlen) == 0) {
        g_ok_count++;
    }

    client_send(fd, 0x8, NULL, 0);
    close(fd);
}

int main(void) {
    printf("handshake crypto:\n");
    test_handshake_vector();

    printf("live echo over one event loop (server + client fibers, loopback):\n");
    cfiber_reactor_t* s = cfiber_reactor_create((cfiber_reactor_config_t){.stack_size = 128 * 1024, .stack_cache = 64});
    if (!s) {
        perror("cfiber_reactor_create");
        return 1;
    }
    if (ws_serve(s, PORT, /*max_conns=*/1) < 0) {
        fprintf(stderr, "ws_serve failed\n");
        cfiber_reactor_destroy(s);
        return 1;
    }
    cfiber_reactor_spawn(s, client_fiber, NULL, NULL);
    cfiber_reactor_run(s);
    cfiber_reactor_destroy(s);

    printf("concurrency: 300 client fibers, one thread, one epoll loop:\n");
    enum { N = 300 };
    cfiber_reactor_t* cs = cfiber_reactor_create((cfiber_reactor_config_t){.stack_size = 64 * 1024, .stack_cache = 64});
    ws_serve(cs, PORT2, N);
    for (long i = 0; i < N; i++) {
        cfiber_reactor_spawn(cs, concurrency_client, (void*)(intptr_t)i, NULL);
    }
    cfiber_reactor_run(cs);
    cfiber_reactor_destroy(cs);

    char label[64];
    snprintf(label, sizeof label, "all %d echoes correct (got %d)", N, g_ok_count);
    CHECK(g_ok_count == N, label);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
