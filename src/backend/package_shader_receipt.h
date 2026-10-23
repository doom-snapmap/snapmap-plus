/* Exact shader declaration bytes observed during successful native parsing. */
#ifndef SH_PACKAGE_SHADER_RECEIPT_H
#define SH_PACKAGE_SHADER_RECEIPT_H
#include <stddef.h>
#include <stdint.h>

#include "package_shader.h"

typedef int (*sh_package_shader_receipt_read)(void *context, uintptr_t address,
    void *destination, size_t length);
typedef struct sh_package_shader_receipt_frame {
    struct sh_package_shader_receipt_frame *parent;
    void *pending;
    uintptr_t resource;
    int owner;
} sh_package_shader_receipt_frame;

/* Every native source parse must bracket a frame, including non-shader parses
 * which isolate nested file reads. Nested wrappers for the same object share
 * the outer receipt. Begin invalidates earlier proof before native mutation.
 * The ledger is independent of provider/asset handles so startup observations
 * survive creation and refresh of the installed catalog adapter. */
void sh_package_shader_receipt_begin(sh_package_shader_receipt_frame *frame,
    uintptr_t resource, const char *family, const char *name);
void sh_package_shader_receipt_end(sh_package_shader_receipt_frame *frame,
    int parsed, sh_package_shader_receipt_read read, void *context);
/* Invalidate before native reconstruction, destruction or unobserved mutation.
 * A lookup, source checksum or later file read never creates a receipt. */
void sh_package_shader_receipt_invalidate(uintptr_t resource);

/* Fixed ABI frames accept only their actual source path. Admitted program
 * frames also observe include/source paths; callers require every decoded
 * dependency before accepting a unit. Read observations contain bytes returned to the engine, the
 * stream's original length, and the starting offset. Partial/contradictory reads
 * cannot certify a source. No read or stream replacement occurs in this API. */
int sh_package_shader_receipt_wants(const char *path);
void sh_package_shader_receipt_bytes(const char *path, size_t total,
    size_t offset, const void *body, size_t length);
/* Direct ParseSource(readSource=0) consumes the already retained native text.
 * Call only after verifying that branch and before invoking its parser. */
void sh_package_shader_receipt_retained(const void *body, size_t length);
/* Bracket the verified native text setter without changing its arguments.
 * Before invalidates an existing receipt outside this object's active parse;
 * after records the exact resulting +0x38/+0x40 text only in that same parse.
 * Stream observations must agree after the native space/CR/LF trim. */
void sh_package_shader_receipt_text_begin(uintptr_t resource);
void sh_package_shader_receipt_text(uintptr_t resource, const void *body, size_t length);

/* Program capture is restricted to an explicitly admitted native preparation.
 * The compiler witness is separate from parse success and from any cached
 * binary read. Nested generated permutations retain their own receipts. */
void sh_package_shader_receipt_program_scope(int enter);
/* Revoke every construction observed in the current admitted scope, including
 * fixed parameters and nested programs, after failed native state restoration. */
void sh_package_shader_receipt_revoke_scope(void);
uintptr_t sh_package_shader_receipt_program_current(void);
void sh_package_shader_receipt_program_compiled(uintptr_t resource);
int sh_package_shader_receipt_program_wrote(uintptr_t resource,
    const void *metadata, size_t length);
typedef struct sh_package_shader_input {
    struct sh_package_shader_input *next;
    char *path;
    unsigned char *body;
    size_t length;
} sh_package_shader_input;
typedef struct sh_package_shader_capture {
    unsigned char *metadata; size_t metadata_length;
    unsigned char *source; size_t source_length;
    sh_package_shader_input *inputs;
    uint64_t ticket; /* Same admitted source construction, including children. */
} sh_package_shader_capture;
/* Copies a successful source-compiled result and complete actual file reads.
 * Partial or contradictory reads are omitted, so callers must require every
 * decoded include. NULL source copies the witnessed generated declaration;
 * use that form only for a decoded permutation with the parent's same ticket.
 * No callbacks run while the receipt ledger is locked. The
 * caller checks exact candidate/original include bytes and current membership. */
int sh_package_shader_receipt_program_copy(uintptr_t resource, const char *name,
    const void *source, size_t length, const unsigned char *header,
    sh_package_shader_capture *capture);
void sh_package_shader_capture_clear(sh_package_shader_capture *capture);

/* An isolated source cook owns parser/metadata workspaces and never constructs
 * a registered GPU program. Its certificate proves only actual compiler input
 * and output, not equality with any live program. token is an owned temporary
 * identity, renderer is 0 (OpenGL) or 1 (Vulkan). Begin/end must be inside the
 * admitted program scope; success requires a compiler witness, metadata and
 * complete source observation and every observed input. Native input reads in nested cooks are also
 * recorded by their enclosing cook. All products from a failed scope revoke
 * together. A NULL expected source is only for the parent's decoded child. */
void sh_package_shader_cook_begin(sh_package_shader_receipt_frame *frame,
    uintptr_t token, int renderer, const char *name);
void sh_package_shader_cook_end(sh_package_shader_receipt_frame *frame, int parsed);
int sh_package_shader_cook_copy(int renderer, const char *name,
    const void *source, size_t length, sh_package_shader_capture *capture);

/* The caller supplies a current stable native header and separately verifies
 * registry membership, native ID and readiness. Exact source plus immutable
 * identity/default/ABI fields must match one successful observed construction.
 * Text/length/checksum retirement, lifetime promotion and a renderparm's
 * engine-owned creator claim (+0x74) are allowed; changes to default value,
 * native kind/index, data type, flag mask or source record are not. */
int sh_package_shader_receipt_matches(uintptr_t resource, const char *family,
    const char *name, const void *source, size_t length,
    const unsigned char *header, size_t header_length);

/* Diagnostics only: why sh_package_shader_receipt_matches refuses the same
 * arguments, written to out ("no source receipt", "receipt header differs at
 * +0x68", ...). Never evidence of readiness. Returns out. */
char *sh_package_shader_receipt_refusal(uintptr_t resource, const char *family,
    const char *name, const void *source, size_t length,
    const unsigned char *header, size_t header_length, char *out, size_t capacity);

#ifdef SH_PACKAGE_SHADER_RECEIPT_TESTING
void sh_package_shader_receipt_test_reset(void);
/* A ledger entry exactly as a backend's ledger held it, for replaying a
 * captured observation. resource is the replayed object standing for the
 * observed one (0 for a cook entry); header holds 0x90 bytes. A cleared entry
 * has no body. Nothing here creates or completes evidence: the entry is stored
 * as observed, and the ordinary matchers decide what it proves. */
typedef struct sh_package_shader_receipt_observed_input {
    const char *path;
    const unsigned char *body, *seen; /* length bytes each (1 when empty) */
    size_t length, covered;
    int failed;
} sh_package_shader_receipt_observed_input;
typedef struct sh_package_shader_receipt_observation {
    int cook, renderer;
    uintptr_t resource;
    uint64_t epoch;
    const char *family, *name;
    const unsigned char *body;
    size_t length, header_length;
    int retained;
    const unsigned char *header;
    const unsigned char *metadata;
    size_t metadata_length;
    const sh_package_shader_receipt_observed_input *inputs;
    size_t input_count;
    uint64_t ticket, preparation_ticket;
} sh_package_shader_receipt_observation;
/* Appends one observed entry behind the existing ones, keeping ledger order.
 * 0 when it is malformed, repeats an entry's key or memory is exhausted. */
int sh_package_shader_receipt_test_replay(const sh_package_shader_receipt_observation *observed);
#endif
#endif
