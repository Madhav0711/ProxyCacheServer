#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MAX_BYTES 4096
#define MAX_CLIENTS 400
#define MAX_CACHE_SIZE 200*(1<<20)
#define MAX_ELEMENT_SIZE 10*(1<<20)

typedef struct cache_element cache_element;

struct cache_element {
    char *data;
    int length;
    char *url;
    time_t lru_time;
    cache_element *next;
};

cache_element *cache_head;
int current_cache_size;

int port_number = 8080;
int proxy_socket_id;
pthread_t thread_ids[MAX_CLIENTS];
sem_t semaphore;
pthread_mutex_t cache_lock;

cache_element *find_in_cache(char *url);
int add_to_cache(char *data, int size, char *url);
void remove_from_cache();
int send_error_response(int socket, int status_code);
int connect_to_remote_server(char *host, int port);
int handle_client_request(int client_socket, struct ParsedRequest *request, char *full_request);
int validate_http_version(char *version);
void *client_handler(void *client_socket_ptr);
int initialize_proxy(int argc, char *argv[]);

int main(int argc, char *argv[]) {
    return initialize_proxy(argc, argv);
}

int initialize_proxy(int argc, char *argv[]) {
    int client_socket, client_len;
    struct sockaddr_in server_addr, client_addr;

    sem_init(&semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&cache_lock, NULL);

    if (argc == 2) {
        port_number = atoi(argv[1]);
    } else {
        printf("Too few arguments\n");
        exit(1);
    }

    printf("Setting Proxy Server Port : %d\n", port_number);

    proxy_socket_id = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socket_id < 0) {
        perror("Failed to create socket.\n");
        exit(1);
    }

    int reuse = 1;
    if (setsockopt(proxy_socket_id, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed\n");

    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(proxy_socket_id, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Port is not free\n");
        exit(1);
    }
    printf("Binding on port: %d\n", port_number);

    if (listen(proxy_socket_id, MAX_CLIENTS) < 0) {
        perror("Error while Listening !\n");
        exit(1);
    }

    int i = 0;
    int connected_sockets[MAX_CLIENTS];

    while (1) {
        bzero((char*)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr);
        client_socket = accept(proxy_socket_id, (struct sockaddr*)&client_addr, (socklen_t*)&client_len);
        if (client_socket < 0) {
            fprintf(stderr, "Error in Accepting connection !\n");
            exit(1);
        } else {
            connected_sockets[i] = client_socket;
        }

        struct sockaddr_in* client_ptr = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_ptr->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
        printf("Client is connected with port number: %d and ip address: %s \n", ntohs(client_addr.sin_port), str);

        pthread_create(&thread_ids[i], NULL, client_handler, (void*)&connected_sockets[i]);
        i++;
    }
    close(proxy_socket_id);
    return 0;
}

void *client_handler(void *client_socket_ptr) {
    sem_wait(&semaphore);
    int sem_value;
    sem_getvalue(&semaphore, &sem_value);
    printf("Semaphore value: %d\n", sem_value);

    int client_socket = *((int*)client_socket_ptr);
    int bytes_received, len;

    char *buffer = (char*)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);
    bytes_received = recv(client_socket, buffer, MAX_BYTES, 0);

    while (bytes_received > 0) {
        len = strlen(buffer);
        if (strstr(buffer, "\r\n\r\n") == NULL) {
            bytes_received = recv(client_socket, buffer + len, MAX_BYTES - len, 0);
        } else {
            break;
        }
    }

    char *full_request = (char*)malloc(strlen(buffer) * sizeof(char) + 1);
    strcpy(full_request, buffer);

    cache_element *cached_response = find_in_cache(full_request);
    if (cached_response != NULL) {
        int size = cached_response->length / sizeof(char);
        int pos = 0;
        char response[MAX_BYTES];
        while (pos < size) {
            bzero(response, MAX_BYTES);
            for (int i = 0; i < MAX_BYTES; i++) {
                response[i] = cached_response->data[pos];
                pos++;
            }
            send(client_socket, response, MAX_BYTES, 0);
        }
        printf("Data retrieved from the Cache\n\n");
        printf("%s\n\n", response);
    } else if (bytes_received > 0) {
        len = strlen(buffer);
        struct ParsedRequest *request = ParsedRequest_create();
        if (ParsedRequest_parse(request, buffer, len) < 0) {
            printf("Parsing failed\n");
        } else {
            bzero(buffer, MAX_BYTES);
            if (!strcmp(request->method, "GET")) {
                if (request->host && request->path && (validate_http_version(request->version) == 1)) {
                    bytes_received = handle_client_request(client_socket, request, full_request);
                    if (bytes_received == -1) {
                        send_error_response(client_socket, 500);
                    }
                } else {
                    send_error_response(client_socket, 500);
                }
            } else {
                printf("This code doesn't support any method other than GET\n");
            }
        }
        ParsedRequest_destroy(request);
    } else if (bytes_received < 0) {
        perror("Error in receiving from client.\n");
    } else if (bytes_received == 0) {
        printf("Client disconnected!\n");
    }

    shutdown(client_socket, SHUT_RDWR);
    close(client_socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, &sem_value);
    printf("Semaphore post value: %d\n", sem_value);
    free(full_request);
    return NULL;
}

cache_element *find_in_cache(char *url) {
    cache_element *element = NULL;
    pthread_mutex_lock(&cache_lock);
    if (cache_head != NULL) {
        element = cache_head;
        while (element != NULL) {
            if (!strcmp(element->url, url)) {
                printf("LRU Time Track Before: %ld\n", element->lru_time);
                printf("URL found\n");
                element->lru_time = time(NULL);
                printf("LRU Time Track After: %ld\n", element->lru_time);
                break;
            }
            element = element->next;
        }
    } else {
        printf("URL not found\n");
    }
    pthread_mutex_unlock(&cache_lock);
    return element;
}

void remove_from_cache() {
    cache_element *prev = NULL;
    cache_element *current = NULL;
    cache_element *oldest = NULL;
    pthread_mutex_lock(&cache_lock);
    if (cache_head != NULL) {
        oldest = cache_head;
        for (current = cache_head, prev = cache_head; current->next != NULL; current = current->next) {
            if ((current->next->lru_time) < (oldest->lru_time)) {
                oldest = current->next;
                prev = current;
            }
        }
        if (oldest == cache_head) {
            cache_head = cache_head->next;
        } else {
            prev->next = oldest->next;
        }
        current_cache_size -= (oldest->length) + sizeof(cache_element) + strlen(oldest->url) - 1;
        free(oldest->data);
        free(oldest->url);
        free(oldest);
    }
    pthread_mutex_unlock(&cache_lock);
}

int add_to_cache(char *data, int size, char *url) {
    int element_size = size + 1 + strlen(url) + sizeof(cache_element);
    if (element_size > MAX_ELEMENT_SIZE) {
        pthread_mutex_unlock(&cache_lock);
        return 0;
    } else {
        if (current_cache_size + element_size > MAX_CACHE_SIZE) {
            while (current_cache_size + element_size > MAX_CACHE_SIZE) {
                remove_from_cache();
            }
        }

        cache_element *element = (cache_element*)malloc(sizeof(cache_element));
        element->data = (char*)malloc(size * sizeof(char));
        strcpy(element->data, data);
        element->url = (char*)malloc((strlen(url) + 1) * sizeof(char));
        strcpy(element->url, url);
        element->length = size + 1;
        element->lru_time = time(NULL);
        pthread_mutex_lock(&cache_lock);
        element->next = cache_head;
        cache_head = element;
        current_cache_size += element_size;
        pthread_mutex_unlock(&cache_lock);
        return 1;
    }
}

int validate_http_version(char *version) {
    if (strcmp(version, "HTTP/1.0") == 0 || strcmp(version, "HTTP/1.1") == 0) {
        return 1;
    } else {
        printf("Invalid HTTP version: %s\n", version);
        return 0;
    }
}

int send_error_response(int socket, int status_code) {
    char buffer[MAX_BYTES];
    bzero(buffer, MAX_BYTES);
    const char *status_line;
    if (status_code == 500) {
        status_line = "HTTP/1.0 500 Internal Server Error\r\n";
    } else if (status_code == 404) {
        status_line = "HTTP/1.0 404 Not Found\r\n";
    }
    sprintf(buffer, "%sContent-Length: 0\r\n\r\n", status_line);
    send(socket, buffer, MAX_BYTES, 0);
    return 1;
}

int connect_to_remote_server(char *host, int port) {
    struct sockaddr_in remote_server_addr;
    struct hostent *remote_server;
    int remote_socket_id;

    remote_socket_id = socket(AF_INET, SOCK_STREAM, 0);
    if (remote_socket_id < 0) {
        perror("Failed to create socket for Remote Server.\n");
        return -1;
    }

    remote_server = gethostbyname(host);
    if (remote_server == NULL) {
        perror("No such host found.\n");
        return -1;
    }

    bzero((char*)&remote_server_addr, sizeof(remote_server_addr));
    remote_server_addr.sin_family = AF_INET;
    remote_server_addr.sin_port = htons(port);
    bcopy((char*)remote_server->h_addr, (char*)&remote_server_addr.sin_addr.s_addr, remote_server->h_length);

    if (connect(remote_socket_id, (struct sockaddr*)&remote_server_addr, sizeof(remote_server_addr)) < 0) {
        perror("Connection to remote server failed.\n");
        return -1;
    }

    return remote_socket_id;
}

int handle_client_request(int client_socket, struct ParsedRequest *request, char *full_request) {
    char buffer[MAX_BYTES];
    char *host = request->host;
    int port = (request->port == NULL) ? 80 : atoi(request->port);
    int remote_socket = connect_to_remote_server(host, port);

    if (remote_socket < 0) {
        perror("Remote socket connection failed.\n");
        return -1;
    }

    size_t request_length = 0;
    if (ParsedRequest_unparse(request, NULL, request_length) < 0) {
        perror("Failed to get request length.\n");
        return -1;
    }
    char *request_string = (char*)malloc(request_length);
    if (ParsedRequest_unparse(request, request_string, request_length) < 0) {
        perror("Failed to unparse request.\n");
        free(request_string);
        return -1;
    }

    if (send(remote_socket, request_string, strlen(request_string), 0) < 0) {
        perror("Sending request to remote server failed.\n");
        free(request_string);
        return -1;
    }

    int bytes_received = recv(remote_socket, buffer, MAX_BYTES, 0);
    if (bytes_received <= 0) {
        perror("Receiving response from remote server failed.\n");
        free(request_string);
        return -1;
    }

    char *response = (char*)malloc(MAX_BYTES * sizeof(char));
    bzero(response, MAX_BYTES);

    while (bytes_received > 0) {
        send(client_socket, buffer, bytes_received, 0);
        strcat(response, buffer);
        bytes_received = recv(remote_socket, buffer, MAX_BYTES, 0);
    }

    printf("Adding to cache\n\n");
    add_to_cache(response, strlen(response), full_request);
    free(request_string);
    free(response);
    return bytes_received;
}

