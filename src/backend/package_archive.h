/* Complete source delivery; no metadata is added to authored folders. */
#ifndef SH_PACKAGE_ARCHIVE_H
#define SH_PACKAGE_ARCHIVE_H
#include "package_sources.h"
#include <limits.h>
/* Native resource streams use signed 32-bit idFile lengths. Archive counts
 * and offsets use ZIP64 as needed, without aggregate package-size quotas. */
#define SH_PACKAGE_ARCHIVE_MAX_FILE_BYTES ((size_t)INT_MAX)

/* Owned, fully verified archive members for compiler input conversion. Reading
 * raw members validates ZIP structure, paths and every CRC before returning;
 * descriptor semantics are the converter's responsibility. */
typedef struct sh_package_archive_file {
    char *name;
    unsigned char *body;
    size_t length;
    int directory;
} sh_package_archive_file;
typedef struct sh_package_archive_files {
    sh_package_archive_file *items;
    size_t count;
} sh_package_archive_files;
int sh_package_archive_read(const unsigned char *bytes, size_t length,
    sh_package_archive_files *out, char *error, size_t capacity);
void sh_package_archive_files_free(sh_package_archive_files *files);
unsigned char *sh_package_archive_write(const sh_package_archive_files *files,
    size_t *length, char *error, size_t capacity);

/* All returned buffers are malloc-owned. Files and empty directories retain
 * their exact paths and bytes. A changed source refuses the entire archive. */
unsigned char *sh_package_archive_pack(const sh_package_sources *sources, size_t owner,
                                       size_t *length, char *error, size_t capacity);
/* Validate every member, its CRC, paths and the root descriptor before writing.
 * Extraction requires a new destination and never overwrites a file. Failed
 * writes leave an unpublished staging directory for the caller to handle. */
int sh_package_archive_inspect(const unsigned char *bytes, size_t length,
                               char id[SH_PACKAGE_ID_CAP], unsigned *files,
                               char *error, size_t capacity);
/* Complete authored tree identity, independent of ZIP encoding. Implicit
 * parent directories count exactly as they do after extraction. The transport
 * checksum must still be verified by the map carrier. */
int sh_package_archive_identity(const unsigned char *bytes, size_t length,
                                char id[SH_PACKAGE_ID_CAP], unsigned char fingerprint[32],
                                char *error, size_t capacity);
int sh_package_archive_unpack(const unsigned char *bytes, size_t length,
                              const char *destination, unsigned *files,
                              char *error, size_t capacity);
#endif
