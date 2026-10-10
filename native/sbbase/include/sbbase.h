#ifndef SBBASE_H
#define SBBASE_H
#include <stddef.h>
#include <stdint.h>

#define SB_MAX_DISCOVERY 512
#define SB_MAX_SLIM_FRAME 65536
#define SB_MAX_HTTP_HEADER 16384
#define SB_MAX_HTTP_BODY 131072
#define SBBASE_VERSION "0.2.6"
#define SB_LMS_COMPAT_VERSION "7.999.999"

typedef struct {
    char name[64], uuid[40], lms_version[24], advertise_ip[16];
    char catalog_path[256], state_path[256];
    unsigned http_port, slim_port, discovery_port;
    int time_sync;
} sb_config;

size_t sb_discovery_response(const unsigned char *request, size_t request_len,
                             unsigned char *out, size_t out_size,
                             const sb_config *config, const char *ip);
int sb_parse_client_frame(const unsigned char *data, size_t size, char opcode[5],
                          const unsigned char **payload, uint32_t *payload_len,
                          size_t *consumed);
size_t sb_server_frame(const char opcode[4], const void *payload, size_t payload_len,
                       unsigned char *out, size_t out_size);
int sb_catalog_valid(const char *json, size_t length);

#endif
