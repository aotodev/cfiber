/* Standalone WebSocket echo daemon on the cfiber reactor. Usage: ws_echo [port] */
#include "ws_echo.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 8080;

    cfiber_reactor_t* r = cfiber_reactor_create((cfiber_reactor_config_t){
        .stack_size = 64 * 1024,
        .stack_cache = 256,
    });
    if (!r) {
        perror("cfiber_reactor_create");
        return 1;
    }

    if (ws_serve(r, port, /*max_conns=*/0) < 0) {
        fprintf(stderr, "ws_serve failed\n");
        cfiber_reactor_destroy(r);
        return 1;
    }

    cfiber_reactor_run(r);
    cfiber_reactor_destroy(r);
    return 0;
}
