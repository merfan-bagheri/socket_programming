#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <arpa/inet.h>

#define SOCKET_PATH "/tmp/recruitment_socket.sock"

#define REQUEST_TYPE_LS  1
#define REQUEST_TYPE_PWD 2
#define REQUEST_TYPE_CAT 3

#pragma pack(push, 1)
typedef struct {
    uint16_t req_len;
    uint16_t req_type;
    uint16_t req_arg_len;
} RequestHeader;

typedef struct {
    uint16_t response_len;
} ResponseHeader;
#pragma pack(pop)

#endif // COMMON_H
