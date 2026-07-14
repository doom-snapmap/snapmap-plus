/* snapstack.h -- BACKEND-hosted port of src/ui/snapstack.cpp: the SnapStack stores (stack-of-stacks +
 * named groups) and the 20 `sh <subcommand>` console handlers, in pure C.
 *
 * This is ADDITIVE, not a replacement: src/ui/snapstack.cpp (Qt) is untouched. Both copies register the
 * SAME 20 command names on the SAME shared sh_iface cmd-map; whichever registers LAST wins (the cmd-map
 * overwrites on duplicate name -- see snaphak_iface.c's iface_register_cmd). This module registers from
 * ui_bridge.c right after the interface object is created (before any frontend loads), and Qt's own
 * unchanged registrar (snaphak_ui_init.cpp -> snapstack.cpp) still runs after and overwrites these with
 * Qt's own handlers -- so a Qt build's *behavior* is unaffected; only a frontend that never registers
 * its own copy (the webview host) ends up actually running this module's handlers.
 *
 * Stores are file-static singletons here (mirrors Qt's own g_stacks/g_groups singleton pattern) -- a
 * SEPARATE instance from Qt's, since they live in different DLLs/address spaces when Qt IS running (Qt's
 * copy is compiled into snaphakui.dll; this copy is compiled into XINPUT1_3.dll). That's fine: only one
 * of the two copies of any given command is ever the ACTIVE one at a time (whichever registered last),
 * so only one store is ever actually read/written in a given session.
 *
 * Clean-room: ported from our own RE (src/ui/snapstack.cpp, itself clean-room) + the JSON structural
 * work now lives in json_patch.c instead of QJsonObject. Zero OG SnapHak bytes.
 */
#ifndef BACKEND_SNAPSTACK_H
#define BACKEND_SNAPSTACK_H

#include "snaphak_iface.h"

/* Register the 20 SnapStack subcommands PLUS `snapstack_diag` (a backend-exclusive diagnostic -- see
 * snapstack.c) on the interface's cmd-map (+0x188). Safe to call once, early (before any frontend loads)
 * -- ctx = iface for every handler, matching the Qt registrar's convention. */
void sh_register_snapstack_commands_backend(sh_iface *iface);

/* The Entities-tab "Push to stack 0" context-menu action, exposed for the push_to_stack vtable slot
 * (iface_engine.c) so ANY frontend (not just one compiled in-process with this module) can push onto
 * the SAME backend-owned stack a `sh psel`/`sh popsel`/etc. console command run afterward will see.
 * Dedup-on-push, same as every other path into the stores. */
void sh_snapstack_push_ids_backend(int index, const int *ids, int count);

/* The Entities-tab "Clear stack 0" context-menu action, exposed for the clear_stack vtable slot
 * (iface_engine.c) so ANY frontend can empty the SAME backend-owned stack a `sh cstk` console command
 * would -- lets a user work purely from the UI without ever touching the DOOM console. Returns the number
 * of ids that were on the stack before clearing (so the caller can toast a count, same as `sh cstk` itself). */
int sh_snapstack_clear_stack_backend(int index);

#endif /* BACKEND_SNAPSTACK_H */
