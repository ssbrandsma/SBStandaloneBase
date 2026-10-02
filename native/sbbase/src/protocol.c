#include "sbbase.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *value_for(const char tag[5], const sb_config *c, const char *ip) {
    if (!memcmp(tag, "IPAD", 4)) return ip;
    if (!memcmp(tag, "NAME", 4)) return c->name;
    if (!memcmp(tag, "VERS", 4)) return c->version;
    if (!memcmp(tag, "UUID", 4)) return c->uuid;
    return NULL;
}

size_t sb_discovery_response(const unsigned char *r, size_t n, unsigned char *o,
                             size_t cap, const sb_config *c, const char *ip) {
    size_t p = 1, q = 1;
    if (!r || !o || !c || n < 1 || n > SB_MAX_DISCOVERY || (r[0] != 'e' && r[0] != 'E') || cap < 1) return 0;
    o[0] = 'E';
    while (p + 5 <= n) {
        char tag[5]; const char *v; size_t len = r[p + 4];
        memcpy(tag, r + p, 4); tag[4] = 0; p += 5;
        if (p + len > n) break;
        p += len;
        if (!memcmp(tag, "JSON", 4)) {
            char port[12]; snprintf(port, sizeof port, "%u", c->http_port); v = port;
            len = strlen(v); if (q + 5 + len > cap) break;
            memcpy(o + q, tag, 4); o[q + 4] = (unsigned char)len; memcpy(o + q + 5, v, len); q += 5 + len;
            continue;
        }
        v = value_for(tag, c, ip ? ip : "");
        if (!v) continue;
        len = strlen(v); if (len > 255 || q + 5 + len > cap) break;
        memcpy(o + q, tag, 4); o[q + 4] = (unsigned char)len; memcpy(o + q + 5, v, len); q += 5 + len;
    }
    return q;
}

int sb_parse_client_frame(const unsigned char *d, size_t n, char op[5],
                          const unsigned char **payload, uint32_t *len, size_t *used) {
    uint32_t l;
    if (n < 8) return 0;
    l = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
    if (l > SB_MAX_SLIM_FRAME) return -1;
    if (n < 8u + l) return 0;
    memcpy(op, d, 4); op[4] = 0; *payload = d + 8; *len = l; *used = 8u + l; return 1;
}

size_t sb_server_frame(const char op[4], const void *p, size_t n, unsigned char *o, size_t cap) {
    size_t total = n + 4;
    if (total > 65535 || cap < total + 2) return 0;
    o[0] = (unsigned char)(total >> 8); o[1] = (unsigned char)total;
    memcpy(o + 2, op, 4); if (n) memcpy(o + 6, p, n); return total + 2;
}

/* Strict, bounded pre-validation. Full signature verification is performed before
 * this function by catalog.c; this routine deliberately accepts no schema-less data. */
int sb_catalog_valid(const char *j, size_t n) {
    size_t i; int depth = 0, string = 0, esc = 0;
    if (!j || !n || n > SB_MAX_HTTP_BODY || !strstr(j, "\"applets\"") || !strstr(j, "\"version\"")) return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)j[i];
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') return 0;
        if (string) { if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') string = 0; }
        else if (c == '"') string = 1;
        else if (c == '{' || c == '[') ++depth;
        else if ((c == '}' || c == ']') && --depth < 0) return 0;
    }
    return !string && depth == 0;
}
