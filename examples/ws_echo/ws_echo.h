/*
 * ws_echo.h - a minimal WebSocket (RFC 6455) echo server on the cfiber reactor.
 *
 * Example, not library API. It shows a real protocol (handshake + frame
 * parsing) driven entirely by the reactor's cfiber_ev_* transport helpers: one
 * fiber per connection, all multiplexed on a single epoll loop.
 */
#ifndef WS_ECHO_H
#define WS_ECHO_H

#include "cfiber/reactor/reactor.h"

#include <stddef.h>

/*
 * Spawn a listener fiber on `r`, bound to 127.0.0.1:`port`. It accepts
 * connections and spawns one echo fiber per connection.
 *
 * If max_conns > 0 the listener exits after that many connections (the self-test
 * uses this for a clean shutdown); 0 serves forever. Returns 0 on success.
 */
int ws_serve(cfiber_reactor_t* r, int port, int max_conns);

/* Handshake crypto, exposed so the self-test can drive the client side. */
void ws_sha1(const unsigned char* data, size_t len, unsigned char out[20]);
int ws_base64(const unsigned char* in, size_t len, char* out, size_t out_cap);

#endif /* WS_ECHO_H */
