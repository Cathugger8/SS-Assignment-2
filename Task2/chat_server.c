
#include <stdio.h>
#include <stdlib.h>
#include "udp.h"

struct Node {
    char *client_name;
    char *client_IP;
    char **muted_list;
    struct Node *next;
};

struct Node* create_node(char *client_name, char *client_IP) {
    struct Node* new_node = malloc(sizeof(struct Node));
    new_node->client_name = client_name;
    new_node->client_IP = client_IP;
    new_node->next = NULL;
    return new_node;
}

void push_front(struct Node** head, char *client_name, char *client_IP) {
    struct Node* new_node = create_node(client_name, client_IP);
    new_node->next = *head;
    *head = new_node;
}

void append(struct Node** head, char *client_name, char *client_IP) {
    struct Node* new_node = create_node(client_name, client_IP);

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

int main(int argc, char *argv[])
{

    // This function opens a UDP socket,
    // binding it to all IP interfaces of this machine,
    // and port number SERVER_PORT
    // (See details of the function in udp.h)
    int sd = udp_socket_open(SERVER_PORT);

    assert(sd > -1);



    // Server main loop
    while (1) 
    {
        // Storage for request and response messages
        char client_request[BUFFER_SIZE], server_response[BUFFER_SIZE];

        // Demo code (remove later)
        printf("Server is listening on port %d\n", SERVER_PORT);

        // Variable to store incoming client's IP address and port
        struct sockaddr_in client_address;

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_address.sin_addr), addr_str, INET_ADDRSTRLEN);
    
        // This function reads incoming client request from
        // the socket at sd.
        // (See details of the function in udp.h)
        int rc = udp_socket_read(sd, &client_address, client_request, BUFFER_SIZE);

        // Successfully received an incoming request
        if (rc > 0)
        {
            int upper = 0;
            int lower = 0;

            for (int i = 0; client_request[i] != '\0'; i++) {
                char c = client_request[i];

                if (c >= 'A' && c <= 'Z') {
                    upper = 1;
                }
                else if (c >= 'a' && c <= 'z') {
                    lower = 1;
                }
            }
            // Demo code (remove later)
            strcpy(server_response, "Hi, the server has received: ");
            strcat(server_response, client_request);
            strcat(server_response, " from the IP address: ");
            strcat(server_response, addr_str);
            strcat(server_response, " This request contains ");
            if (upper && !lower) {
                strcat(server_response, "only upper case");
            }
            else if (!upper && lower) {
                strcat(server_response, "only lower case");
            }
            else if (upper && lower) {
                strcat(server_response, "mixed case");
            }
            strcat(server_response, "\n");

            // This function writes back to the incoming client,
            // whose address is now available in client_address, 
            // through the socket at sd.
            // (See details of the function in udp.h)
            rc = udp_socket_write(sd, &client_address, server_response, BUFFER_SIZE);

            // Demo code (remove later)
            printf("Request served...\n");
        }
    }

    return 0;
}