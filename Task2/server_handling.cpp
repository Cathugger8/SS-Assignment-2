#include <stdio.h>
#include <string.h>

void parse_request(char *buffer, char **command, char **content) {
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }

    *command = strtok(buffer, "$"); // tokenise everything before the $
    *content = strtok(NULL, "\0"); // tokenise  everything after the $

    if (*content == NULL) {
        *content = "";
    }

    while (**content == ' ')
        (*content)++;
}

void handle_request(server_context_t *ctx, struct sockaddr_in *client_addr, char *client_request, int length) {

}