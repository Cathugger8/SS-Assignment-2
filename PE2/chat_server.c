#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include "udp.h"

#define MAX_NAME_LEN 64
#define MAX_MUTED 16
#define INACTIVE_THRESHOLD 120
#define PING_TIMEOUT 10

struct Node {
    char client_name[MAX_NAME_LEN];
    struct sockaddr_in addr;
    char muted_names[MAX_MUTED][MAX_NAME_LEN];
    int muted_count;
    struct Node *next;
    time_t last_active;
    int heap_index;
    int awaiting_ping_reply;
    time_t ping_sent_time;
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
    new_node->last_active = time(NULL);
    new_node->heap_index = -1;
    new_node->awaiting_ping_reply = 0;
    new_node->ping_sent_time = 0;
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

struct Node *find_client_by_addr_nolock(server_context_t *ctx, struct sockaddr_in *addr)
{
    struct Node *cur;
    for (cur = ctx->clients_head; cur != NULL; cur = cur->next) {
        if (cur->addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            cur->addr.sin_port        == addr->sin_port) {
            break;
        }
    }
    return cur;
}

struct Node *find_client_by_addr(server_context_t *ctx, struct sockaddr_in *addr)
{
    struct Node *cur;
    pthread_rwlock_rdlock(&ctx->clients_lock);
    cur = find_client_by_addr_nolock(ctx, addr);
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

// broadcast a message
void broadcast_message(server_context_t *ctx, struct Node *sender, const char *msg)
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
            if (rc < BUFFER_SIZE) client_request[rc] = '\0';

            else client_request[BUFFER_SIZE - 1] = '\0';

            printf("Received request: %s\n", client_request);

            handle_request(ctx, &client_addr, client_request, rc);
        } 
        else if (rc < 0) {
            perror("udp_socket_read");
            break;
        }
    }

    printf("Listener thread exiting.\n");
    return NULL;
}

// tokenise requests
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

    while (**content == ' ') (*content)++;
}

// min heap functions (assumes you hold client_lock)
static void heap_swap(server_context_t *ctx, int i, int j)
{
    struct Node *a = ctx->activity_heap[i];
    struct Node *b = ctx->activity_heap[j];
    ctx->activity_heap[i] = b;
    ctx->activity_heap[j] = a;
    if (a) a->heap_index = j;
    if (b) b->heap_index = i;
}

static void heap_sift_up(server_context_t *ctx, int idx)
{
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (ctx->activity_heap[parent]->last_active <= ctx->activity_heap[idx]->last_active) break;
        heap_swap(ctx, parent, idx);
        idx = parent;
    }
}

static void heap_sift_down(server_context_t *ctx, int idx)
{
    int n = ctx->heap_size;
    while (1) {
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;
        int smallest = idx;

        if (left < n && ctx->activity_heap[left]->last_active < ctx->activity_heap[smallest]->last_active) {
            smallest = left;
        }
        if (right < n && ctx->activity_heap[right]->last_active < ctx->activity_heap[smallest]->last_active) {
            smallest = right;
        }
        if (smallest == idx) break;
        heap_swap(ctx, idx, smallest);
        idx = smallest;
    }
}

static void heap_insert(server_context_t *ctx, struct Node *node)
{
    if (ctx->heap_size >= MAX_CLIENTS) return;
    int idx = ctx->heap_size++;
    ctx->activity_heap[idx] = node;
    node->heap_index = idx;
    heap_sift_up(ctx, idx);
}

static void heap_remove(server_context_t *ctx, struct Node *node)
{
    int idx = node->heap_index;
    if (idx < 0 || idx >= ctx->heap_size) return;

    int last = ctx->heap_size - 1;
    if (idx != last) {
        heap_swap(ctx, idx, last);
    }
    ctx->heap_size--;
    ctx->activity_heap[ctx->heap_size] = NULL;
    node->heap_index = -1;

    if (idx < ctx->heap_size) {
        heap_sift_down(ctx, idx);
        heap_sift_up(ctx, idx);
    }
}

static void heap_update(server_context_t *ctx, struct Node *node)
{
    int idx = node->heap_index;
    if (idx < 0 || idx >= ctx->heap_size) return;
    heap_sift_down(ctx, idx);
    heap_sift_up(ctx, idx);
}

void *ping_monitor_thread(void *arg)
{
    server_context_t *ctx = (server_context_t *)arg;

    while (ctx->running) {
        pthread_rwlock_wrlock(&ctx->clients_lock);

        if (ctx->heap_size > 0) {
            struct Node *least = ctx->activity_heap[0];
            time_t now = time(NULL);

            if (!least->awaiting_ping_reply) {

                if (now - least->last_active >= INACTIVE_THRESHOLD) {
                    const char *ping_msg = "ping$";
                    udp_socket_write(ctx->sd, &least->addr, (char *)ping_msg, strlen(ping_msg));
                    least->awaiting_ping_reply = 1;
                    least->ping_sent_time = now;
                }
            }
            else {
                if (now - least->ping_sent_time >= PING_TIMEOUT) {

                    struct Node *prev = NULL;
                    struct Node *cur = ctx->clients_head;
                    char removed_name[MAX_NAME_LEN];
                    int removed = 0;

                    while (cur != NULL && cur != least) {
                        prev = cur;
                        cur = cur->next;
                    }

                    if (cur == least) {
                        strncpy(removed_name, cur->client_name, MAX_NAME_LEN - 1);
                        removed_name[MAX_NAME_LEN - 1] = '\0';

                        if (prev == NULL) ctx->clients_head = cur->next;
                        else prev->next = cur->next;

                        heap_remove(ctx, cur);
                        free(cur);

                        removed = 1;
                    }

                    pthread_rwlock_unlock(&ctx->clients_lock);

                    if (removed) {
                        char msg_bcast[BUFFER_SIZE];
                        snprintf(msg_bcast, sizeof(msg_bcast), "%s has been removed due to inactivity", removed_name); 
                        broadcast_message(ctx, NULL, msg_bcast);
                    }

                    continue; // (to avoid double unlocking)
                }
            }
        }

        pthread_rwlock_unlock(&ctx->clients_lock);

        sleep(1);
    }

    return NULL;
}

// connect client and also output last 15 global messages
void handle_conn(server_context_t *ctx, struct sockaddr_in *client_addr, const char *name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *existing = find_client_by_addr_nolock(ctx, client_addr);
    if (existing == NULL) {
        struct Node *new_node = create_node(name, client_addr);
        new_node->next = ctx->clients_head;
        ctx->clients_head = new_node;
        existing = new_node;
        existing->last_active = time(NULL);
        heap_insert(ctx, existing);
    } 
    else {
        strncpy(existing->client_name, name, MAX_NAME_LEN - 1);
        existing->client_name[MAX_NAME_LEN - 1] = '\0';
        existing->last_active = time(NULL);
        if (existing->heap_index >= 0) {
            heap_update(ctx, existing);
        } 
        else {
            heap_insert(ctx, existing);
        }
    }
    pthread_rwlock_unlock(&ctx->clients_lock);

    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response), "Hi %s, you have successfully connected to the chat", existing->client_name);
    send_to_client(ctx, existing, response);

    for (int i = 0; i < ctx->global_count; i++) {
        int idx = (ctx->global_start + i) % GLOBAL_BUFFER_SIZE;
        send_to_client(ctx, existing, ctx->global_buffer[idx]);
    }
}

// send a message to all clients and store message in global buffer
void handle_say(server_context_t *ctx, struct sockaddr_in *client_addr, const char *msg)
{
    struct Node *sender = find_client_by_addr(ctx, client_addr);
    const char *name = sender ? sender->client_name : "Unknown";

    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s: %s", name, msg);

    if (ctx->global_count < GLOBAL_BUFFER_SIZE) {
        int idx = (ctx->global_start + ctx->global_count) % GLOBAL_BUFFER_SIZE;
        strncpy(ctx->global_buffer[idx], buffer, BUFFER_SIZE - 1);
        ctx->global_buffer[idx][BUFFER_SIZE - 1] = '\0';
        ctx->global_count++;
    } 
    else {
        int idx = ctx->global_start;
        strncpy(ctx->global_buffer[idx], buffer, BUFFER_SIZE - 1);
        ctx->global_buffer[idx][BUFFER_SIZE - 1] = '\0';
        ctx->global_start = (ctx->global_start + 1) % GLOBAL_BUFFER_SIZE;
    }

    broadcast_message(ctx, sender, buffer);
}

// send a message to one person (don't store in global buffer)
void handle_sayto(server_context_t *ctx, struct sockaddr_in *client_addr, char *content)
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

    if (is_muted(recipient, sender_name)) {
        return;
    }

    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s: %s", sender_name, msg);
    send_to_client(ctx, recipient, buffer);
}

// disconnect client (client will also do a local disconnect)
void handle_disconn(server_context_t *ctx, struct sockaddr_in *client_addr)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *prev = NULL;
    struct Node *cur = ctx->clients_head;

    while (cur != NULL) {
        if (cur->addr.sin_addr.s_addr == client_addr->sin_addr.s_addr &&
            cur->addr.sin_port        == client_addr->sin_port) {

            if (prev == NULL) ctx->clients_head = cur->next;

            else prev->next = cur->next;
                

            char name[MAX_NAME_LEN];
            strncpy(name, cur->client_name, MAX_NAME_LEN);
            heap_remove(ctx, cur);
            free(cur);
            pthread_rwlock_unlock(&ctx->clients_lock);

            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response), "Disconnected. Bye! (%s)", name);
            udp_socket_write(ctx->sd, client_addr, response, BUFFER_SIZE);
            return;
        }
        prev = cur;
        cur = cur->next;
    }

    pthread_rwlock_unlock(&ctx->clients_lock);
}

// change client name in linked list
void handle_rename(server_context_t *ctx, struct sockaddr_in *client_addr, const char *new_name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *client = find_client_by_addr_nolock(ctx, client_addr);
    if (client) {
        strncpy(client->client_name, new_name, MAX_NAME_LEN - 1);
        client->client_name[MAX_NAME_LEN - 1] = '\0';
    }
    pthread_rwlock_unlock(&ctx->clients_lock);

    if (client) {
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response), "You are now known as %s", client->client_name);
        send_to_client(ctx, client, response);
    }
}

// mute other clients
void handle_mute(server_context_t *ctx, struct sockaddr_in *client_addr, const char *name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *client = find_client_by_addr_nolock(ctx, client_addr);
    if (client) {
        add_mute(client, name);
    }
    pthread_rwlock_unlock(&ctx->clients_lock);
}

// unmute other clients
void handle_unmute(server_context_t *ctx, struct sockaddr_in *client_addr, const char *name)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *client = find_client_by_addr_nolock(ctx, client_addr);
    if (client) {
        remove_mute(client, name);
    }
    pthread_rwlock_unlock(&ctx->clients_lock);
}

// if admin (server port = 6666), kick, otherwise don't
void handle_kick(server_context_t *ctx, struct sockaddr_in *client_addr, const char *name)
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

            if (prev == NULL) ctx->clients_head = cur->next;

            else prev->next = cur->next;

            struct sockaddr_in kicked_addr = cur->addr;
            heap_remove(ctx, cur);
            free(cur);
            pthread_rwlock_unlock(&ctx->clients_lock);

            char msg_kicked[BUFFER_SIZE];
            snprintf(msg_kicked, sizeof(msg_kicked), "You have been removed from the chat");
            udp_socket_write(ctx->sd, &kicked_addr, msg_kicked, BUFFER_SIZE);

            char msg_bcast[BUFFER_SIZE];
            snprintf(msg_bcast, sizeof(msg_bcast), "%s has been removed from the chat", name);
            broadcast_message(ctx, NULL, msg_bcast);
            return;
        }
        prev = cur;
        cur = cur->next;
    }

    pthread_rwlock_unlock(&ctx->clients_lock);
}

void handle_ret_ping(server_context_t *ctx, struct sockaddr_in *client_addr)
{
    pthread_rwlock_wrlock(&ctx->clients_lock);

    struct Node *client = find_client_by_addr_nolock(ctx, client_addr);
    if (client) {
        client->last_active = time(NULL);
        client->awaiting_ping_reply = 0;
        client->ping_sent_time = 0;

        if (client->heap_index >= 0) {
            heap_update(ctx, client);
        }
    }

    pthread_rwlock_unlock(&ctx->clients_lock);
}

// tokenise request and use handle_... functions to handle the request
void handle_request(server_context_t *ctx, struct sockaddr_in *client_addr, char *client_request, int length)
{
    char *command = NULL;
    char *content = NULL;

    parse_request(client_request, &command, &content);

    if (command == NULL) {
        return;
    }

    pthread_rwlock_wrlock(&ctx->clients_lock);
    struct Node *client = find_client_by_addr_nolock(ctx, client_addr);
    time_t now = time(NULL);
    if (client) {
        client->last_active = now;
        client->awaiting_ping_reply = 0;
        client->ping_sent_time = 0;
        if (client->heap_index >= 0) {
            heap_update(ctx, client);
        }
    }
    pthread_rwlock_unlock(&ctx->clients_lock);

    if (strcmp(command, "conn") == 0) {
        handle_conn(ctx, client_addr, content);
    } 
    else if (strcmp(command, "say") == 0) {
        handle_say(ctx, client_addr, content);
    } 
    else if (strcmp(command, "sayto") == 0) {
        handle_sayto(ctx, client_addr, content);
    } 
    else if (strcmp(command, "disconn") == 0) {
        handle_disconn(ctx, client_addr);
    } 
    else if (strcmp(command, "rename") == 0) {
        handle_rename(ctx, client_addr, content);
    } 
    else if (strcmp(command, "mute") == 0) {
        handle_mute(ctx, client_addr, content);
    } 
    else if (strcmp(command, "unmute") == 0) {
        handle_unmute(ctx, client_addr, content);
    } 
    else if (strcmp(command, "kick") == 0) {
        handle_kick(ctx, client_addr, content);
    }
    else if (strcmp(command, "ret-ping") == 0) {
        handle_ret_ping(ctx, client_addr);
    }
    else {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "Invalid command: %s", command);
        udp_socket_write(ctx->sd, client_addr, msg, BUFFER_SIZE);
    }
}

// initialise server
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
    ctx.global_count = 0;
    ctx.global_start = 0;
    ctx.heap_size = 0;

    pthread_t listener_tid, ping_tid;
    int rc = pthread_create(&listener_tid, NULL, listener_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to create listener thread\n");
        return 1;
    }

    rc = pthread_create(&ping_tid, NULL, ping_monitor_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "Failed to create ping monitor thread\n");
        return 1;
    }
    
    pthread_join(listener_tid, NULL);
    ctx.running = 0;
    pthread_join(ping_tid, NULL);

    close(sd);
    pthread_rwlock_destroy(&ctx.clients_lock);

    return 0;
}
