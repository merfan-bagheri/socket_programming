#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include "common.h"

/**
 * Writes the exact number of bytes specified by count to a file descriptor.
 * Handles partial writes by looping until all data is sent or an error occurs.
 *
 * @param fd    The file descriptor (e.g., socket) to write to.
 * @param buf   Pointer to the buffer containing data to write.
 * @param count Total number of bytes to write.
 * @return      Total bytes written on success, or <= 0 on error/disconnection.
 */
ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t total_written = 0;
    while (total_written < count) {
        ssize_t w = write(fd, (const char*)buf + total_written, count - total_written);
        if (w <= 0) return w; /* Error or socket closed */
        total_written += w;
    }
    return total_written;
}

/**
 * Reads the exact number of bytes specified by count from a file descriptor.
 * Handles partial reads by looping until all requested data is received.
 *
 * @param fd    The file descriptor (e.g., socket) to read from.
 * @param buf   Pointer to the buffer where data will be stored.
 * @param count Total number of bytes to read.
 * @return      Total bytes read on success, or <= 0 on error/disconnection.
 */
ssize_t read_all(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t r = read(fd, (char*)buf + total_read, count - total_read);
        if (r <= 0) return r; /* Error or EOF (socket closed) */
        total_read += r;
    }
    return total_read;
}

int main(int argc, char *argv[]) {
    /* Ignore SIGPIPE to prevent the process from crashing when writing to a closed socket */
    signal(SIGPIPE, SIG_IGN);

    /* Validate command-line arguments */
    if (argc < 2) {
        printf("Usage: %s <type> [arg]\n", argv[0]);
        return -1;
    }

    uint16_t req_type = 0;
    char* arg = NULL;
    uint16_t arg_len = 0;

    /* Parse and validate the command type */
    if (strcmp(argv[1], "ls") == 0) {
        req_type = REQUEST_TYPE_LS;
        if (argc > 2) {
            arg = argv[2];
            arg_len = strlen(arg);
        }
    } else if (strcmp(argv[1], "pwd") == 0) {
        req_type = REQUEST_TYPE_PWD;
    } else if (strcmp(argv[1], "cat") == 0) {
        req_type = REQUEST_TYPE_CAT;
        if (argc > 2) {
            arg = argv[2];
            arg_len = strlen(arg);
        } else {
            printf("Error: cat requires an absolute filename argument.\n");
            return -1;
        }
    } else {
        printf("Unknown request type.\n");
        return -1;
    }

    int sock = 0;
    struct sockaddr_in serv_addr;
    
    /* Create a TCP IPv4 socket */
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    /* Configure server address structure */
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT); /* Convert port to network byte order */

    /* Convert IP address from text to binary format */
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        return -1;
    }

    /* Establish connection to the server */
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }

    /* Construct the request packet */
    RequestHeader header;
    uint16_t total_len = sizeof(RequestHeader) + arg_len;

    /* Convert header fields to network byte order (Big Endian) */
    header.req_len = htons(total_len);
    header.req_type = htons(req_type);
    header.req_arg_len = htons(arg_len);

    // send argument and header
    /* Send the fixed-size request header to the server */
    if (write_all(sock, &header, sizeof(RequestHeader)) <= 0) {
        perror("Failed to send header");
        close(sock);
        return -1;
    }

    /* Send the variable-length argument payload if it exists */
    if (arg_len > 0) {
        if (write_all(sock, arg, arg_len) <= 0) {
            perror("Failed to send argument");
            close(sock);
            return -1;
        }
    }

    /* Receive and process the server's response */
    ResponseHeader resp_hdr;
    if (read_all(sock, &resp_hdr, sizeof(ResponseHeader)) == sizeof(ResponseHeader)) {
        /* Convert response length back to host byte order */
        uint16_t resp_len = ntohs(resp_hdr.response_len);

        /* Ensure the reported length includes at least the header itself */
        if (resp_len >= sizeof(ResponseHeader)) {
            uint16_t data_len = resp_len - sizeof(ResponseHeader);

            if (data_len > 0) {
                /* Allocate memory for the incoming data payload (+1 for null terminator) */
                char* buffer = (char*)malloc(data_len + 1);
                if (buffer) {
                    /* Read the actual data payload from the socket */
                    if (read_all(sock, buffer, data_len) == data_len) {
                        buffer[data_len] = '\0'; /* Null-terminate the string */
                        printf("%s\n", buffer);  /* Print response to standard output */
                    } else {
                        printf("Error: Failed to read complete response data.\n");
                    }
                    free(buffer); /* Clean up allocated memory */
                }
            } else {
                printf("[Empty Response]\n");
            }
        }
    } else {
        printf("Error: Failed to read response header.\n");
    }

    /* Close socket and release resources */
    close(sock);
    return 0;
}
