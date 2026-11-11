/* modstore_internal.h -- the file helpers modstore.c shares with
 * modstore_upgrade.c (the store tool's half of the store). Not an interface
 * of the store: only those two files include it. The helpers keep their
 * modstore.c names here and link as sh_msi_*. */
#ifndef SNAPMAP_PLUS_MODSTORE_INTERNAL_H
#define SNAPMAP_PLUS_MODSTORE_INTERNAL_H

#include <windows.h>
#include "modstore.h"

#define ms_cause            sh_msi_cause
#define ms_why              sh_msi_why
#define ms_log              sh_msi_log
#define ms_wjoin            sh_msi_wjoin
#define ms_wjoin_utf8       sh_msi_wjoin_utf8
#define ms_wroot            sh_msi_wroot
#define ms_mkdirs           sh_msi_mkdirs
#define ms_inside           sh_msi_inside
#define ms_candidate        sh_msi_candidate
#define ms_has_descriptor   sh_msi_has_descriptor
#define ms_delete_file      sh_msi_delete_file
#define ms_rmtree           sh_msi_rmtree
#define ms_exists           sh_msi_exists
#define ms_is_dir           sh_msi_is_dir
#define ms_has_file         sh_msi_has_file
#define ms_file_size        sh_msi_file_size
#define ms_hash_wfile       sh_msi_hash_wfile
#define ms_read_text        sh_msi_read_text
#define ms_is_hash          sh_msi_is_hash

int         ms_cause(char *cause, size_t cap, const char *fmt, ...);
const char *ms_why(DWORD err, const char *name, int in_store, unsigned long long bytes);
void        ms_log(const char *fmt, ...);
wchar_t    *ms_wjoin(const wchar_t *dir, const wchar_t *name);
wchar_t    *ms_wjoin_utf8(const wchar_t *dir, const char *rel);
wchar_t    *ms_wroot(void);
int         ms_mkdirs(const wchar_t *path);
int         ms_inside(const wchar_t *path, const wchar_t *dir);
int         ms_candidate(const WIN32_FIND_DATAW *fd);
int         ms_has_descriptor(const wchar_t *folder);
int         ms_delete_file(const wchar_t *path);
int         ms_rmtree(const wchar_t *path, const wchar_t *root);
int         ms_exists(const wchar_t *path);
int         ms_is_dir(const wchar_t *path);
int         ms_has_file(const wchar_t *dir, const wchar_t *leaf);
int         ms_file_size(const wchar_t *path, unsigned long long *size);
int         ms_hash_wfile(const wchar_t *path, unsigned long long *size, char out[SH_MB_HASH_HEX + 1]);
char       *ms_read_text(const wchar_t *path, size_t max, size_t *len);
int         ms_is_hash(const char *h);

#endif /* SNAPMAP_PLUS_MODSTORE_INTERNAL_H */
