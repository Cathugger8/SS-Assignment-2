#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ncurses.h>
#include "udp.h"

#define CLIENT_PORT 0

//since this version of chat_client uses ncurses for ui, the way it is compiled is different
//Also, pthread.h is included. So both of these appear as flags in the compile command:
//gcc chat_client_with_ui.c -o chat_client_with_ui -lncurses -lpthread

typedef struct {
    int sd;
    struct sockaddr_in server_addr;
    volatile int running;

    // ncurses UI
    WINDOW *chat_win;
    WINDOW *input_win;
    pthread_mutex_t ui_lock;
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

            pthread_mutex_lock(&ctx->ui_lock);

            int maxy, maxx;
            getmaxyx(ctx->chat_win, maxy, maxx);
            // move to last line before border and print, with scrolling enabled
            wmove(ctx->chat_win, maxy - 2, 1);
            wclrtoeol(ctx->chat_win);
            wprintw(ctx->chat_win, "[SERVER] %s", server_response);
            wrefresh(ctx->chat_win);

            

            pthread_mutex_unlock(&ctx->ui_lock);
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
        pthread_mutex_lock(&ctx->ui_lock);

        // clear input line area, redraw box and prompt
        int iy, ix;
        getmaxyx(ctx->input_win, iy, ix);
        mvwprintw(ctx->input_win, 1, 1, "> ");
        wmove(ctx->input_win, 1, 3);  // after '> '
        wclrtoeol(ctx->input_win);
        wrefresh(ctx->input_win);
        
        pthread_mutex_unlock(&ctx->ui_lock);

        // Enable echo locally just for this window if you want
        echo();
        wgetnstr(ctx->input_win, client_request, BUFFER_SIZE - 1);
        noecho();

        


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

    initscr();              // start ncurses
    cbreak();               // disable line buffering
    noecho();               // don't echo typed chars automatically
    keypad(stdscr, TRUE);   // enable special keys
    raw();

    int rows, cols;
getmaxyx(stdscr, rows, cols);

int divider_y = rows / 2;

int chat_height = divider_y;

int input_height = rows - divider_y - 1;

mvprintw(1, 0, "Chat Messages");

move(divider_y, 0);
hline(ACS_HLINE, cols);
refresh();

WINDOW *chat_win  = newwin(chat_height, cols, 0, 0);
WINDOW *input_win = newwin(input_height, cols, divider_y + 1, 0);


    scrollok(chat_win, TRUE);           // allow scrolling in chat window


    mvwprintw(input_win, 1, 1, "> ");   // prompt
    wrefresh(chat_win);
    wrefresh(input_win);

    client_context_t ctx;
    ctx.sd = sd;
    ctx.server_addr = server_addr;
    ctx.running = 1;
    ctx.chat_win = chat_win;
    ctx.input_win = input_win;
    pthread_mutex_init(&ctx.ui_lock, NULL);

    pthread_t listener_tid, sender_tid;

    rc = pthread_create(&listener_tid, NULL, listener_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to create listener thread\n");
        endwin();
        return 1;
    }

    rc = pthread_create(&sender_tid, NULL, sender_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to create sender thread\n");
        ctx.running = 0;
        pthread_join(listener_tid, NULL);
        endwin();
        return 1;
    }

    pthread_join(sender_tid, NULL);
    ctx.running = 0;
    close(sd);
    pthread_join(listener_tid, NULL);

    // Clean up ncurses
    delwin(chat_win);
    delwin(input_win);
    endwin();
    pthread_mutex_destroy(&ctx.ui_lock);

    printf("Client exiting.\n");
    return 0;
}
