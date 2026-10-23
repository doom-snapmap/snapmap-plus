/* Owned page-family inputs and sealed streams for one compiled binding. */
#ifndef SH_PACKAGE_VIRTUAL_PAGE_SOURCE_H
#define SH_PACKAGE_VIRTUAL_PAGE_SOURCE_H
#include "package_compiler.h"
#include "package_virtual_pages.h"
typedef struct sh_virtual_page_source sh_virtual_page_source;
/* Return 1 with a retained immutable installed file, 0 absent, -1 failure.
 * Originals bypass package overrides. The caller transfers the reference. */
typedef int (*sh_virtual_page_original)(void *context, const char *path,
    sh_package_file **file, char *error, size_t capacity);

/* Capture exactly 16 sq, four q, and the pages fallback in native order.
 * Authored files resolve in this source scope; missing companions resolve only
 * from installed originals. Validate all members and retain source expectations
 * until the normal compiler cache seals them. No native calls or file writes.
 * Empty mega2 members have the native meaning of absent members. */
sh_virtual_page_source *sh_virtual_page_source_open(const sh_package_compilation *scope,
    const char *source, sh_virtual_page_original original, void *context,
    char *error, size_t capacity);
sh_virtual_page_source *sh_virtual_page_source_retain(sh_virtual_page_source *source);
void sh_virtual_page_source_release(sh_virtual_page_source *source);
const unsigned char *sh_virtual_page_source_identity(const sh_virtual_page_source *source);
const char *sh_virtual_page_source_origin(const sh_virtual_page_source *source);
/* Exact positive family comparison, including every captured payload. This is
 * required after an identity match and before sharing one native provider. */
int sh_virtual_page_source_equal(const sh_virtual_page_source *a, const sh_virtual_page_source *b,
    char *error, size_t capacity);
/* Return the captured compiler input for a native member slot, or NULL for an
 * absent/empty member. Its file/source indices retain the original inventory
 * numbering. Bodies, installed file references and metadata are owned here. */
const sh_compiled_resource *sh_virtual_page_source_member(const sh_virtual_page_source *source, unsigned slot);
/* Seal all effective members from cached or owned-body private compiler paths.
 * paths[slot] names exactly the corresponding projection; absent members use
 * NULL. Verify captured identity AND positive bytes before retaining streams.
 * Atomic on failure, idempotent on the same captured bytes. The source may then
 * outlive its input scope/inventory, including every renderer stream it opens. */
int sh_virtual_page_source_seal(sh_virtual_page_source *source,
    const sh_package_compilation *compiled, const char *const paths[21],
    char *error, size_t capacity);
/* 1 opens an owned FILE stream, 0 absent, -1 unsealed/unreadable. The caller
 * closes the stream. It has an independent cursor and outlives source release. */
int sh_virtual_page_source_open_member(const sh_virtual_page_source *source, unsigned slot,
    FILE **stream, uint64_t *length, char *error, size_t capacity);
/* A body-backed sealed member has no FILE. Return its immutable borrowed body
 * and size, or NULL for a file/absent/unsealed member. Keep a source reference
 * until the exact native stream constructor has copied these bytes. */
const unsigned char *sh_virtual_page_source_body_member(const sh_virtual_page_source *source,
    unsigned slot, size_t *length);
int sh_virtual_page_source_sealed(const sh_virtual_page_source *source);
/* Read exactly count bytes at offset of a captured member, sealed or not, with
 * its own cursor. 1 read, 0 absent/empty member, -1 unreadable. */
int sh_virtual_page_source_member_read(const sh_virtual_page_source *source, unsigned slot,
    uint64_t offset, void *bytes, size_t count, char *error, size_t capacity);
/* 1 when the member was captured from the installed original because the
 * scope supplies no file at that path, else 0. */
int sh_virtual_page_source_member_installed(const sh_virtual_page_source *source, unsigned slot);
#endif
