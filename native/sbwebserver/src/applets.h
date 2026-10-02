#ifndef SB_APPLETS_H
#define SB_APPLETS_H
#include <stddef.h>
void applets_json(char *out,size_t n); void stations_json(char*out,size_t n); int stations_put(const char*body); int stations_csv(char*out,size_t n); int stations_import(const char*body,size_t len);
#endif
