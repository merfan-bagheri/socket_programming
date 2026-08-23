#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include "common.h"

void print_usage(const char* prog_name) {
    printf("Usage: %s <command> [argument]\n", prog_name);
    printf("Supported Commands:\n");
    printf("  pwd             - Print current working directory on server\n");
    printf("  ls [dir]        - List files in current or specified directory\n");
    printf("  cat <filepath>  - Display contents of a file on server\n");
}

int main(int argc, char *argv[]) {
    /* Ignore SIGPIPE to prevent the process from crashing if writing to a closed socket */
    signal(SIGPIPE, SIG_IGN); 

    /* Validate arguments */
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    uint16_t req_type = 0;
    const char* arg = NULL;
    uint16_t arg_len = 0;

    /* Parse command-line arguments and map them to protocol request types */
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

    /* Create a local UNIX domain stream socket */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error creating socket");
        return 1;
    }

    /* Set socket timeout for read/write operations (5 seconds) */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    /* Set up the target server address structure with the specified local socket path */
    struct sockaddr_un serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    snprintf(serv_addr.sun_path, sizeof(serv_addr.sun_path), "%s", SOCKET_PATH);

    /* Connect to the local server socket path */
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection to server failed");
        close(sock);
        return 1;
    }

    /* Construct the protocol request header and convert fields to network byte order */
    RequestHeader header;
    uint16_t total_len = sizeof(RequestHeader) + arg_len;
    header.req_len = htons(total_len);
    header.req_type = htons(req_type);
    header.req_arg_len = htons(arg_len);

    /* Allocate and serialize the header and its optional argument payload into a single buffer */
    char* send_buf = malloc(total_len);
    if (send_buf) {
        memcpy(send_buf, &header, sizeof(RequestHeader));
        if (arg_len > 0 && arg != NULL) {
            memcpy(send_buf + sizeof(RequestHeader), arg, arg_len);
        }

        size_t total_written = 0;
        while (total_written < total_len) {
            ssize_t w = write(sock, send_buf + total_written, total_len - total_written);
            if (w <= 0) {
                perror("Write error to server");
                free(send_buf);
                close(sock);
                return 1;
            }
            total_written += w;
        }
        free(send_buf);
    } else {
        fprintf(stderr, "Out of memory allocating send buffer\n");
        close(sock);
        return 1;
    }

    /* Read the fixed-size response header from the socket, handling partial reads */
    ResponseHeader resp_hdr;
    size_t hdr_read = 0;
    while (hdr_read < sizeof(ResponseHeader)) {
        ssize_t r = read(sock, ((char*)&resp_hdr) + hdr_read, sizeof(ResponseHeader) - hdr_read);
        if (r <= 0) break;
        hdr_read += r;
    }
    
    /* Process the incoming message if the header was read completely */
    if (hdr_read == sizeof(ResponseHeader)) {
        uint16_t msg_len = ntohs(resp_hdr.response_len);
        
        if (msg_len >= sizeof(ResponseHeader)) {
            uint16_t data_len = msg_len - sizeof(ResponseHeader);
            
            /* If the response contains a data payload, allocate memory and read it */
            if (data_len > 0) {
                char* data = malloc((size_t)data_len + 1);
                if (data) {
                    size_t total_read = 0;
                    
                    /* Read the exact length of the data payload */
                    while (total_read < data_len) {
                        ssize_t bytes = read(sock, data + total_read, data_len - total_read);
                        if (bytes <= 0) break;
                        total_read += bytes;
                    }
                    
                    data[total_read] = '\0';
                    printf("--- Response from Server ---\n");
                    fwrite(data, 1, total_read, stdout);
                    
                    if (total_read > 0 && data[total_read - 1] != '\n') {
                        printf("\n");
                    }
                    free(data);
                }
            } else {
                printf("--- Response from Server ---\n[Empty Response]\n");
            }
        }
    } else {
        fprintf(stderr, "--- Error ---\nFailed to read response header from server.\n");
        close(sock);
        return 1;
    }

    close(sock);
    return 0;
}

