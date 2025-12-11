#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "udp.h"

#define MAX_NAME_LEN 64
#define MAX_MUTED    16

struct Node {
    char client_name[MAX_NAME_LEN];
    struct sockaddr_in addr;
    char muted_names[MAX_MUTED][MAX_NAME_LEN];
    int muted_count;
    struct Node *next;
};

struct Node* create_node(const char *client_name, struct sockaddr_in *addr)
{
    struct Node* new_node = malloc(sizeof(struct Node));
    if (!new_node) {
        perror("malloc");
        exit(1);
    }
    strncpy(new_node->client_name, client_name, MAX_NAME_LEN - 1);
    new_node->client_name[MAX_NAME_LEN - 1] = '\0';
    new_node->addr = *addr;
    new_node->muted_count = 0;
    new_node->next = NULL;
    return new_node;
}

void push_front(struct Node** head, const char *client_name, struct sockaddr_in *addr)
{
    struct Node* new_node = create_node(client_name, addr);
    new_node->next = *head;
    *head = new_node;
}

void append(struct Node** head, const char *client_name, struct sockaddr_in *addr)
{
    struct Node* new_node = create_node(client_name, addr);

    if (*head == NULL) {
        *head = new_node;
        return;
    }

    struct Node* temp = *head;
    while (temp->next != NULL) {
        temp = temp->next;
    }
    temp->next = new_node;
}



void handle_request(server_context_t *ctx, struct sockaddr_in *client_addr, char *client_request, int length);

struct Node *find_client_by_addr(server_context_t *ctx, struct sockaddr_in *addr)
{
    struct Node *cur;
    pthread_rwlock_rdlock(&ctx->clients_lock);
    for (cur = ctx->clients_head; cur != NULL; cur = cur->next) {
        if (cur->addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            cur->addr.sin_port        == addr->sin_port) {
            break;
        }
    }
    pthread_rwlock_unlock(&ctx->clients_lock);
    return cur;
}

struct Node *find_client_by_name(server_context_t *ctx, const char *name)
{
    struct Node *cur;
    pthread_rwlock_rdlock(&ctx->clients_lock);
    for (cur = ctx->clients_head; cur != NULL; cur = cur->next) {
        if (strcmp(cur->client_name, name) == 0) {
            break;
        }
    }
    pthread_rwlock_unlock(&ctx->clients_lock);
    return cur;
}

int is_muted(struct Node *receiver, const char *sender_name)
{
    for (int i = 0; i < receiver->muted_count; i++) {
        if (strcmp(receiver->muted_names[i], sender_name) == 0) {
            return 1;
        }
    }
    return 0;
}

void add_mute(struct Node *client, const char *name)
{
    if (client->muted_count >= MAX_MUTED) {
        return;
    }
    for (int i = 0; i < client->muted_count; i++) {
        if (strcmp(client->muted_names[i], name) == 0) {
            return;
        }
    }
    strncpy(client->muted_names[client->muted_count], name, MAX_NAME_LEN - 1);
    client->muted_names[client->muted_count][MAX_NAME_LEN - 1] = '\0';
    client->muted_count++;
}

void remove_mute(struct Node *client, const char *name)
{
    for (int i = 0; i < client->muted_count; i++) {
        if (strcmp(client->muted_names[i], name) == 0) {
            for (int j = i; j < client->muted_count - 1; j++) {
                strcpy(client->muted_names[j], client->muted_names[j + 1]);
            }
            client->muted_count--;
            return;
        }
    }
}

void send_to_client(server_context_t *ctx, struct Node *client, const char *msg)
{
    udp_socket_write(ctx->sd, &client->addr, (char *)msg, BUFFER_SIZE);
}

void broadcast_message(server_context_t *ctx,
                       struct Node *sender,
                       const char *msg)
{
    pthread_rwlock_rdlock(&ctx->clients_lock);
    struct Node *cur = ctx->clients_head;
    while (cur != NULL) {
        if (sender == NULL || !is_muted(cur, sender->client_name)) {
            udp_socket_write(ctx->sd, &cur->addr, (char *)msg, BUFFER_SIZE);
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&ctx->clients_lock);
}

void *listener_thread(void *arg)
{
    server_context_t *ctx = (server_context_t *)arg;

    char client_request[BUFFER_SIZE];
    struct sockaddr_in client_addr;

    while (ctx->running) {
        int rc = udp_socket_read(ctx->sd, &client_addr, client_request, BUFFER_SIZE);

        if (rc > 0) {
            if (rc < BUFFER_SIZE)
                client_request[rc] = '\0';
            else
                client_request[BUFFER_SIZE - 1] = '\0';

            printf("Received request: %s\n", client_request);

            handle_request(ctx, &client_addr, client_request, rc);
        } else if (rc < 0) {
            perror("udp_socket_read");
            break;
        }
    }

    printf("Listener thread exiting.\n");
    return NULL;
}

void parse_request(char *buffer, char **command, char **content) {
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }

    *command = strtok(buffer, "$");
    *content = strtok(NULL, "\0");

    if (*content == NULL) {
        *content = "";
    }

    while (**content == ' ')
        (*content)++;
}

void handle_conn(server_context_t *ctx,
                 struct sockaddr_in *client_addr,
                 const char *name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *existing = find_client_by_addr(ctx, client_addr);
    if (existing == NULL) {
        struct Node *new_node = create_node(name, client_addr);
        new_node->next = ctx->clients_head;
        ctx->clients_head = new_node;
        existing = new_node;
    } else {
        strncpy(existing->client_name, name, MAX_NAME_LEN - 1);
        existing->client_name[MAX_NAME_LEN - 1] = '\0';
    }
    pthread_rwlock_unlock(&ctx->clients_lock);

    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
             "Hi %s, you have successfully connected to the chat",
             existing->client_name);
    send_to_client(ctx, existing, response);
}

void handle_say(server_context_t *ctx,
                struct sockaddr_in *client_addr,
                const char *msg)
{
    struct Node *sender = find_client_by_addr(ctx, client_addr);
    const char *name = sender ? sender->client_name : "Unknown";

    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s: %s", name, msg);
    broadcast_message(ctx, sender, buffer);
}

void handle_sayto(server_context_t *ctx,
                  struct sockaddr_in *client_addr,
                  char *content)
{
    struct Node *sender = find_client_by_addr(ctx, client_addr);
    const char *sender_name = sender ? sender->client_name : "Unknown";

    char *space = strchr(content, ' ');
    if (!space) {
        return;
    }
    *space = '\0';
    char *recipient_name = content;
    char *msg = space + 1;

    struct Node *recipient = find_client_by_name(ctx, recipient_name);
    if (!recipient) {
        return;
    }

    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s: %s", sender_name, msg);
    send_to_client(ctx, recipient, buffer);
}

void handle_disconn(server_context_t *ctx,
                    struct sockaddr_in *client_addr)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *prev = NULL;
    struct Node *cur = ctx->clients_head;

    while (cur != NULL) {
        if (cur->addr.sin_addr.s_addr == client_addr->sin_addr.s_addr &&
            cur->addr.sin_port        == client_addr->sin_port) {

            if (prev == NULL)
                ctx->clients_head = cur->next;
            else
                prev->next = cur->next;

            char name[MAX_NAME_LEN];
            strncpy(name, cur->client_name, MAX_NAME_LEN);
            free(cur);
            pthread_rwlock_unlock(&ctx->clients_lock);

            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response),
                     "Disconnected. Bye! (%s)", name);
            udp_socket_write(ctx->sd, client_addr, response, BUFFER_SIZE);
            return;
        }
        prev = cur;
        cur = cur->next;
    }

    pthread_rwlock_unlock(&ctx->clients_lock);
}

void handle_rename(server_context_t *ctx,
                   struct sockaddr_in *client_addr,
                   const char *new_name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *client = find_client_by_addr(ctx, client_addr);
    if (client) {
        strncpy(client->client_name, new_name, MAX_NAME_LEN - 1);
        client->client_name[MAX_NAME_LEN - 1] = '\0';
    }
    pthread_rwlock_unlock(&ctx->clients_lock);

    if (client) {
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response),
                 "You are now known as %s", client->client_name);
        send_to_client(ctx, client, response);
    }
}

void handle_mute(server_context_t *ctx,
                 struct sockaddr_in *client_addr,
                 const char *name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *client = find_client_by_addr(ctx, client_addr);
    if (client) {
        add_mute(client, name);
    }
    pthread_rwlock_unlock(&ctx->clients_lock);
}

void handle_unmute(server_context_t *ctx,
                   struct sockaddr_in *client_addr,
                   const char *name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *client = find_client_by_addr(ctx, client_addr);
    if (client) {
        remove_mute(client, name);
    }
    pthread_rwlock_unlock(&ctx->clients_lock);
}

void handle_kick(server_context_t *ctx,
                 struct sockaddr_in *client_addr,
                 const char *name)
{
    if (client_addr->sin_port != htons(6666)) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "You are not authorized to kick users.");
        udp_socket_write(ctx->sd, client_addr, msg, BUFFER_SIZE);
        return;
    }

    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *prev = NULL;
    struct Node *cur = ctx->clients_head;

    while (cur != NULL) {
        if (strcmp(cur->client_name, name) == 0) {

            if (prev == NULL)
                ctx->clients_head = cur->next;
            else
                prev->next = cur->next;

            struct sockaddr_in kicked_addr = cur->addr;
            free(cur);
            pthread_rwlock_unlock(&ctx->clients_lock);

            char msg_kicked[BUFFER_SIZE];
            snprintf(msg_kicked, sizeof(msg_kicked),
                     "You have been removed from the chat");
            udp_socket_write(ctx->sd, &kicked_addr, msg_kicked, BUFFER_SIZE);

            char msg_bcast[BUFFER_SIZE];
            snprintf(msg_bcast, sizeof(msg_bcast),
                     "%s has been removed from the chat", name);
            broadcast_message(ctx, NULL, msg_bcast);
            return;
        }
        prev = cur;
        cur = cur->next;
    }

    pthread_rwlock_unlock(&ctx->clients_lock);
}

void handle_request(server_context_t *ctx, struct sockaddr_in *client_addr, char *client_request, int length)
{
    char *command = NULL;
    char *content = NULL;

    parse_request(client_request, &command, &content);

    if (command == NULL) {
        return;
    }

    if (strcmp(command, "conn") == 0) {
        handle_conn(ctx, client_addr, content);
    } else if (strcmp(command, "say") == 0) {
        handle_say(ctx, client_addr, content);
    } else if (strcmp(command, "sayto") == 0) {
        handle_sayto(ctx, client_addr, content);
    } else if (strcmp(command, "disconn") == 0) {
        handle_disconn(ctx, client_addr);
    } else if (strcmp(command, "rename") == 0) {
        handle_rename(ctx, client_addr, content);
    } else if (strcmp(command, "mute") == 0) {
        handle_mute(ctx, client_addr, content);
    } else if (strcmp(command, "unmute") == 0) {
        handle_unmute(ctx, client_addr, content);
    } else if (strcmp(command, "kick") == 0) {
        handle_kick(ctx, client_addr, content);
    }
    else
    {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "Invalid command: %s", command);
        udp_socket_write(ctx->sd, client_addr, msg, BUFFER_SIZE);
    }
}

int main(int argc, char *argv[])
{
    int sd = udp_socket_open(SERVER_PORT);
    assert(sd > -1);
    printf("Chat server running on port %d...\n", SERVER_PORT);


    server_context_t ctx;
    ctx.sd = sd;
    ctx.running = 1;
    ctx.clients_head = NULL;
    pthread_rwlock_init(&ctx.clients_lock, NULL);

    pthread_t listener_tid;
    int rc = pthread_create(&listener_tid, NULL, listener_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to create listener thread\n");
        return 1;
    }

    pthread_join(listener_tid, NULL);

    close(sd);
    pthread_rwlock_destroy(&ctx.clients_lock);

    return 0;
}
