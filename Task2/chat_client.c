#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "udp.h"

#define CLIENT_PORT 0

typedef struct {
    int sd;
    struct sockaddr_in server_addr;
    volatile int running;
} client_context_t;

void *listener_thread(void *arg)
{
    client_context_t *ctx = (client_context_t *)arg;
    char server_response[BUFFER_SIZE];
    struct sockaddr_in responder_addr;

    while (ctx->running) {
        int rc = udp_socket_read(ctx->sd, &responder_addr,server_response, BUFFER_SIZE);
                                 
        if (rc > 0) {
            if (rc < BUFFER_SIZE) server_response[rc] = '\0';

            else server_response[BUFFER_SIZE - 1] = '\0';

            printf("\n[SERVER] %s\n", server_response);
            printf("> ");
            fflush(stdout);
        } else if (rc <= 0) {
            break;
        }
    }

    return NULL;
}

void *sender_thread(void *arg)
{
    client_context_t *ctx = (client_context_t *)arg;
    char client_request[BUFFER_SIZE];

    while (ctx->running) {
        printf("> ");
        fflush(stdout);

        if (fgets(client_request, sizeof(client_request), stdin) == NULL) {
            // EOF (Ctrl+D) or error: stop
            ctx->running = 0;
            break;
        }

        size_t len = strlen(client_request);
        if (len > 0 && client_request[len - 1] == '\n') {
            client_request[len - 1] = '\0';
            len--;
        }

        if (len == 0) {
            continue;
        }

        int rc = udp_socket_write(ctx->sd, &ctx->server_addr, client_request, BUFFER_SIZE);
        if (rc <= 0) {
            perror("udp_socket_write");
            ctx->running = 0;
            break;
        }

        // If user typed disconn$, stop client locally too
        if (strncmp(client_request, "disconn$", 8) == 0) {
            ctx->running = 0;
            break;
        }
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    int sd = udp_socket_open(CLIENT_PORT);
    if (sd < 0) {
        perror("udp_socket_open");
        return 1;
    }

    struct sockaddr_in server_addr;
    int rc = set_socket_addr(&server_addr, "127.0.0.1", SERVER_PORT);
    if (rc < 0) {
        fprintf(stderr, "Failed to set server address\n");
        return 1;
    }

    client_context_t ctx;
    ctx.sd = sd;
    ctx.server_addr = server_addr;
    ctx.running = 1;

    pthread_t listener_tid, sender_tid;

    rc = pthread_create(&listener_tid, NULL, listener_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to create listener thread\n");
        return 1;
    }

    rc = pthread_create(&sender_tid, NULL, sender_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to create sender thread\n");
        ctx.running = 0;
        pthread_join(listener_tid, NULL);
        return 1;
    }

    pthread_join(sender_tid, NULL);

    ctx.running = 0;
    close(sd);
    pthread_join(listener_tid, NULL);

    printf("Client exiting.\n");
    return 0;
}
