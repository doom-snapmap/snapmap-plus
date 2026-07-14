/* json_patch.h -- a minimal, purpose-built JSON object mutator for entityDef.state.edit.<dotted path>
 * on the compact, machine-generated JSON the engine's own TreeRenderJson emits (serialize_entity /
 * +0xc8). NOT a general JSON library -- the backend deliberately carries none (see apply_engine.c's
 * ae_splice_targets, whose "insert item[N] before num, bump num" technique this module generalizes to
 * an arbitrary dotted path + reuses for the dedup-append case).
 *
 * Every function fails CLOSED: a shape mismatch, missing field, or buffer-cap overflow returns 0 and
 * writes nothing meaningful to *out* -- callers must treat 0 as "no patch, entity untouched" and never
 * schedule an apply on a 0 return. This is the FRONTEND's half of the split (serialize -> patch here ->
 * schedule deserialize+commit); ae_apply_one's own SEH-guarded deserialize is a second, independent
 * safety net if a patch result is ever subtly malformed despite this.
 *
 * Clean-room: our own design, built on the proven raw-splice technique already shipping in
 * apply_engine.c's ae_splice_targets. Zero OG bytes.
 */
#ifndef JSON_PATCH_H
#define JSON_PATCH_H

/* Set a SCALAR leaf at entityDef.state.edit.<dotted prop_path> to raw_leaf_token -- an ALREADY-ENCODED
 * JSON value token (a quoted+escaped string literal, a bare number, or bare true/false; NEVER
 * re-escaped or re-parsed by this function, so the caller controls the exact engine-format text, e.g.
 * a float that must keep its trailing ".0"). Missing intermediate objects along the path are created
 * fresh. Returns 1 + out (a full patched copy of full_json) on success, 0 on any shape/overflow
 * failure (out is undefined on failure -- do not use it). */
int sh_json_patch_set_leaf(const char *full_json, const char *prop_path,
                            const char *raw_leaf_token, char *out, int outcap);

/* Upsert a num/item[] reference LIST at entityDef.state.edit.<dotted prop_path>: if a list already
 * exists there (has a "num" key), its existing item[0..num-1] entries are KEPT VERBATIM and only the
 * id_strings NOT already present are appended (so re-targeting an already-targeted id is a no-op, never
 * a duplicate); otherwise a fresh list is created. id_strings are RAW (unescaped) id-strings -- this
 * function JSON-escapes them internally. Returns 1 + out on success, 0 on any shape/overflow failure. */
int sh_json_patch_upsert_reflist(const char *full_json, const char *prop_path,
                                  const char * const *id_strings, int n_ids,
                                  char *out, int outcap);

/* JSON-escape `raw` into a quoted string literal token (surrounding quotes included) -- exposed so
 * callers building a raw_leaf_token (e.g. the bss string-set op) don't need their own escaper. Returns
 * 1 + out on success, 0 on overflow. */
int sh_json_quote_string(const char *raw, char *out, int cap);

#endif /* JSON_PATCH_H */
