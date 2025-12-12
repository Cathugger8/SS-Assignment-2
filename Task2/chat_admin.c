#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ncursesw/ncurses.h>
#include <ctype.h>
#include "udp.h"

#define CLIENT_PORT 6666

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

            wprintw(ctx->chat_win, "%s\n", server_response);
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

    while (ctx->running) {

        char client_request[BUFFER_SIZE];
        int len = 0;
        int pos = 0;
        client_request[0] = '\0';

        pthread_mutex_lock(&ctx->ui_lock);
        wmove(ctx->input_win, 1, 1);
        wclrtoeol(ctx->input_win);
        mvwprintw(ctx->input_win, 1, 1, "> ");
        wrefresh(ctx->input_win);
        pthread_mutex_unlock(&ctx->ui_lock);

        void redraw(void) {
            pthread_mutex_lock(&ctx->ui_lock);
            wmove(ctx->input_win, 1, 1);
            wclrtoeol(ctx->input_win);
            mvwprintw(ctx->input_win, 1, 1, "> %s", client_request);
            wmove(ctx->input_win, 1, 3 + pos);
            wrefresh(ctx->input_win);
            pthread_mutex_unlock(&ctx->ui_lock);
        }

        redraw();

        while (ctx->running) {
            int ch = wgetch(ctx->input_win);

            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                client_request[len] = '\0';
                break;
            }
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (pos > 0) {
                    memmove(&client_request[pos - 1], &client_request[pos], (size_t)(len - pos + 1));
                    pos--;
                    len--;
                }
            }
            else if (ch == KEY_LEFT) {
                if (pos > 0) pos--;
            }
            else if (ch == KEY_RIGHT) {
                if (pos < len) pos++;
            }
            else if (ch == KEY_UP || ch == KEY_DOWN) {
                continue;
            }
            else if (isprint(ch) && len < BUFFER_SIZE - 1) {
                memmove(&client_request[pos + 1], &client_request[pos], (size_t)(len - pos + 1));
                client_request[pos] = (char)ch;
                pos++;
                len++;
            }

            redraw();
        }

        if (len == 0)
            continue;

        client_request[len] = '\0';

        int rc = udp_socket_write(ctx->sd, &ctx->server_addr, client_request, (int)strlen(client_request));
        if (rc <= 0) {
            perror("udp_socket_write");
            ctx->running = 0;
            break;
        }

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
    // raw();

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

    keypad(chat_win, TRUE);
    keypad(input_win, TRUE);

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
