/* strids.h -- the custom #str_ STRING INJECTOR, native C
 * (port of OG's strids detour FUN_1800102e0 / FUN_18000FF10).
 *
 * SnapHak lets custom maps/prefabs reference custom localized strings: it injects #str_<id> -> text
 * mappings from %USERPROFILE%\snaphak\strings\strids.json into the engine's runtime string-id table
 * (idLangDict), so the engine resolves those custom ids just like its own built-in localized strings.
 *
 * MECHANISM (DIRECT, RE of the OG XINPUT1_3.dll + the engine, this campaign):
 *   OG detours the engine idLangDict radix-sort wrapper `0x1A2B480` with FUN_1800102e0, whose body is:
 *       FUN_18000FF10(engineBase + 0x55B7F18);                 // INJECT: load strids.json, append rows
 *       (engineBase + 0x1A2B490)(ctx, table[0], count, 0x20);  // tail-call the ORIGINAL sort
 *   FUN_18000FF10 reads strings/strids.json, parses {id: text}, and for each row builds a 32-byte
 *   idLangDict entry { u32 hash; u32 pad; void* keyHandle; void* valHandle; u32 keyLen; u32 valLen }
 *   where hash = FNV-1a(lower("#str_<id>")) and the two handles are interned idStr pool handles, then
 *   appends the row to the table descriptor at engineBase+0x55B7F18 via the engine idList::Append.
 *
 *   The engine's own native lang loader (FUN_141a2a050) builds the IDENTICAL 32-byte record + calls the
 *   same Append (FUN_141a29980) + the same hash (FUN_141a29b90) + the same idStr ctor (FUN_141a03e10) --
 *   so SnapHak's injector is a faithful clone of the engine's string-table population, sourced from
 *   strids.json and ADDED to (not replacing) the engine's table.
 *
 * NATIVE PORT (the difference from OG):
 *   - We detour the engine sort BODY `0x1A2B490` (the wrapper `0x1A2B480` is only ~8 bytes -- too small
 *     for our 14-byte abs-jmp installer; the wrapper JMPs straight into the body, so a body detour fires
 *     for the same engine sort). A recursion+one-shot latch makes the inject fire exactly ONCE, on the
 *     first top-level sort, then every call (incl. the sort's own recursion) passes straight through.
 *     OG re-injects on every sort (it has no latch); we inject once to avoid duplicate rows, which has
 *     the same net effect because the strids table is loaded+sorted once at startup.
 *   - We resolve every engine fn by the masked-byte signature scanner (no hardcoded RVAs), and we
 *     resolve the table-descriptor `.data` global build-portably by decoding the `LEA RCX,[rip+disp32]`
 *     inside the resolved idLangDict::GetIndexForId (StridsTableLea) -- NOT a hardcoded 2021 offset.
 *   - We MUST call the engine's idStr ctor (it interns into an engine-private string pool) + the engine
 *     Append (it grows the engine-owned idList) -- those can't be reimplemented client-side. The JSON
 *     parse + the "#str_<id>" build + the FNV-1a record-hash we do natively (the engine hash fn is also
 *     resolved + used so the key field matches the engine's lookup exactly).
 *
 * Clean-room: ported from our own RE (above). Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B1_STRIDS_H
#define BACKEND_B1_STRIDS_H

#include <stdint.h>
#include <stddef.h>

/* Install the strids injector: detour the engine idLangDict sort body so the first top-level sort first
 * appends our #str_ rows from strings/strids.json into the live string table, then runs the real sort.
 *
 *   sort_body_fn   = resolved engine StridsSortBody (0 => not resolved; logs SKIPPED, returns 0).
 *   sort_status_ok = 1 iff a CLEAN scan hit (SIG_OK), not the hook-tolerant known_rva fallback
 *                    (SIG_OK_HOOKED). When the prologue is already inline-hooked we refuse (installing
 *                    over a detour would steal detour bytes) -- same policy as the rawmap LOAD-swap.
 *   table_lea_fn   = resolved engine StridsTableLea (carries the LEA to the table global; 0 => SKIPPED).
 *   insert_fn      = resolved engine StridsInsert (idList::Append; 0 => SKIPPED).
 *   hash_fn        = resolved engine StridsHash (idStr::Hash FNV-1a; 0 => SKIPPED).
 *   idstr_ctor_fn  = resolved engine idStr pool ctor (DB name "IdStrAssign", rva 0x1a03e10; 0 => SKIPPED).
 *
 * Returns 1 if the detour was installed, 0 otherwise (logs the reason). Emits a "B1: strids injector
 * installed ..." marker on success; the actual inject + count is logged on the first sort ("B1: strids
 * injected N #str_ entries"). */
int sh_strids_install(void *sort_body_fn, int sort_status_ok,
                      void *table_lea_fn, void *insert_fn, void *hash_fn, void *idstr_ctor_fn);

/* Set the strids.json source path (default %USERPROFILE%\snaphak\strings\strids.json -- OG's path). NULL
 * resets to default. Returns 1 if a path is set. */
int sh_strids_set_source(const char *path);

/* How many #str_ rows the injector has appended (observability for the manager's differential test). */
unsigned long sh_strids_injected_count(void);

#endif /* BACKEND_B1_STRIDS_H */
