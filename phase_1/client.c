#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include "common.h"

int main(int argc, char *argv[]) {
    /* Ignore SIGPIPE to prevent the process from crashing if writing to a closed socket */
    signal(SIGPIPE, SIG_IGN); 

    /* Ensure at least the command type argument is provided */
    if (argc < 2) return -1;

    uint16_t req_type = 0;
    char* arg = NULL;
    uint16_t arg_len = 0;

    /* Parse command-line arguments and map them to protocol request types */
    if (strcmp(argv[1], "ls") == 0) {
        req_type = REQUEST_TYPE_LS;
        if (argc > 2) { arg = argv[2]; arg_len = strlen(arg); }
    } else if (strcmp(argv[1], "pwd") == 0) {
        req_type = REQUEST_TYPE_PWD;
    } else if (strcmp(argv[1], "cat") == 0) {
        req_type = REQUEST_TYPE_CAT;
        if (argc > 2) { arg = argv[2]; arg_len = strlen(arg); } 
        else return -1; /* 'cat' command requires a filename argument */
    } else return -1; /* Unknown command */

    /* Create a local UNIX domain stream socket */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    /* Set up the target server address structure with the specified local socket path */
    struct sockaddr_un serv_addr;
    serv_addr.sun_family = AF_UNIX;
    strncpy(serv_addr.sun_path, SOCKET_PATH, sizeof(serv_addr.sun_path) - 1);

    /* Connect to the local server socket path */
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return -1;
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
        if (arg_len > 0) memcpy(send_buf + sizeof(RequestHeader), arg, arg_len);

        // Utilize a while loop to guarantee complete transmission on a blocking socket
        size_t total_written = 0;
        while (total_written < total_len) {
            ssize_t w = write(sock, send_buf + total_written, total_len - total_written);
            if (w <= 0) {
                perror("Write error to server");
                break;
            }
            total_written += w;
        }
        free(send_buf); /* Free the serialized request buffer */
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
        // As defined, response_len includes the size of the header itself
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
                    
                    data[total_read] = '\0'; /* Ensure string formatting safety */
                    printf("--- Response from Server ---\n");
                    fwrite(data, 1, total_read, stdout);
                    
                    /* Ensure output formatting ends cleanly with a trailing newline if missing */
                    if (total_read > 0 && data[total_read - 1] != '\n') {
                        printf("\n");
                    }
                    free(data); /* Clean up allocated data buffer */
                }
            } else {
                printf("--- Response from Server ---\n[Empty Response]\n");
            }
        }
    } else {
        printf("--- Error ---\nFailed to read response header from server.\n");
    }

    /* Close the socket and terminate cleanly */
    close(sock);
    return 0;
}
