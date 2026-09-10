/* Patch entityDef.state.edit paths in compact engine JSON without building a tree.
 * On failure, discard out and do not schedule an apply. These helpers validate
 * JSON token grammar and nesting, without interpreting Unicode or duplicate keys. */
#ifndef JSON_PATCH_H
#define JSON_PATCH_H

#define SH_JSON_PATCH_MAX_DEPTH 64

/* Set a scalar using an already encoded JSON token; no escaping is applied.
 * Create missing intermediate objects. Return 1 with a full patched document,
 * or 0 on shape/size failure, leaving out unusable. */
int sh_json_patch_set_leaf(const char *full_json, const char *prop_path,
                            const char *raw_leaf_token, char *out, int outcap);

/* Append raw IDs to a num/item[] reference list, escaping each new value and
 * preserving existing entries. Create a list if absent. Recognized duplicate
 * strings are skipped; unsupported escapes may compare unequal. Return 1 with
 * the patched document or 0 on failure. */
int sh_json_patch_upsert_reflist(const char *full_json, const char *prop_path,
                                  const char * const *id_strings, int n_ids,
                                  char *out, int outcap);

/* Quote and JSON-escape raw text. Return 1 on success, 0 on overflow. */
int sh_json_quote_string(const char *raw, char *out, int cap);

#endif /* JSON_PATCH_H */
