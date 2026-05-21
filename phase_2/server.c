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
#include <sys/time.h> // Added for struct timeval
#include "common.h"

// Global flag to control the main server loop
volatile sig_atomic_t keep_running = 1;

// Synchronization primitives to track active threads
int active_threads = 0;
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;

// Signal handler for SIGINT (Ctrl+C)
void handle_sigint(int sig) {
    keep_running = 0;
}

// Helper function to reliably read exactly 'count' bytes from a TCP stream with timeout support
ssize_t read_all(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t r = read(fd, (char*)buf + total_read, count - total_read);
        if (r < 0) {
            // Check if read timed out or was interrupted
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (!keep_running) return -1; // Exit cleanly if server is shutting down
                continue; // Otherwise, try again
            }
            return r; // Real error occurred
        }
        if (r == 0) return 0; // Client disconnected
        total_read += r;
    }
    return total_read;
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
    return total_written;
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
        /* Get current working directory */
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            *out_len = strlen(cwd) + 1;
            output = malloc(*out_len);
            if (output) snprintf(output, *out_len, "%s\n", cwd);
        } else {
            output = strdup("Error: getcwd failed\n");
            *out_len = strlen(output);
        }
    } else if (type == REQUEST_TYPE_LS) {
        /* Default to current directory if no argument is supplied */
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

                /* Read directory entries sequentially */
                while ((ent = readdir(dir)) != NULL) {
                    size_t name_len = strlen(ent->d_name);
                    /* Dynamically resize output buffer if capacity is reached */
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
                /* Determine file size to allocate appropriate buffer */
                if (fstat(fd, &st) == 0) {
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

    // Set a 1-second timeout for read and write operations
    // This prevents threads from hanging forever if the client is idle
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    RequestHeader header;
    
    // Loop will naturally break if timeout occurs AND keep_running == 0
    while (read_all(client_fd, &header, sizeof(RequestHeader)) == sizeof(RequestHeader)) {
        /* Convert incoming network byte order fields to host order */
        uint16_t req_type = ntohs(header.req_type);
        uint16_t arg_len = ntohs(header.req_arg_len);
        
        char* req_arg = NULL;
        /* Read optional command argument if present */
        if (arg_len > 0) {
            req_arg = (char*)malloc(arg_len + 1);
            if (req_arg) {
                if (read_all(client_fd, req_arg, arg_len) != arg_len) {
                    free(req_arg);
                    break; 
                }
                req_arg[arg_len] = '\0'; // Null-terminate argument string
            }
        }

        size_t data_len = 0;
        /* Execute requested remote operation */
        char* output = execute_command_secure(req_type, req_arg, &data_len);

        if (req_arg != NULL) {
            free(req_arg);
        }

        /* Calculate total response length including the response header */
        uint16_t total_resp_len = data_len + sizeof(ResponseHeader);
        ResponseHeader resp_hdr;
        resp_hdr.response_len = htons(total_resp_len); 

        /* Send fixed-size response header back to client */
        if (write_all(client_fd, &resp_hdr, sizeof(ResponseHeader)) <= 0) {
            if (output) free(output);
            break; 
        }

        /* Send variable-size data payload back to client */
        if (data_len > 0 && output != NULL) {
            if (write_all(client_fd, output, data_len) <= 0) {
                free(output);
                break;
            }
        }

        if (output) free(output);
    }

    /* Connection cleanup */
    close(client_fd);

    // Safely decrement the active thread counter
    pthread_mutex_lock(&thread_mutex);
    active_threads--;
    if (active_threads == 0) {
        pthread_cond_signal(&thread_cond); // Wake up main thread if this was the last client
    }
    pthread_mutex_unlock(&thread_mutex);

    pthread_exit(NULL);
}

int main() {
    /* Ignore SIGPIPE to avoid process abortion when writing to a broken socket */
    signal(SIGPIPE, SIG_IGN); 
    
    // Using sigaction instead of signal to explicitly PREVENT SA_RESTART
    // This ensures accept() returns immediately when Ctrl+C is pressed
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // DO NOT set SA_RESTART
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    /* Allocate IPv4 TCP endpoint */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* Enable address reuse to allow rapid restarts without binding delays */
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    /* Set up target listening address profile */
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SERVER_PORT);

    /* Bind server endpoint onto system port */
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    /* Place socket in listen state with a connection backlog pool */
    if (listen(server_fd, 1024) < 0) { 
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("TCP Server listening on port %d\n", SERVER_PORT);
    printf("Press Ctrl+C to stop the server gracefully.\n");

    /* Core connection dispatching loop */
    while (keep_running) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            // accept will now return -1 (EINTR) when Ctrl+C is pressed
            if (errno == EINTR || !keep_running) {
                break; 
            }
            perror("Accept error");
            continue;
        }

        /* Dynamically package connection file descriptor for the thread */
        int* client_sock_ptr = malloc(sizeof(int));
        if (!client_sock_ptr) {
            close(new_socket);
            continue;
        }
        *client_sock_ptr = new_socket;

        /* Atomically register thread allocation tracking */
        pthread_mutex_lock(&thread_mutex);
        active_threads++;
        pthread_mutex_unlock(&thread_mutex);

        /* Launch individual worker context */
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, client_handler, (void*)client_sock_ptr) != 0) {
            perror("Could not create thread");
            free(client_sock_ptr);
            close(new_socket);

            /* Revert connection count upon generation failures */
            pthread_mutex_lock(&thread_mutex);
            active_threads--;
            if (active_threads == 0) {
                pthread_cond_signal(&thread_cond);
            }
            pthread_mutex_unlock(&thread_mutex);
        }
    }

    printf("\nShutting down server gracefully...\n");
    close(server_fd); // Stop accepting new connections
    
    // Wait for all active clients to disconnect safely (they will timeout within 1s)
    pthread_mutex_lock(&thread_mutex);
    while (active_threads > 0) {
        pthread_cond_wait(&thread_cond, &thread_mutex);
    }
    pthread_mutex_unlock(&thread_mutex);

    return 0;
}
