#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include "common.h"

void print_usage(const char* prog_name) {
    printf("Usage: %s <command> [argument]\n", prog_name);
    printf("Supported Commands:\n");
    printf("  pwd             - Print current working directory on server\n");
    printf("  ls [dir]        - List files in current or specified directory\n");
    printf("  cat <filepath>  - Display contents of a file on server\n");
}

ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t total_written = 0;
    while (total_written < count) {
        ssize_t w = write(fd, (const char*)buf + total_written, count - total_written);
        if (w <= 0) return w;
        total_written += w;
    }
    return (ssize_t)total_written;
}

ssize_t read_all(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t r = read(fd, (char*)buf + total_read, count - total_read);
        if (r <= 0) return r;
        total_read += r;
    }
    return (ssize_t)total_read;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    uint16_t req_type = 0;
    const char* arg = NULL;
    uint16_t arg_len = 0;

    if (strcmp(argv[1], "ls") == 0) {
        req_type = REQUEST_TYPE_LS;
        if (argc > 2) {
            arg = argv[2];
            arg_len = (uint16_t)strlen(arg);
        }
    } else if (strcmp(argv[1], "pwd") == 0) {
        req_type = REQUEST_TYPE_PWD;
    } else if (strcmp(argv[1], "cat") == 0) {
        req_type = REQUEST_TYPE_CAT;
        if (argc > 2) {
            arg = argv[2];
            arg_len = (uint16_t)strlen(arg);
        } else {
            fprintf(stderr, "Error: 'cat' command requires a filename argument.\n");
            return 1;
        }
    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation error");
        return 1;
    }

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection to TCP server failed");
        close(sock);
        return 1;
    }

    RequestHeader header;
    uint16_t total_len = sizeof(RequestHeader) + arg_len;

    header.req_len = htons(total_len);
    header.req_type = htons(req_type);
    header.req_arg_len = htons(arg_len);

    if (write_all(sock, &header, sizeof(RequestHeader)) <= 0) {
        perror("Failed to send header");
        close(sock);
        return 1;
    }

    if (arg_len > 0 && arg != NULL) {
        if (write_all(sock, arg, arg_len) <= 0) {
            perror("Failed to send argument");
            close(sock);
            return 1;
        }
    }

    ResponseHeader resp_hdr;
    if (read_all(sock, &resp_hdr, sizeof(ResponseHeader)) == sizeof(ResponseHeader)) {
        uint16_t resp_len = ntohs(resp_hdr.response_len);

        if (resp_len >= sizeof(ResponseHeader)) {
            uint16_t data_len = resp_len - sizeof(ResponseHeader);

            if (data_len > 0) {
                char* buffer = (char*)malloc(data_len + 1);
                if (buffer) {
                    ssize_t bytes_read = read_all(sock, buffer, data_len);
                    if (bytes_read > 0) {
                        buffer[bytes_read] = '\0';
                        printf("--- Response from Server ---\n");
                        fwrite(buffer, 1, bytes_read, stdout);
                        if (bytes_read > 0 && buffer[bytes_read - 1] != '\n') {
                            printf("\n");
                        }
                    } else {
                        fprintf(stderr, "Failed to read data payload from server.\n");
                    }
                    free(buffer);
                }
            } else {
                printf("--- Response from Server ---\n[Empty Response]\n");
            }
        }
    } else {
        fprintf(stderr, "Failed to read response header from server.\n");
        close(sock);
        return 1;
    }

    close(sock);
    return 0;
}

