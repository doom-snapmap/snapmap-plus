/* Assemble scoped source dependencies for the resource linker. */
#ifndef SH_PACKAGE_SOURCE_LINK_H
#define SH_PACKAGE_SOURCE_LINK_H
#include "package_link_inputs.h"
#include "package_source_graph.h"

typedef struct sh_package_source_link sh_package_source_link;
typedef struct sh_package_audio_program sh_package_audio_program;
typedef struct sh_package_source_memo sh_package_source_memo;
typedef struct sh_package_source_reuse sh_package_source_reuse;
typedef int (*sh_package_source_implicit)(void *context, size_t scope, int observing,
    const sh_package_link_unit *unit,
    int (*visit)(void *, const char *slot, const char *family, const char *name),
    void *visit_context, char *error, size_t capacity);
typedef struct sh_package_source_link_native {
    void *context;
    /* Admit a verified engine-created identity with no physical source. Never
     * infer admission from a currently loaded/modded object or an absent file.
     * 1 admits with stable semantics, 0 is unsupported, -1 is an error. This
     * callback is used only after proven source absence, not a read failure.
     * The linker requires admitted zero-file units to retain their native name. */
    int (*admit)(void *context, size_t scope, const char *family, const char *name,
        unsigned char semantics[32], char *error, size_t capacity);
    /* Bind stable implicit/name-derived state for every file-backed unit into
     * its equivalence identity. Return 1 if this state matches the engine
     * original, 0 if different/uncertified, -1 on an incomplete/failed read.
     * The driver independently checks exact original file bytes; the linker
     * checks explicit dependencies transitively. No callback means no native
     * original certificate. Zero-file units use admit's semantics instead.
     * A product-provided baseline is not an engine-original certificate. */
    int (*semantics)(void *context, size_t scope, const char *family,
        const char *name, unsigned char semantics[32], char *error, size_t capacity);
    /* The current renderer and verified fixed-parameter ABI. Its context is
     * independent of the generic admission/semantics context above. */
    const sh_package_source_shader *shader;
    /* Add exact implicit page-source witnesses after the material's physical
     * declaration is captured. The adapter owns returned metadata through
     * source-link close; each role is unique and the visitor copies its name.
     * These files are streamed for equality, never passed to a decl parser. */
    int (*material_companions)(void *context, size_t scope, const char *material,
        int (*visit)(void *, const char *, const sh_compiled_resource *), void *visit_context,
        char *error, size_t capacity);
    /* Seed materials reached by authored page/table families before reverse
     * root nomination. The visitor captures their exact implicit witnesses. */
    int (*material_seeds)(void *context, size_t scope,
        int (*visit)(void *, const char *), void *visit_context,
        char *error, size_t capacity);
    /* Enumerate proven constructor/consumer resource requests outside the
     * serialized bytes. The unit's physical members have already been captured.
     * Return 1 after the complete walk, 0/-1 on unsupported/failed capture.
     * Each nonempty semantic slot resolves in this scope, never a peer scope.
     * Repeating the same slot/identity is harmless; changing its target is a
     * capture error. Strings are borrowed for the visitor call and copied.
     * observing is nonzero only for reverse-root discovery in the original-only
     * scope. Such units may lack source and are not admitted runtime objects.
     * No runtime load, mutation or assumption of original equivalence is allowed.
     * Consumer publication remains a separate native adapter obligation. */
    sh_package_source_implicit implicit;
    /* Capture delivery ownership after complete discovery and implicit
     * semantics. The adapter owns returned sets until source-link close.
     * Null sets mean no additional owners. This cannot grant admission or
     * substitute for a complete identity certificate. Return 1 on success. */
    int (*owners)(void *context, size_t scope, const char *family, const char *name,
        const sh_package_owners **owners, const sh_package_owners **gameplay,
        char *error, size_t capacity);
    /* Optional retained audio inventory for this exact scope list. Owns raw
     * bank/media source accounting and reverse sound nomination. */
    sh_package_audio_program *audio;
    /* Prepare a single-file unit's effective native fields before dependency
     * discovery. 0 keeps its source; 1 returns malloc-owned equivalent text;
     * -2 denotes invalid authored state, -3 unavailable inherited meaning,
     * -1 an operational failure. A rewrite
     * must preserve native meaning, not introduce policy. Exact source bytes
     * remain the original certificate witness and are never overwritten. */
    int (*prepare)(void *context, sh_package_source_reader *reader,
        const sh_package_link_unit *unit, unsigned char **body, size_t *length,
        char *error, size_t capacity);
    /* Optional discovery recordings retained from earlier captures in this
     * process (sh_package_source_memo_open), borrowed for this capture. */
    sh_package_source_memo *memo;
    /* Optional collector of every scope this capture withdraws
     * (sh_package_source_withdrawals below), borrowed for this capture. */
    struct sh_package_source_withdrawals *withdrawals;
    /* The scopes' product producer (their compile environment's), or NULL.
     * A path neither supplied nor installed that it claims is a product
     * original: its output over the originals, as the compiler evaluates an
     * absent original. A scope that changes an input it read is refused. */
    sh_package_producer produce;
    void *producer_context;
    /* The environment's produce_derived (package_compiler.h), or NULL. */
    int (*produce_derived)(void *context, const char *path);
    /* Return the native module that owns a producer output, if any. */
    int (*produce_module)(void *context, const char *path, char *module, size_t capacity);
    /* Optional native image availability (R-309), asked once for each file-backed image unit
     * before certification, with the generic context above. 1: the unit's scope declares the
     * image gate open and the registered native image of that name is an exclusion placeholder;
     * semantics receives what a private copy is certified with. 0: nothing to do. -1: unknown or
     * failed (error set); the compile fails. The linker decides per native name:
     * - a placeholder that a fixed native ABI unit (renderparm, renderprog, sampler) of a
     *   gate-opening scope reaches keeps its original binding in every scope, because that unit
     *   keeps its own; it is listed for in-place recovery (sh_package_source_link_recovered);
     * - any other placeholder becomes a private copy in each gate-opening scope that reaches it.
     * Append new fields after this one and set them by name: callers initialize the leading
     * fields positionally (R-309, r7-repro-1). */
    int (*availability)(void *context, size_t scope, const char *name, unsigned char semantics[32],
        char *error, size_t capacity);
    /* Optional borrowing form of the original reader this link is opened with: the same answer for the same
     * path (1 SnapMap, 2 campaign, 3 product, 0 absent, -1 failed), with the bytes borrowed instead of copied.
     * They must stay unchanged, NUL-terminated and valid until the link closes (a per-compile snapshot that
     * outlives it). The link then keeps no copy of a captured original; nothing else changes. *owner, when not
     * NULL, holds those bytes as a shared body (sh_package_body): the captured original takes a reference, so
     * resources that serve it share the bytes instead of copying them, and keep them after the snapshot closes.
     * When that body has a backing, the link stops holding the bytes at its release points (below) and reads
     * them through the body from then on. */
    int (*original_view)(void *context, const char *path, const unsigned char **body, size_t *length,
        sh_package_body **owner);
    void *original_view_context;
    /* Optional, with original_view (R-342). At each release point of the link (the end of its certification, and
     * its freeze) its captured originals whose shared body has a backing (sh_package_body_back) stop holding the
     * bytes and read them only through the body; then this is called, so that the snapshot behind original_view
     * does the same. A body every holder of which reads only through it lets its bytes go until a reader needs
     * them again. A read that cannot reproduce them exactly is recorded on the body and fails the reader that
     * asked as an environment failure. */
    void (*originals_released)(void *context);
    void *originals_released_context;
    /* Optional, with original_view (R-344 stage A): the same for one path, called when the link stops holding that
     * captured original's bytes before a release point. A binary original recorded for replay is no longer read by
     * the discoveries that replay it, so it goes after its first recorded discovery (sh_package_source_link_stream);
     * a later reader reads it again exactly, or fails as an environment failure, as above. */
    void (*original_released)(void *context, const char *path);
    void *original_released_context;
    /* Optional unchanged-scope reuse (R-348; sh_package_source_reuse below). reuse_out receives this capture's
     * record when the capture completes (its earlier content is replaced); reuse_in offers an earlier capture's,
     * read only and borrowed until close. reuse_context: 32 bytes every scope key includes, for what the capture
     * does not state itself (the product and game images, renderer, compiler format); NULL is 32 zero bytes. */
    sh_package_source_reuse *reuse_out;
    const sh_package_source_reuse *reuse_in;
    const unsigned char *reuse_context;
    /* When not NULL, counts the offered bindings this capture bound, also when it then fails; reuse_attempted is
     * set when some scope matched a record (the caller compiles again without reuse should the capture fail). */
    size_t *reuse_bound;
    int *reuse_attempted;
    /* Retained reverse graph (R-386). 1 asserts that every stock original this link reads is a function of the
     * catalog, the native readers and the renderer (production's catalog and installation; never a test baseline):
     * the shape of the stock reverse base the capture discovers is then kept in memo for the next capture with the
     * same catalog, native and renderer identity, which rebuilds it instead of reading the stock closure again
     * (sh_package_source_memo_reverse_export below). 0 reads the stock closure every time. */
    int reverse_retain;
    /* Optional native image availability for an image that state its scope authored names (R-389): the unit is
     * itself supplied, or a unit the scope's packages supply binds it, in a scope whose policy does not open
     * the image gate. 1: the registered native image of that name is an exclusion placeholder; semantics receives
     * what a private copy is certified with, as availability does. 0: nothing to do, also when availability cannot
     * be read: such a scope then binds the stock name exactly as it did without this question. The linker isolates
     * such an image in that scope only, never one a fixed native ABI unit of the scope reaches (it keeps its native
     * binding there, as stock does) and never one listed for in-place recovery. */
    int (*availability_authored)(void *context, size_t scope, const char *name, unsigned char semantics[32],
        char *error, size_t capacity);
} sh_package_source_link_native;
/* 1 when unit index's captured text (derived 0) or its last derived binding
 * (derived 1) bound with undecided native semantics (the source binding's
 * coverage): it is served as authored, never as a decided native state. */
int sh_package_source_link_coverage(const sh_package_source_link *s, size_t index, int derived);

/* Enumerate supplied units, complete their catalog permutations and walk the
 * production typed source binders and registered constructor requests until
 * every captured reference resolves.
 * Each identity resolves in its input compilation, then verified originals.
 * Peer scopes never supply missing files. Original reads/absence are captured
 * once per path and shared as immutable bytes; package bytes are not changed.
 * Campaign-only dependencies absent from the authored scope are not silently
 * imported into a bundle. Unsupported/absent readers fail the whole candidate.
 * Movies additionally bind the finite supplied/reverse-affected sound/material
 * namespace in their scope, independent of ActionScript literal spellings.
 * Conditional edges retain changed complete bindings; certified baseline
 * targets keep native lookup. Movie string bindings join in package_library.
 * Font units capture their available 48/64_df/12 glyph forms and exact material
 * request slots. Optional materials use only this scope and SnapMap originals;
 * confirmed absence is retained by omitting that slot from the complete plan.
 * Read failures never count as absence. Native consumer admission remains
 * separate: the production adapter must not enable unbound constructors.
 *
 * This establishes source closure for the registered readers. Complete implicit
 * dependency enumeration and native ABI/alias admission still belong to the
 * native adapter; this is not
 * permission to rename every shader parameter or engine-created identity.
 * Inputs/native metadata must stay stable through close. No native calls,
 * runtime publication, author metadata changes or filesystem writes occur. */
sh_package_source_link *sh_package_source_link_open(
    const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_link_native *native, char *error, size_t capacity);
typedef struct sh_package_source_root_plan {
    /* One byte per scope. Discover baseline SnapMap palette roots affected by
     * this scope's supplied units; never combine independent source scopes. */
    const unsigned char *discover;
    size_t baseline_scope; /* Existing original-only comparison scope. */
    /* Recorded bindings seed their exact original names in their recorded
     * context indices. This reconstructs derived roots without promoting
     * unused authoring inventory into a document's selected occurrences. */
    const sh_package_provenance *recorded;
    /* The typed values a legacy document actually names. When set, discovery
     * walks these stock roots instead of the SnapMap palette, so a discovering
     * scope nominates only the ones it changes that the document uses. */
    const sh_package_references *candidates;
    /* Optional, one byte per scope (R-344 stage A): 1 when the scope's units proven equal to the stock binding may be
     * bound once in baseline_scope instead of copied (shared stock, sh_package_source_link_share). A caller must not
     * select or edit a shared unit for one scope only (sh_package_source_link_shared_stock): the library gives such a
     * scope its own copies by compiling again without folding it. NULL shares nothing. */
    const unsigned char *fold;
} sh_package_source_root_plan;
sh_package_source_link *sh_package_source_link_open_roots(
    const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_link_native *native, const sh_package_source_root_plan *roots,
    char *error, size_t capacity);
/* Where a failed closure stopped and why. Each discovered unit remembers the
 * unit whose reader first requested it, so a failure carries its causal chain
 * back to the seed that reached it. Only content an input scope supplied can
 * make that scope responsible:
 * SOURCE: the text of the scope's own supplied unit does not parse in the
 *   native grammar (a reader without an adapter for it is COVERAGE).
 * COVERAGE: the scope's supplied content (or a stock root nominated because
 *   of it) reaches a unit the product cannot link: missing reader/admission
 *   coverage, or a dependency SnapMap does not ship and the scope does not
 *   supply. The scope's files are not proven corrupt.
 * PRODUCT: only product built-ins and stock units reach the failing unit.
 * ENVIRONMENT: I/O, allocation, changed native state or setup.
 * scope is the responsible input scope for SOURCE/COVERAGE, else SIZE_MAX. */
typedef enum sh_package_source_failure_kind {
    SH_PACKAGE_SOURCE_FAILURE_NONE,
    SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT,
    SH_PACKAGE_SOURCE_FAILURE_PRODUCT,
    SH_PACKAGE_SOURCE_FAILURE_COVERAGE,
    SH_PACKAGE_SOURCE_FAILURE_SOURCE
} sh_package_source_failure_kind;
typedef struct sh_package_source_failure {
    sh_package_source_failure_kind kind;
    size_t scope;
    char family[64], name[256];
    char chain[512]; /* "family name <- requester <- ... <- seed". */
} sh_package_source_failure;
/* Every scope a capture withdrew for a failure that scope alone answers for
 * (SOURCE or COVERAGE), in discovery order, with the reader's reason. The
 * failure an attributed open reports is the first of them; the others were
 * found in the same capture after it. Recording never changes the capture's
 * outcome. Caller-owned: start zeroed, free with
 * sh_package_source_withdrawals_free. */
typedef struct sh_package_source_withdrawal {
    sh_package_source_failure failure;
    char reason[1024];
} sh_package_source_withdrawal;
typedef struct sh_package_source_withdrawals {
    sh_package_source_withdrawal *items;
    size_t count, capacity;
    /* Admission: the supplied seed units this capture judged before it
     * stopped, when a failure among them ended it ahead of the stock closure
     * they request (that closure would only be discovered again by the next
     * capture); 0 when the capture went on past its seeds. */
    size_t admission_units;
    /* 1 when a failure other than an environment one stopped discovery while it ran scope by scope (shared stock):
     * what was found before the stop depends on that order, so the caller captures again without sharing to report
     * exactly what the capture's creation order finds (R-344 stage A). */
    int stopped;
} sh_package_source_withdrawals;
void sh_package_source_withdrawals_free(sh_package_source_withdrawals *withdrawals);
sh_package_source_link *sh_package_source_link_open_attributed(
    const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_link_native *native, const sh_package_source_root_plan *roots,
    sh_package_source_failure *failure, char *error, size_t capacity);
void sh_package_source_link_close(sh_package_source_link *source);
/* Discovery recordings that outlive the capture that made them, for later
 * captures in this process. A recording keeps a unit's preparation output and
 * ordered reference and request sites, with every file and scoped path its
 * reader read and how each resolved: supplied by the scope, or the capture's
 * original with its status, and its content identity. A later capture replays
 * it only after resolving every one of them afresh to the same content;
 * replay still resolves each reference in its own scope and reruns file
 * selection, material companions and constructor requests. The memo is keyed
 * by what readers consult outside those paths, read afresh by every capture:
 * the catalog rows (sh_resource_catalog_digest), the native managers and
 * reflection (sh_package_source_reader_native_digest), the product's script
 * capture and the preparation callback. A different key drops every
 * recording. Readers of the native shader ABI (renderprog, renderprogflag)
 * are never recorded. Portable data only (strings, identities, byte copies);
 * never persisted. One capture borrows the memo at a time; a capture that
 * finds it busy records only for itself. */
sh_package_source_memo *sh_package_source_memo_open(void);
void sh_package_source_memo_close(sh_package_source_memo *memo);
/* The retained reverse graph (sh_package_source_link_native.reverse_retain) a capture left in memo, encoded for a
 * later process: a sealed record (its SHA-256 over the body is checked on import) whose body names the stock reverse
 * base's units, paths, edges and reader gaps under the catalog, native and renderer key the capture computed.
 * export: malloc-owned bytes, 0 when memo holds none or memory runs out. import: replaces memo's graph with the
 * record's when the record decodes and its seal holds (1); a truncated, altered or foreign record changes nothing
 * (0). key: the graph's 32-byte key and unit count, 0 when memo holds none. clear: drops the graph. The caller
 * validates the record's provenance (product, game and renderer) before offering it: the key states what the
 * capture read, not which product read it. */
int sh_package_source_memo_reverse_export(const sh_package_source_memo *memo, unsigned char **bytes, size_t *length);
int sh_package_source_memo_reverse_import(sh_package_source_memo *memo, const unsigned char *bytes, size_t length);
int sh_package_source_memo_reverse_key(const sh_package_source_memo *memo, unsigned char key[32], size_t *units);
void sh_package_source_memo_reverse_clear(sh_package_source_memo *memo);
/* Unchanged-scope reuse (R-348). A capture given reuse_out records, when it completes, what each package scope that
 * folds shared stock bound instead of its own copies, in the pass it bound it: the family and name, a digest of the
 * bound unit's fold structure (files and witnesses by path and resolution, discovered files, sites, dependencies
 * and requests by target name, state-machine configuration, flags) and the paths its copy's discovery consulted. A
 * scope's key covers its exact compiled content and attributes, the capture's catalog and native readers, its
 * sharing modes and the caller's reuse_context. A later capture given reuse_in discovers a scope with an equal key
 * as usual, except that each recorded binding is offered to it in its pass: one whose stock unit the capture's
 * baseline still has with the same digest, whose consulted paths neither the baseline nor the scope supplies, whose
 * certificate holds for this scope now (asked as the fold asks it) and whose whole closure is offered too is bound
 * when the scope first requests it, instead of discovering, folding and certifying a copy. Everything else the scope
 * reaches is discovered and folded as usual. Records hold strings and digests only. */
sh_package_source_reuse *sh_package_source_reuse_open(void);
void sh_package_source_reuse_close(sh_package_source_reuse *reuse);
/* dst takes src's content (dst's own is freed first); src is left empty. */
void sh_package_source_reuse_move(sh_package_source_reuse *dst, sh_package_source_reuse *src);
size_t sh_package_source_reuse_scope_count(const sh_package_source_reuse *reuse);
/* Settled groups (R-348): the library's keys of editor contribution groups (package_library.c,
 * pl_group_settled_key), each with what the group staged as the library encodes it (outputs; none when it staged
 * nothing). A later compile whose group has an equal key settles it again from them without composing. add copies
 * outputs (0 on allocation failure); settled: 1 when the record holds key, with its outputs borrowed until the
 * record closes. Encoded sorted and unique by key. */
int sh_package_source_reuse_add_settled(sh_package_source_reuse *reuse, const unsigned char key[32],
    const unsigned char *outputs, size_t length);
int sh_package_source_reuse_settled(const sh_package_source_reuse *reuse, const unsigned char key[32],
    const unsigned char **outputs, size_t *length);
size_t sh_package_source_reuse_settled_count(const sh_package_source_reuse *reuse);
/* Identity templates (R-348): the linker's record of a compile's capture templates (sh_package_link_reuse_templates_
 * encode), kept with the record of the compile that made them and never interpreted here. set takes bytes (malloc-
 * owned) into a record a compile writes, templates lends them back. A record loaded from a file instead holds that
 * sealed file (attach retains it) and where in it they are, so a loaded record never holds them in memory; read gives
 * a malloc-owned copy of them from either (1 read, 0 none, -1 failure). */
void sh_package_source_reuse_set_templates(sh_package_source_reuse *reuse, unsigned char *bytes, size_t length);
const unsigned char *sh_package_source_reuse_templates(const sh_package_source_reuse *reuse, size_t *length);
void sh_package_source_reuse_attach_templates(sh_package_source_reuse *reuse, sh_package_file *file, uint64_t offset,
    uint64_t length);
int sh_package_source_reuse_read_templates(const sh_package_source_reuse *reuse, unsigned char **bytes, size_t *length);
size_t sh_package_source_reuse_binding_count(const sh_package_source_reuse *reuse);
/* The memory a record holds (its tables' capacity and its strings), in bytes. */
size_t sh_package_source_reuse_bytes(const sh_package_source_reuse *reuse);
/* A record as portable bytes, and back: decoding checks every count and index against what remains and the table it
 * names (1 decoded, 0 malformed, -1 memory); *out is NULL unless 1. */
int sh_package_source_reuse_encode(const sh_package_source_reuse *reuse, unsigned char **bytes, size_t *length);
int sh_package_source_reuse_decode(const unsigned char *bytes, size_t length, sh_package_source_reuse **out);
typedef struct sh_package_source_reuse_counters {
    size_t scopes_keyed, scopes_recorded, bindings_recorded, stock_recorded;
    size_t scopes_offered, scopes_matched, scopes_restored;
    size_t bindings_offered, bindings_valid, bindings_bound, copies_dropped, certified;
    /* Offered bindings refused, by reason: own unit, no stock unit, structure, family, changed digest, consulted
     * inputs, closure, certificate. */
    size_t refused[8];
    unsigned long long key_us, validate_us, record_us;
    /* Memory: the process's private bytes across keying (MB, 0 without a meter), the judging, restoring and recording
     * state released at the freeze and the marks within it, the offered record and the one recorded (bytes). */
    long long key_private_mb;
    size_t working_bytes, mark_bytes, offered_bytes, record_bytes;
} sh_package_source_reuse_counters;
void sh_package_source_link_reuse_counters(const sh_package_source_link *s, sh_package_source_reuse_counters *out);
/* Reverse discovery stops at stock sources beyond the registered readers
 * instead of refusing every package. scope SIZE_MAX names the stock unit whose
 * reader stopped; any other scope changed a stock root whose closure crosses
 * such a unit, so no derived variant was exposed for it. Unreadable originals
 * and allocation failures still fail the candidate. variant marks a stock
 * module card that scope changes outside its blueprint models while no
 * installed module has the card: it is reached by its module's name, so no
 * module variant can carry it, and it is not nominated.
 * Borrowed until close. */
typedef struct sh_package_reverse_gap {
    size_t scope;
    const char *family, *name, *reason;
    int variant;
} sh_package_reverse_gap;
size_t sh_package_source_link_reverse_gaps(const sh_package_source_link *source,
    const sh_package_reverse_gap **gaps);
/* Why the link accounted an authored shader input without publishing it for
 * the renderer it served. These are the only shader inputs it accepts without
 * a linked owner; anything else fails the link. */
typedef enum sh_package_shader_unpublished_reason {
    SH_PACKAGE_SHADER_UNPUBLISHED_OTHER_RENDERER = 1, /* the other renderer's binary or SPIR-V */
    SH_PACKAGE_SHADER_UNPUBLISHED_DRIVER_CACHE,       /* generated/glsl or glslbinaries */
    SH_PACKAGE_SHADER_UNPUBLISHED_STOCK_INCLUDE,      /* equals the SnapMap include; no program read it */
    SH_PACKAGE_SHADER_UNPUBLISHED_DORMANT_PAIR        /* checksum matches its other-renderer binary */
} sh_package_shader_unpublished_reason;
/* One accounted input in its scope. pair and checksum are the verified binary
 * of a dormant pair, otherwise NULL and 0. renderer is the served
 * sh_package_shader_renderer, or -1 when the link had no shader contract. */
typedef struct sh_package_shader_unpublished {
    size_t scope;
    const sh_compiled_resource *file, *pair;
    sh_package_shader_unpublished_reason reason;
    int renderer;
    uint32_t checksum;
} sh_package_shader_unpublished;
/* Borrowed until close. Records are diagnostics: when one could not be kept,
 * *lost is set and the link is otherwise unaffected. */
size_t sh_package_source_link_shader_unpublished(const sh_package_source_link *source,
    const sh_package_shader_unpublished **records, int *lost);
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
/* Fail the next count record allocations as an exhausted heap would. */
void sh_package_source_link_test_unpublished_failures(long count);
#endif
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Refuse the nth interned spelling and the nth prepared stand-in a capture would keep (0: none) as an exhausted heap
 * would; _left reports the countdowns no capture reached. borrow 0 captures originals as owned copies even when a
 * borrowing reader (original_view) is given, the path of a link opened without one. */
void sh_package_source_link_test_refuse(long intern, long prepared);
void sh_package_source_link_test_refusals_left(long *intern, long *prepared);
void sh_package_source_link_test_borrow(int enabled);
/* status 0: the capture's readers copy an original to learn only its status, as before R-342
 * (sh_package_source_reader_original_status); queries counts the status-only answers given. */
void sh_package_source_link_test_status(int enabled);
size_t sh_package_source_link_test_status_queries(void);
/* Every block of the discovery-list arenas holds bytes (0: the default growth, 64 KiB doubling to 4 MiB, or the
 * list's own size when larger), and the nth arena block or frozen site block is refused (0: none) as an exhausted
 * heap would; _left reports the countdown no capture reached. */
void sh_package_source_link_test_blocks(size_t bytes, long refuse);
long sh_package_source_link_test_blocks_left(void);
/* Refuse the nth lending of a frozen unit table (0: none) as an exhausted heap would; _left reports the countdown no
 * capture reached. */
void sh_package_source_link_test_take_refusal(long nth);
long sh_package_source_link_test_take_refusal_left(void);
/* Refuse the nth kept copy of a lent table's row (0: none) as an exhausted heap would (R-343); rows the capture
 * compared with their frozen copies (when keeping a copy, and at close for rows it kept none of) and those found changed,
 * since the process started (a changed row means the borrower edited it without keeping the capture's copy first),
 * and the copies kept. */
void sh_package_source_link_test_preserve_refusal(long nth);
long sh_package_source_link_test_preserve_refusal_left(void);
void sh_package_source_link_test_rows(size_t *checked, size_t *changed, size_t *kept);
/* Strict (the default): a capture that finds such a changed row at close aborts the test process. */
void sh_package_source_link_test_strict_rows(int strict);
/* Refuse the nth optional unit record a capture allocates (0: none; R-343): its writer fails the capture as an
 * environment failure, or, for a diagnostic note, keeps none; _left reports the countdown no capture reached. */
void sh_package_source_link_test_extra_refusal(long nth);
long sh_package_source_link_test_extra_refusal_left(void);
/* Grow five interleaved lists in discovery arenas of several block sizes against shadow copies: 1, or 0 with the
 * first difference. Every freeze also compares each discovered file's frozen site range with the scan of its
 * discovery sites; _site_ranges reports how many were compared and how many differed since the process started. */
int sh_package_source_link_test_arena(char *error, size_t capacity);
void sh_package_source_link_test_site_ranges(size_t *checked, size_t *mismatched);
/* Units whose discovery state the freezes since the process started released, and freezes that left a unit's state
 * or one of its discovery arenas behind (R-341). */
void sh_package_source_link_test_discovery(size_t *released, size_t *kept);
#endif
/* ---- Shared stock (R-344 stage A) -------------------------------------------
 * A package scope whose root plan allows it (sh_package_source_root_plan.fold) binds its units proven equal to the
 * stock binding once, in the baseline scope: after each discovery pass of that scope, a unit whose native
 * certificate holds in that scope and in the baseline, whose discovery read only captured originals the baseline
 * resolves alike, and whose whole closure (every dependency, site, implicit and conditional request, cycles included)
 * is such a unit or already shared, is replaced by the baseline's equal unit, or becomes that baseline unit (only when
 * the native implicit adapter, which may answer per scope, requested nothing for it). The
 * scope's other units then bind the baseline unit's index. Units a scope's later contributions usually select or edit
 * (editor collections, weapons and hands webs, catalog cards, AI registrations and the entities they name,
 * registration-key sites) stay each scope's own, as does anything reaching them; render programs, fonts and cooked
 * movies, which have capture paths of their own, are never shared. A frozen shared unit is in the baseline scope:
 * find_unit of the baseline does not return one no baseline unit reaches (stock_only), and resolve_unit returns one
 * only for a unit of a scope that shares it. Outputs are those of the capture without sharing. mode 0 captures every
 * scope's copies (the oracle), 1 shares (the default), 2 decides as 1 but keeps every copy and records each one's
 * representative (sh_package_source_link_test_share_rep). */
void sh_package_source_link_share(int mode);
/* 1 when frozen unit index is a shared stock unit no baseline unit reaches: not a member of the baseline scope. */
int sh_package_source_link_stock_only(const sh_package_source_link *source, size_t index);
/* 1 when frozen unit index is bound once for at least one package scope: an edit for one scope must not change it. */
int sh_package_source_link_shared_stock(const sh_package_source_link *source, size_t index);
/* A package scope's occurrences of shared stock: the frozen baseline units it binds instead of its own copies, sorted
 * by family and name (count, the ith unit index), and the one of a family and name (1 found, with *unit). */
size_t sh_package_source_link_scope_shared(const sh_package_source_link *source, size_t scope);
size_t sh_package_source_link_scope_shared_unit(const sh_package_source_link *source, size_t scope, size_t i);
int sh_package_source_link_scope_find(const sh_package_source_link *source, size_t scope, const char *family,
    const char *name, size_t *unit);
/* What frozen unit index's own captured text states, read once per captured original declaration: registration-key
 * sites (*keys 1 some, 0 none or not a key declaration, -1 unreadable) and AI registration lists (*conductor 1, 0,
 * -1 unreadable). 0 with both -1 when it cannot be read or kept. */
int sh_package_source_link_stock_facts(sh_package_source_link *source, size_t index, int *keys, int *conductor);
/* Diagnostics and tests: 0 keeps every binary original recorded for replay held until the release points, as before
 * stage A; 1 (the default) lets it go after its first recorded discovery. */
void sh_package_source_link_stream(int enabled);
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Mode 2: the frozen index of the unit whose binding unit index would share, or SIZE_MAX when it would not be shared
 * (its own index when it would become the stock unit). */
size_t sh_package_source_link_test_share_rep(const sh_package_source_link *source, size_t index);
/* Unchanged-scope reuse (R-348): refuse the nth allocation or binding a restore makes (0: none) as an exhausted heap
 * would, after the bindings before it were admitted; _left reports the countdown no capture reached. Mutation
 * controls drop validations of offered bindings, a bit each (1 << n: 1 digest, 2 consulted inputs and file
 * selection, 3 closure, 4 certificate, 5 own units, 6 native requests, 7 all of them: the record trusted; 0 none),
 * which some differential must then catch. */
void sh_package_source_link_test_reuse_refusal(long nth);
long sh_package_source_link_test_reuse_refusal_left(void);
void sh_package_source_link_test_reuse_mutation(int clause);
/* 0 lets shared stock fold the units a scope's later contributions select or edit by scope (editor collections,
 * weapons, hands webs, catalog cards, AI registrations and the entities they name, key sites), so the library's
 * boundary must fall back for them; 1 (the default) keeps them each scope's own. */
void sh_package_source_link_test_share_conservative(int enabled);
/* Mutation controls: 0 none; drop one clause of shared stock: 1 the closure, 2 the input rule (files, lookups and
 * native requests), 3 structural equality, 4 the native certificate, 5 the first-copy placement, 6 the conductor
 * exclusion, 7 the state-machine selection rule, 8 the reverse closure of a privatization (only its triggers get
 * copies). A suite-wide run selects one with SMP_SHARE_MUTATION s1-s8. */
void sh_package_source_link_test_share_mutation(int clause);
/* Restore the suite's modes: the defaults, or those the environment chose for the whole process (SMP_SHARE_MODE 0-2,
 * SMP_SHARE_CONSERVATIVE, SMP_STREAM, SMP_SHARE_MUTATION; read before the first capture or by this call). */
void sh_package_source_link_test_share_defaults(void);
/* Process totals of the captures closed so far: captures, units decided, joined a baseline unit, became one, given
 * back by privatizations, streamed originals, reads again of let-go originals. */
void sh_package_source_link_test_share_totals(size_t totals[7]);
/* Enabling (re)starts a log of the occurrences of shared stock each frozen capture binds (mode 2: would bind), one line
 * "<package name>\t<family>\t<name>" each; _logged returns it ("" when none). */
void sh_package_source_link_test_shared_log(int enabled);
const char *sh_package_source_link_test_shared_logged(void);
/* Enabling (re)starts the canonical text of each frozen capture (units in frozen order with their scopes, files,
 * edges, certificates and flags, then each scope's share map): a restored capture compares exactly with a fresh one. */
void sh_package_source_link_test_capture_dump(int enabled);
const char *sh_package_source_link_test_capture_dumped(void);
#endif
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Refuse the nth allocation the shared-stock decisions make (0: none) as an exhausted heap would; _left reports the
 * countdown no capture reached. Force the next fold of scope (SIZE_MAX: none) to privatize family/name afterwards, as
 * a later trigger would. */
void sh_package_source_link_test_share_refusal(long nth);
long sh_package_source_link_test_share_refusal_left(void);
void sh_package_source_link_test_privatize(size_t scope, const char *family, const char *name);
/* Shared stock rows the capture found edited at close beyond their fixed and stable names (strict: abort). */
void sh_package_source_link_test_shared_rows(size_t *checked, size_t *changed);
/* Offer record scope `to` (in recorded order) the binding scope `from` records for family and name too, in pass
 * `pass` (0: the same pass). */
int sh_package_source_reuse_test_copy_binding(sh_package_source_reuse *reuse, size_t from, const char *family,
    const char *name, size_t to, unsigned pass);
/* A record as text (scopes with key prefixes, bindings by pass, family, name and digest prefix); malloc-owned. */
char *sh_package_source_reuse_test_text(const sh_package_source_reuse *reuse);
/* The i-th settled entry (a decoded record holds them by key) and its outputs' length; 0 past the last. */
int sh_package_source_reuse_test_settled_entry(const sh_package_source_reuse *reuse, size_t i, unsigned char key[32],
    size_t *length);
#endif
/* Borrowed, captured source units, sorted by scope/family/name; with shared stock, a shared unit sorts at the
 * earliest package scope that binds it (its first copy's place without sharing), so scopes need not ascend: find a
 * unit with sh_package_source_link_find_unit or sh_package_source_link_scope_find, not by search over this order. */
const sh_package_link_inputs *sh_package_source_link_inputs(const sh_package_source_link *source);
/* The frozen capture's own record of unit index, borrowed until close: equal to its inputs entry at the freeze and
 * unchanged after it, also once the inputs table was handed over. NULL when not frozen or out of range. */
const sh_package_link_unit *sh_package_source_link_unit(const sh_package_source_link *source, size_t index);
/* Lend the frozen unit table to a caller that extends and edits it (the library's contribution phase; R-343: the
 * capture keeps owning it and frees it at close). The first count rows are the captured units, the rest up to capacity
 * zero; a later call grows it to capacity, which may move it (rows keep their indices). Afterwards the capture's inputs
 * keep their count and scopes but no units, and its own identify and compile entry points refuse. The borrower edits a
 * captured row only after sh_package_source_link_preserve_unit kept the capture's copy of it, so that
 * sh_package_source_link_unit answers each record as it was frozen. NULL, with nothing changed, when the capture is not
 * frozen, capacity is below the count, or the table cannot grow. */
sh_package_link_unit *sh_package_source_link_lend_units(sh_package_source_link *source, size_t capacity);
/* Keep the capture's own copy of captured row index before the borrower of the unit table edits it: 1 kept (also
 * when already kept, or for an appended row), 0 when the copy cannot be allocated (the row must then stay as it is). */
int sh_package_source_link_preserve_unit(sh_package_source_link *source, size_t index);
/* Borrowed until close: the image names the availability callback reported as exclusion
 * placeholders that a fixed native ABI unit of a gate-opening scope reaches, sorted and unique.
 * They keep their original bindings; the publication recovers the native objects in place. */
size_t sh_package_source_link_recovered(const sh_package_source_link *source, const char *const **names);
/* Write "readers N ms, seeds N ms, ..." for the capture that opened source;
 * returns out. A diagnostic only. */
const char *sh_package_source_link_phases(const sh_package_source_link *source, char *out, size_t capacity);
/* Process private/working-set MB at each capture phase end (sh_package_memory_read); diagnostics only. */
const char *sh_package_source_link_memory(const sh_package_source_link *source, char *out, size_t capacity);
/* Diagnostic counts of the capture, as one line: units discovered and
 * replayed from another scope's discovery, file binds, original captures. */
const char *sh_package_source_link_counters(const sh_package_source_link *source, char *out, size_t capacity);
/* The payload identity of one file of source unit index, as
 * sh_package_compilation_identity computes it for that unit's compilation;
 * an unsupplied original's is computed once for every unit that binds it. */
int sh_package_source_link_identity(sh_package_source_link *source, size_t index,
    const sh_compiled_resource *file, sh_package_file_identity *out, char *error, size_t capacity);
/* Tests and diagnostics: discovery replay of unchanged units is on by
 * default; 0 binds every unit afresh (the same result, more slowly). */
void sh_package_source_link_replay(int enabled);
/* Capture-local supplied lookups and payload identities only. Canonicalizer
 * calls and original/native validation still run. 0 is the diagnostic oracle. */
void sh_package_source_link_lookup_memo(int enabled);
typedef struct sh_package_source_lookup_counters {
    size_t lookups, searches, lookup_hits;
    size_t marked; /* lookups by a path already proven its own key (no canonicalization) */
    size_t identities, identity_computations, body_hashes, identity_hits;
    size_t entries, bytes, peak_bytes, limit;
    size_t original_captures;
    unsigned long long original_bytes, original_capture_us;
    unsigned long long replay_validation_us, replay_validation_without_capture_us;
} sh_package_source_lookup_counters;
void sh_package_source_link_lookup_counters(const sh_package_source_link *source,
    sh_package_source_lookup_counters *out);
#ifdef SH_PACKAGE_LIBRARY_TESTING
/* Optional-storage failure/budget and exact-key collision oracles. */
void sh_package_source_link_test_lookup(size_t limit, long allocation_failures, int collisions);
#endif
/* Tests only: 0 captures as before AI state-machine selection (no behavior
 * edges), reproducing the identities a pre-selection build recorded. */
void sh_package_source_link_fsm(int enabled);
/* Diagnostics: 1 when input unit index was discovered by replay. */
int sh_package_source_link_unit_replayed(const sh_package_source_link *source, size_t index);
/* One byte per input unit: its complete captured dependency closure has no
 * gameplay contribution. Editor collection composition uses this eligibility;
 * an editor resource reaching private gameplay must follow that asset scope.
 * Zero-file originals and product-only sources contribute no gameplay owners.
 * This is NOT native-original certification or a map-embedding decision: native
 * implicit state and actual map usage remain separate. Borrowed until close. */
const unsigned char *sh_package_source_link_shared(const sh_package_source_link *source);
/* 1 when unit index is a module catalog card whose binding changed only
 * through its blueprint floor models, which only the editor reads: one
 * presentation slot under its native name, which the library resolves across
 * scopes. */
int sh_package_source_link_presentation(const sh_package_source_link *source, size_t index);
/* Diagnostics: why the unit's own binding is not certified original, or "". */
const char *sh_package_source_link_original_note(const sh_package_source_link *source, size_t index);
/* Assignment-free binding view for shared collection composition. These use
 * exactly the frozen source snapshot and unit indices returned by inputs().
 * bind_unit may inspect a derived body, but its inherited class/schema lookup
 * remains in that unit's authored scope. Cross-scope entries must retain their
 * captured target indices when subsequently replayed; their spelling is not a
 * request to resolve again in the destination scope. Owned output bytes. */
int sh_package_source_link_identify(sh_package_source_link *source,
    sh_package_link_classes *out, char *error, size_t capacity);
int sh_package_source_link_resolve_unit(sh_package_source_link *source, size_t unit,
    const char *family, const char *name, size_t *target, char *error, size_t capacity);
/* The frozen unit with this family and name in one input scope, if any. */
int sh_package_source_link_find_unit(sh_package_source_link *source, size_t scope,
    const char *family, const char *name, size_t *unit);
/* The layer configuration a frozen aibehavior unit names (its expanded
 * edit.aiFSMManagerDeclLayersConfig), or NULL: none, or not a behavior. */
const char *sh_package_source_link_fsm_config(const sh_package_source_link *source, size_t unit);
/* 1 when the catalog lists this SnapMap state-machine declaration. */
int sh_package_source_link_fsm_stock(const sh_package_source_link *source, const char *name);
unsigned char *sh_package_source_link_bind_unit(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_binding_resolver resolver,
    void *context, size_t *output_length, char *error, size_t capacity);
/* Typed idStrId sites use the same frozen source/inheritance scope. This pass
 * neither treats prose as a key nor changes resource dependency collection. */
unsigned char *sh_package_source_link_bind_strings(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_string_site_resolver resolver,
    void *context, size_t *output_length, char *error, size_t capacity);
/* Registration-key sites of one captured unit, in its own scope; see
 * sh_package_source_bind_keys for the status contract. */
int sh_package_source_link_bind_keys(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_key_site_resolver resolver, void *context,
    unsigned char **output, size_t *output_length, char *error, size_t capacity);
int sh_package_source_link_key_space(sh_package_source_link *source, struct sh_package_key_space *space);
/* 1 when the stock player the engine spawns for co-op (the rules entity's
 * playerSpawnDef, default player/mp) names this animation web in
 * idPlayer::handsAnimWeb or _D1: the one global web every weapon's subweb is
 * resolved in. */
int sh_package_source_link_player_hands(const sh_package_source_link *source, const char *name);
/* -1: each compile decides the player hands contract from its own scopes; 0 or 1 pins it (R-397). */
void sh_package_source_link_pin_player_contract(int supplied);
/* The same stock contract read through a baseline reader alone: the names of
 * the hands webs (malloc-owned, possibly none). 0 when a stock entity it needs
 * is unreadable. */
int sh_package_player_hands_webs(sh_package_baseline_reader original, void *context,
    char ***names, size_t *count);
/* Physical members of multi-file units use their captured role. In particular,
 * shader source, cooked records and stage modules are different formats. */
unsigned char *sh_package_source_link_bind_file(sh_package_source_link *source, size_t unit,
    const sh_compiled_resource *file, const unsigned char *body, size_t length,
    sh_package_binding_resolver resolver, void *context, size_t *output_length,
    char *error, size_t capacity);
/* The reference sites that discovery bound in one captured file of a unit,
 * in bind order, as (slot, target unit) pairs: exactly the sites and frozen
 * resolutions bind_file reports for that file, since discovery ran the same
 * reader over the same bytes. 1 with the sites (possibly none), 0 when this
 * link did not discover that file. Valid until the link closes. */
int sh_package_source_link_sites(const sh_package_source_link *source, size_t unit,
    const sh_compiled_resource *file, const sh_package_link_edge **sites, size_t *count);
/* Enumerate a complete captured shader unit under a prospective root identity.
 * PERMUTATION entries are native renderprog identities, not provider paths.
 * The allocator must reserve/check every such ID along with the root before
 * publication. Other entries map original member paths to linked paths. */
int sh_package_source_link_shader_paths(sh_package_source_link *source, size_t unit,
    const char *linked_name, sh_package_shader_path_visitor visitor, void *context,
    char *error, size_t capacity);
unsigned char *sh_package_source_link_bind_derived(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_binding_resolver resolver,
    void *context, sh_package_bound_exists exists, void *exists_context,
    size_t *output_length, char *error, size_t capacity);
/* resolve/bind use the retained scoped readers and frozen original snapshot.
 * The supplied native assignment establishes valid aliases, including implicit
 * ABI. Its registry callback checks native ID/name occupation as aliases are
 * allocated; that callback retains its own context through this call.
 * Output is independent of this builder after a successful link. */
int sh_package_source_link_compile(sh_package_source_link *source,
    const sh_package_sources *inventory, const sh_package_link_adapter *native,
    const sh_package_policy *policy, char *(*canonical_path)(const char *),
    sh_package_link_output *out, char *error, size_t capacity);
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
/* Tests: base-unit reverse-change decisions compared with the per-unit check,
 * and how many differed. */
void sh_package_source_link_test_reverse(size_t *checked, size_t *mismatches);
/* Tests: retained reverse graphs captured into a memo, stock reverse bases rebuilt from one, and rebuilt bases
 * whose fresh discovery (run beside them while the oracle is on, the default) encoded differently. */
void sh_package_source_link_test_reverse_graph(size_t *captured, size_t *reused, size_t *mismatches);
void sh_package_source_link_test_reverse_graph_oracle(int enabled);
#endif
#endif
