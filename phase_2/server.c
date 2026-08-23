#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h> 
#include <sys/time.h>
#include "common.h"

#define MAX_CONCURRENT_CLIENTS 1024

// Global flag to control the main server loop
volatile sig_atomic_t keep_running = 1;

// Synchronization primitives to track active threads
int active_threads = 0;
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;

// Signal handler for SIGINT / SIGTERM
void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

// Helper function to reliably read exactly 'count' bytes from a TCP stream with timeout support
ssize_t read_all(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t r = read(fd, (char*)buf + total_read, count - total_read);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (!keep_running) return -1;
                continue;
            }
            return r;
        }
        if (r == 0) return (ssize_t)total_read; // Peer closed connection
        total_read += r;
    }
    return (ssize_t)total_read;
}

// Helper function to reliably write exactly 'count' bytes to a TCP stream with timeout support
ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t total_written = 0;
    while (total_written < count) {
        ssize_t w = write(fd, (const char*)buf + total_written, count - total_written);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (!keep_running) return -1; 
                continue;
            }
            return w;
        }
        total_written += w;
    }
    return (ssize_t)total_written;
}

/**
 * Safely executes supported system commands based on the request type.
 * Ensures buffer boundaries and handles memory allocation dynamically.
 *
 * @param type    The type of command to execute (PWD, LS, CAT).
 * @param arg     The argument associated with the command (e.g., path or filename).
 * @param out_len Pointer to store the length of the generated output buffer.
 * @return        A dynamically allocated string containing the response data.
 */
char* execute_command_secure(uint16_t type, const char* arg, size_t* out_len) {
    char* output = NULL;
    *out_len = 0;

    if (type == REQUEST_TYPE_PWD) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            *out_len = strlen(cwd) + 1; // includes '\n'
            output = malloc(*out_len + 1);
            if (output) {
                snprintf(output, *out_len + 1, "%s\n", cwd);
            }
        } else {
            output = strdup("Error: getcwd failed\n");
            *out_len = strlen(output);
        }
    } else if (type == REQUEST_TYPE_LS) {
        const char* path = (arg && strlen(arg) > 0) ? arg : ".";
        if (strcmp(path, "/root") == 0) path = "/";

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
                        if (!temp) {
                            free(output);
                            output = strdup("Error: Out of memory\n");
                            current_len = strlen(output);
                            break;
                        }
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
                    if (st.st_size == 0) {
                        output = malloc(1);
                        if (output) output[0] = '\0';
                        *out_len = 0;
                    } else if (st.st_size + sizeof(ResponseHeader) > 65535) {
                        output = strdup("Error: File exceeds 64KB protocol limit\n");
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
                } else {
                    output = strdup("Error: Cannot stat file\n");
                    *out_len = strlen(output);
                }
                close(fd);
            } else {
                output = strdup("Error: Cannot open file\n");
                *out_len = strlen(output);
            }
        }
    } else {
        output = strdup("Error: Invalid Request Type\n");
        *out_len = strlen(output);
    }
    return output;
}

// Thread routine to handle individual client connections
void* client_handler(void* arg) {
    int client_fd = *((int*)arg);
    free(arg); 
    pthread_detach(pthread_self());

    // Set a 2-second timeout for read and write operations
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    RequestHeader header;
    
    while (read_all(client_fd, &header, sizeof(RequestHeader)) == sizeof(RequestHeader)) {
        uint16_t req_type = ntohs(header.req_type);
        uint16_t arg_len = ntohs(header.req_arg_len);
        
        char* req_arg = NULL;
        if (arg_len > 0) {
            req_arg = (char*)malloc(arg_len + 1);
            if (req_arg) {
                if (read_all(client_fd, req_arg, arg_len) != arg_len) {
                    free(req_arg);
                    break; 
                }
                req_arg[arg_len] = '\0';
            }
        }

        size_t data_len = 0;
        char* output = execute_command_secure(req_type, req_arg, &data_len);

        if (req_arg != NULL) {
            free(req_arg);
        }

        if (data_len + sizeof(ResponseHeader) > 65535) {
            if (output) free(output);
            output = strdup("Error: Response exceeds 64KB protocol limit\n");
            data_len = strlen(output);
        }

        uint16_t total_resp_len = (uint16_t)(data_len + sizeof(ResponseHeader));
        ResponseHeader resp_hdr;
        resp_hdr.response_len = htons(total_resp_len); 

        if (write_all(client_fd, &resp_hdr, sizeof(ResponseHeader)) <= 0) {
            if (output) free(output);
            break; 
        }

        if (data_len > 0 && output != NULL) {
            if (write_all(client_fd, output, data_len) <= 0) {
                free(output);
                break;
            }
        }

        if (output) free(output);
    }

    close(client_fd);

    pthread_mutex_lock(&thread_mutex);
    active_threads--;
    if (active_threads == 0) {
        pthread_cond_signal(&thread_cond);
    }
    pthread_mutex_unlock(&thread_mutex);

    pthread_exit(NULL);
}

int main() {
    signal(SIGPIPE, SIG_IGN); 
    
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    int server_fd, new_socket;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SERVER_PORT);

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

    printf("TCP Server listening on port %d\n", SERVER_PORT);
    printf("Press Ctrl+C to stop the server gracefully.\n");

    while (keep_running) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
            if (errno == EINTR || !keep_running) {
                break; 
            }
            perror("Accept error");
            continue;
        }

        pthread_mutex_lock(&thread_mutex);
        if (active_threads >= MAX_CONCURRENT_CLIENTS) {
            pthread_mutex_unlock(&thread_mutex);
            fprintf(stderr, "Max concurrent clients reached (%d). Connection dropped.\n", MAX_CONCURRENT_CLIENTS);
            close(new_socket);
            continue;
        }
        active_threads++;
        pthread_mutex_unlock(&thread_mutex);

        int* client_sock_ptr = malloc(sizeof(int));
        if (!client_sock_ptr) {
            pthread_mutex_lock(&thread_mutex);
            active_threads--;
            pthread_mutex_unlock(&thread_mutex);
            close(new_socket);
            continue;
        }
        *client_sock_ptr = new_socket;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, client_handler, (void*)client_sock_ptr) != 0) {
            perror("Could not create thread");
            free(client_sock_ptr);
            close(new_socket);

            pthread_mutex_lock(&thread_mutex);
            active_threads--;
            if (active_threads == 0) {
                pthread_cond_signal(&thread_cond);
            }
            pthread_mutex_unlock(&thread_mutex);
        }
    }

    printf("\nShutting down server gracefully...\n");
    close(server_fd);
    
    pthread_mutex_lock(&thread_mutex);
    while (active_threads > 0) {
        pthread_cond_wait(&thread_cond, &thread_mutex);
    }
    pthread_mutex_unlock(&thread_mutex);

    return 0;
}

