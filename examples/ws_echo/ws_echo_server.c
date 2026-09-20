/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

/* Standalone WebSocket echo daemon on the cfiber reactor. Usage: ws_echo [port] */
#include "ws_echo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* 1..65535, or -1. atoi would accept "80abc" and turn "abc" into port 0. */
static int parse_port(const char* s) {
    char* end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < 1 || v > 65535) {
        return -1;
    }
    return (int)v;
}

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) {
        port = parse_port(argv[1]);
        if (port < 0) {
            fprintf(stderr, "usage: ws_echo [port 1-65535]\n");
            return 2;
        }
    }

    cfiber_reactor_t* r = cfiber_reactor_create((cfiber_reactor_config_t){
        .stack_size = (size_t)64 * 1024,
        .stack_cache = 256,
    });
    if (!r) {
        perror("cfiber_reactor_create");
        return 1;
    }

    int bound = ws_serve(r, port, /*max_conns=*/0);
    if (bound < 0) {
        perror("ws_serve");
        cfiber_reactor_destroy(r);
        return 1;
    }
    fprintf(stderr, "[ws] listening on 127.0.0.1:%d\n", bound);

    int rc = cfiber_reactor_run(r);
    if (rc < 0) {
        perror("cfiber_reactor_run");
    }
    cfiber_reactor_destroy(r);
    return rc < 0 ? 1 : 0;
}
