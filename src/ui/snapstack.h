/* snapstack.h -- the SnapStack STORES (the DECISION half) + the 20-subcommand registrar.
 *
 * A clean-room, FAITHFUL port of the reference implementation (the proven mechanism). Hosts:
 *   - the STACK-OF-STACKS: std::vector<std::vector<int>> -- OG DAT_180031800 (a vec-of-vec, stride 0x18).
 *     Numbered entity-id stacks; auto-grow on reference; dedup-on-push.
 *   - the named GROUPS: std::map<std::string, std::vector<int>> -- OG group nil-node DAT_180031830.
 *   - the 9 STORE-op handlers (psel/popsel/phov/cstk/pr/pg/pop2g/filtinh/filtcls) -- semantics + the
 *     VERBATIM OG toast strings (incl. the filtcls "had inherit" mislabel).
 *   - the registrar (FUN_180003c80 port): register all 20 subcommands via the interface +0x188.
 *
 * The stores are PURE + offline-testable (no engine state). The op handlers reach the editor (selection
 * read/write, hovered id, toast, class/inherit read) through the BACKEND-owned interface vtable (the
 * engine-touch slots iface_engine.c fills) -- so the DECISION (store mutation, toast text) lives here
 * and the engine touch is behind the vtable, exactly the the reference implementation split.
 *
 * Clean-room: ported from our own RE + the reference implementation. Zero OG SnapHak bytes.
 */
#ifndef SNAPSTACK_H
#define SNAPSTACK_H

#include <string>
#include <vector>
#include <map>

struct sh_iface;

/* ---------------------------------------------------------------------- the STACK-OF-STACKS --------
 * std::vector<std::vector<int>> mirror of OG DAT_180031800 (vec-of-vec, stride 0x18). Auto-grows: a
 * reference to stack N materializes 0..N (intervening empty). Dedup-on-push (psel's body). */
class StackStore {
public:
    static const int DEFAULT_STACK = 0;

    /* the ids on stack `index` (auto-creates the stack; returns the live vector). */
    std::vector<int> &get(int index);
    /* number of stacks materialized (== the C++ vector size). */
    int count() const { return (int)stacks_.size(); }
    /* push `ids` onto stack `index`, dedup (skip ids already present). Returns #pushed. */
    int push(int index, const std::vector<int> &ids);
    /* empty stack `index` in place (cstk); in-range only, no auto-grow (the OG range guard). */
    void clear(int index);
    /* move stack `index`'s ids OUT (leave it EMPTY); return the moved ids (FUN_180001d84). Out-of-range
     * -> empty + no grow (why filt + pop2g "clear" the stack). */
    std::vector<int> move_out(int index);

private:
    void ensure(int index);
    std::vector<std::vector<int>> stacks_;
};

/* ---------------------------------------------------------------------- the named GROUPS -----------
 * std::map<std::string, std::vector<int>> mirror of OG group nil-node DAT_180031830. Lookup auto-creates
 * an empty group (the OG map lookup FUN_180003e9c inserts on a miss) -- get/set never throw. */
class GroupStore {
public:
    std::vector<int> &get(const std::string &name);          /* auto-creates empty (OG lookup-or-insert) */
    void set(const std::string &name, const std::vector<int> &ids);  /* replace (pop2g's move-into swap) */
    bool has(const std::string &name) const;

private:
    std::map<std::string, std::vector<int>> groups_;
};

/* pop2g's group-name gate (0x2998 isalpha): the name MUST start with a letter (empty fails). */
bool is_valid_group_name(const std::string &name);

/* C atoi clamped >= 0; absent/blank -> 0 (the handler stack-index parse, snapstack.parse_stack_index). */
int parse_stack_index(const char *arg);

/* ---------------------------------------------------------------------- the registrar --------------
 * Port of OG snaphakui FUN_180003c80: register all 20 SnapStack subcommands on the interface (+0x188).
 * Called once from the think-loop on UI init (snaphak_ui_init.cpp). The handlers are bound with `iface`
 * as their ctx (so each reaches the engine-touch vtable slots + the shared stores). 9 REAL store-op
 * handlers + 8 apply ops (the 8-pass engine round-trip) + 3 deferred (bsin/bscls/bsincls) refusing handlers = the full 20,
 * OG-faithful (the cmd-map ends with 20 entries). */
void sh_register_snapstack_commands(sh_iface *iface);

/* ---------------------------------------------------------------------- Entities ctx-menu hook --
 * Push a single entity id onto stack `index` (dedup), reusing the SHARED store the `sh` ops mutate. The
 * Entities right-click "Push to stack 0" (OG FUN_180017384 -> FUN_180001c5c -> FUN_180001154) pushes the
 * clicked id onto stack 0. Exposed so sh_tabs.cpp's ctx-menu reaches the same g_stacks the ops use. */
void sh_snapstack_push_one(int index, int id);

/* Push every id in `ids` onto stack `index` (dedup), reusing the SHARED store -- the batch form the Entities
 * ctx-menu "Push to stack 0" uses to push the LIST SELECTION (OG FUN_180018154 loops selectedItems()). */
void sh_snapstack_push_ids(int index, const std::vector<int> &ids);

#endif /* SNAPSTACK_H */
