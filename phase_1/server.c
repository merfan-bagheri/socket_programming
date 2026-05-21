#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <dirent.h>     
#include <sys/stat.h>   
#include <fcntl.h>      
#include "common.h"

#define MAX_CLIENTS 1000
#define CLIENT_BUFFER_SIZE 4096

/* Global flag used for handling graceful shutdown via signals */
volatile sig_atomic_t keep_running = 1;

/**
 * Signal handler for termination events (SIGINT and SIGTERM).
 * Sets the global lifecycle loop condition flag to false.
 */
void sig_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        keep_running = 0;
    }
}

/* Struct tracking the state, protocol reading block, and outgoing data for each client */
typedef struct {
    int fd;
    char read_buffer[CLIENT_BUFFER_SIZE];
    size_t read_len;
    
    char* write_buffer; 
    size_t write_len;   
} ClientState;

/* Node tracking inside a FIFO queue storing incoming operational requests */
typedef struct RequestNode {
    int client_fd;
    uint16_t req_type;
    char arg[1024];
    struct RequestNode* next;
} RequestNode;

/* Wrapper containing head and tail boundaries for dynamic queue management */
typedef struct {
    RequestNode* head;
    RequestNode* tail;
} RequestQueue;

/**
 * Puts an explicit file descriptor into non-blocking mode.
 * Crucial step to prevent I/O methods from locking the single main execution line.
 */
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/**
 * Appends a new node representing an operational request to the tail of the queue.
 */
void enqueue(RequestQueue* q, int client_fd, uint16_t type, const char* arg) {
    RequestNode* newNode = (RequestNode*)malloc(sizeof(RequestNode));
    if (!newNode) return;
    newNode->client_fd = client_fd;
    newNode->req_type = type;
    memset(newNode->arg, 0, sizeof(newNode->arg));
    if (arg != NULL) {
        strncpy(newNode->arg, arg, sizeof(newNode->arg) - 1);
    }
    newNode->next = NULL;

    if (q->tail == NULL) {
        q->head = q->tail = newNode;
    } else {
        q->tail->next = newNode;
        q->tail = newNode;
    }
}

/**
 * Pops and returns the request element located at the head of the queue.
 */
RequestNode* dequeue(RequestQueue* q) {
    if (q->head == NULL) return NULL;
    RequestNode* temp = q->head;
    q->head = q->head->next;
    if (q->head == NULL) q->tail = NULL;
    return temp;
}

/**
 * Closes connections and safely releases dynamic allocations tied to a specific client profile.
 */
void disconnect_client(ClientState* client) {
    if (client->fd > 0) {
        close(client->fd);
        client->fd = 0;
    }
    client->read_len = 0;
    if (client->write_buffer) {
        free(client->write_buffer);
        client->write_buffer = NULL;
    }
    client->write_len = 0;
}

/**
 * Executes a secure internal file system command matching targeted system categories.
 *
 * @param type    The category mapping of the targeted operation (PWD, LS, CAT).
 * @param arg     The dynamic payload context representing absolute or relative targets.
 * @param out_len Reference parameter updated to hold the direct length of the result string.
 * @return        A newly allocated text buffer payload meant for transmission.
 */
char* execute_command(uint16_t type, const char* arg, size_t* out_len) {
    char* output = NULL;
    *out_len = 0;

    if (type == REQUEST_TYPE_PWD) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            *out_len = strlen(cwd) + 1; 
            output = malloc(*out_len + 1);
            if (output) snprintf(output, *out_len + 1, "%s\n", cwd);
        } else {
            output = strdup("Error: getcwd failed\n");
            *out_len = strlen(output);
        }
    } else if (type == REQUEST_TYPE_LS) {
        const char* path = ".";
        if (arg && strlen(arg) > 0) {
            if (strcmp(arg, "/root") == 0) path = "/";
            else path = arg;
        }

        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *ent;
            size_t capacity = 1024;
            output = malloc(capacity);
            if (output) {
                output[0] = '\0';
                size_t current_len = 0;

                while ((ent = readdir(dir)) != NULL) {
                    size_t name_len = strlen(ent->d_name);
                    if (current_len + name_len + 2 > capacity) {
                        capacity *= 2;
                        char* temp = realloc(output, capacity);
                        if (!temp) break; // Handle realloc failure
                        output = temp;
                    }
                    memcpy(output + current_len, ent->d_name, name_len);
                    current_len += name_len;
                    output[current_len++] = '\n';
                    output[current_len] = '\0';
                }
                *out_len = current_len;
            }
            closedir(dir);
        } else {
            output = strdup("Error: Cannot open directory\n");
            *out_len = strlen(output);
        }
    } else if (type == REQUEST_TYPE_CAT) {
        if (!arg || strlen(arg) == 0) {
            output = strdup("Error: No file specified\n");
            *out_len = strlen(output);
        } else {
            int fd = open(arg, O_RDONLY);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0) {
                    /* Restrict processing targets that exceed the protocol limits */
                    if (st.st_size + sizeof(ResponseHeader) > 65535) {
                        output = strdup("Error: File is too large (exceeds 64KB limit)\n");
                        *out_len = strlen(output);
                    } else {
                        *out_len = st.st_size;
                        output = malloc(*out_len + 1);
                        if (output) {
                            ssize_t bytes_read = read(fd, output, *out_len);
                            if (bytes_read >= 0) {
                                output[bytes_read] = '\0';
                                *out_len = bytes_read;
                            } else {
                                free(output);
                                output = strdup("Error: Failed to read file\n");
                                *out_len = strlen(output);
                            }
                        }
                    }
                }
                close(fd);
            } else {
                output = strdup("Error: Cannot open file\n");
                *out_len = strlen(output);
            }
        }
    } else {
        output = strdup("Error: Unknown command\n");
        *out_len = strlen(output);
    }
    return output;
}

int main() {
    int server_fd, new_socket;
    ClientState clients[MAX_CLIENTS];
    
    /* Initialize active states tracking structural matrices */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = 0;
        clients[i].read_len = 0;
        clients[i].write_buffer = NULL;
        clients[i].write_len = 0;
    }
    
    fd_set readfds, writefds;
    RequestQueue req_queue = {NULL, NULL};

    /* Register operational system handlers for signal monitoring */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN); /* Prevent termination if writing to broken pipe */

    /* Instantiate an internal Local UNIX IPC streaming socket */
    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    set_nonblocking(server_fd);

    struct sockaddr_un address;
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCKET_PATH, sizeof(address.sun_path) - 1);
    unlink(SOCKET_PATH); /* Remove existing lingering UNIX domain sockets */

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 1024) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /* Core execution loop driven by multiplexed socket event processing */
    while (keep_running) {
        // Process one request from the queue per cycle
        if (req_queue.head != NULL) {
            RequestNode* req = dequeue(&req_queue);
            if (req != NULL) {
                ClientState* target_client = NULL;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd == req->client_fd) {
                        target_client = &clients[i];
                        break;
                    }
                }

                if (target_client != NULL) {
                    size_t data_len = 0;
                    char* output = execute_command(req->req_type, req->arg, &data_len);

                    /* Enforce size check limits against final packet definitions */
                    if (data_len + sizeof(ResponseHeader) > 65535) {
                        if (output) free(output);
                        output = strdup("Error: Response length exceeded 64KB limit\n");
                        data_len = strlen(output);
                    }

                    // Calculate total message length for the response header
                    size_t total_response_size = sizeof(ResponseHeader) + data_len;
                    ResponseHeader resp_hdr;
                    resp_hdr.response_len = htons((uint16_t)total_response_size);

                    size_t new_write_len = target_client->write_len + total_response_size;
                    char* new_buffer = realloc(target_client->write_buffer, new_write_len);
                    
                    if (new_buffer) {
                        target_client->write_buffer = new_buffer;
                        memcpy(target_client->write_buffer + target_client->write_len, &resp_hdr, sizeof(ResponseHeader));
                        if (data_len > 0 && output != NULL) {
                            memcpy(target_client->write_buffer + target_client->write_len + sizeof(ResponseHeader), output, data_len);
                        }
                        target_client->write_len = new_write_len;
                    } else {
                        // Handle realloc failure: system out of memory
                        fprintf(stderr, "Error: Out of memory during realloc for client %d\n", target_client->fd);
                        disconnect_client(target_client);
                    }
                    if (output) free(output);
                }
                free(req);
            }
        }

        /* Clear and configure bit arrays for monitoring inside select() */
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(server_fd, &readfds);
        int max_sd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int sd = clients[i].fd;
            if (sd > 0) {
                FD_SET(sd, &readfds);
                /* Register file descriptor for write updates if data is pending in buffer */
                if (clients[i].write_len > 0) {
                    FD_SET(sd, &writefds);
                }
                if (sd > max_sd) max_sd = sd;
            }
        }

        // Smart timeout: poll immediately if the queue has items; otherwise, block and wait
        struct timeval tv;
        struct timeval* tv_ptr = NULL;
        if (req_queue.head != NULL) {
            tv.tv_sec = 0;
            tv.tv_usec = 0; // Return immediately (polling mode)
            tv_ptr = &tv;
        } // If queue is empty, tv_ptr remains NULL causing select() to block indefinitely

        /* Multiplexed synchronous I/O barrier blocking execution waiting for activity */
        int activity = select(max_sd + 1, &readfds, &writefds, NULL, tv_ptr);

        if (activity < 0 && errno != EINTR) break;
        if (!keep_running) break;

        /* Manage entry validations processing incoming node connections */
        if (FD_ISSET(server_fd, &readfds)) {
            if ((new_socket = accept(server_fd, NULL, NULL)) >= 0) {
                // Enforce FD_SETSIZE limit to prevent crash
                if (new_socket >= FD_SETSIZE) {
                    fprintf(stderr, "Error: Max FD limit reached. Cannot accept new client.\n");
                    close(new_socket);
                } else {
                    set_nonblocking(new_socket); 
                    int added = 0;
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (clients[i].fd == 0) {
                            clients[i].fd = new_socket;
                            clients[i].read_len = 0;
                            added = 1;
                            break;
                        }
                    }
                    if (!added) close(new_socket); 
                }
            }
        }

        /* Scan through tracking array to manage registered sockets */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int sd = clients[i].fd;
            if (sd <= 0) continue;

            // --- Write Phase (Non-Blocking + memmove) ---
            if (FD_ISSET(sd, &writefds)) {
                ssize_t w = write(sd, clients[i].write_buffer, clients[i].write_len);
                
                if (w > 0) {
                    if ((size_t)w == clients[i].write_len) {
                        free(clients[i].write_buffer);
                        clients[i].write_buffer = NULL;
                        clients[i].write_len = 0;
                    } else {
                        /* Keep partial write remainder and discard sent fraction using memmove */
                        size_t remaining = clients[i].write_len - w;
                        memmove(clients[i].write_buffer, clients[i].write_buffer + w, remaining);
                        clients[i].write_len = remaining;
                    }
                } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    disconnect_client(&clients[i]);
                    continue; 
                }
            }

            // --- Read Phase ---
            if (FD_ISSET(sd, &readfds)) {
                char temp_buf[2048];
                ssize_t valread = read(sd, temp_buf, sizeof(temp_buf));
                
                if (valread == 0) {
                    disconnect_client(&clients[i]); 
                } else if (valread < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        disconnect_client(&clients[i]);
                    }
                } else {
                    /* Verify space availability inside fixed internal storage bounds */
                    if (clients[i].read_len + valread <= CLIENT_BUFFER_SIZE) {
                        memcpy(clients[i].read_buffer + clients[i].read_len, temp_buf, valread);
                        clients[i].read_len += valread;
                        
                        /* Stream parser loop processing all complete packets inside read buffer */
                        while (clients[i].read_len >= sizeof(RequestHeader)) {
                            RequestHeader* header = (RequestHeader*)clients[i].read_buffer;
                            uint16_t req_len = ntohs(header->req_len);
                            
                            /* Drop malicious framing or invalid protocol definitions */
                            if (req_len < sizeof(RequestHeader) || req_len > CLIENT_BUFFER_SIZE) {
                                disconnect_client(&clients[i]); 
                                break;
                            }
                            
                            /* Proceed only if the entire packet payload length has arrived */
                            if (clients[i].read_len >= req_len) {
                                uint16_t req_type = ntohs(header->req_type);
                                uint16_t arg_len = ntohs(header->req_arg_len);
                                
                                char arg[1024] = {0};
                                if (arg_len > 0) {
                                    size_t copy_len = (arg_len < sizeof(arg)) ? arg_len : sizeof(arg) - 1;
                                    memcpy(arg, clients[i].read_buffer + sizeof(RequestHeader), copy_len);
                                }
                                
                                enqueue(&req_queue, sd, req_type, arg);
                                
                                /* Shift data residue leftward to reset block configurations */
                                size_t remaining = clients[i].read_len - req_len;
                                if (remaining > 0) {
                                    memmove(clients[i].read_buffer, clients[i].read_buffer + req_len, remaining);
                                }
                                clients[i].read_len = remaining;
                            } else {
                                break; 
                            }
                        }
                    } else {
                        disconnect_client(&clients[i]);
                    }
                }
            }
        }
    }

    /* Post-shutdown server lifecycle cleaning steps */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        disconnect_client(&clients[i]);
    }
    close(server_fd);
    unlink(SOCKET_PATH);

    /* Free all requests lingering inside the FIFO structure */
    RequestNode* current = req_queue.head;
    while (current != NULL) {
        RequestNode* next = current->next;
        free(current);
        current = next;
    }

    return 0;
}
