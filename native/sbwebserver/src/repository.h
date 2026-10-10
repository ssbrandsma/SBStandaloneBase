#ifndef SB_REPOSITORY_H
#define SB_REPOSITORY_H
#include <stddef.h>

#define REPO_MAX_APPLETS 32
#define REPO_MAX_UPLOAD (16u * 1024u * 1024u)
#define REPO_MAX_ENTRIES 512
#define REPO_MAX_UNCOMPRESSED (64u * 1024u * 1024u)

typedef struct {
  char name[65], title[129], version[33], target[33], min_target_version[33];
  char url[513], sha[41], desc[513], changes[513], creator[129], email[129];
} RepoApplet;

const char *repo_config_path(void);
const char *repo_root(void);
int repo_storage_ready(char *reason, size_t reason_size);
int repo_list_json(char *out, size_t cap);
int repo_get(const char *name, RepoApplet *out);
int repo_save_applet(const RepoApplet *item, int require_existing, char *error, size_t error_size);
int repo_remove_applet(const char *name, int delete_unreferenced, char *error, size_t error_size);
int repo_validate_metadata(const RepoApplet *item, int external, char *error, size_t error_size);
int repo_validate_zip(const char *path, char *detected_name, size_t name_cap,
                      char *error, size_t error_size);
int repo_sha1_file(const char *path, char hex[41]);
int repo_commit_upload(const char *temporary, RepoApplet *item, char *error, size_t error_size);
int repo_package_path(const char *id, char *path, size_t cap);
int repo_is_local_url(const char *url);
void repo_notify_catalog(void);

#endif
