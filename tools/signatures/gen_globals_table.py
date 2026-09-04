"""Emit the product's engine_globals table from the derivation results.

Input:  derived_globals.json (produced by derive_global.py)
Output: engine_globals_table.gen.h -- the BACKEND_ENGINE_GLOBALS array

This file is GENERATED. Nobody hand-maintains a per-build address table; that is the
whole point. Adding support for another DOOM build means re-running the derivation and
regenerating, not editing C by hand.
"""
import json, os, sys

# Every global the product actually consumes, keyed by its RVA on the pinned Vulkan build.
# `delta` is for globals that are reached as a fixed offset from a neighbouring slot the
# engine's own code computes -- the same adjacency the backend already relies on for
# cvarSys == cmdSystem + 0x10.
NAMES = {
    0x3056748: ('editor_singleton',       0, 'idSnapEditorLocal, constructed in place'),
    0x55b7280: ('cmd_system_slot',        0, 'idCmdSystem singleton pointer'),
    0x55b7290: ('cvar_system_slot',       0, 'idCvarSystem singleton pointer (cmdSystem + 0x10)'),
    0x56ffb90: ('game_manager_slot',      0, 'game manager singleton pointer'),
    0x57216f0: ('render_world_slot',      0, 'idRenderWorld singleton pointer'),
    0x6dde190: ('main_thread_id',         0, 'engine main-thread id (DWORD)'),
    0x6dde198: ('load_state',             0, 'map load state; the Play detector'),
    0x6dde19c: ('error_state',            0, 'error state, cleared during shield recovery'),
    0x6ddd990: ('last_error_msg',         0, 'last error message buffer (0x800 bytes)'),
    0x6faf820: ('throw_suppressor_a',     0, 'level-6 dispatcher throw gate A'),
    0x4df7fc8: ('shell_ptr_slot',         0, 'menu shell pointer slot'),
    0x4df9648: ('validator_manager',      0, 'reflection validator manager pointer'),
    0x5557090: ('decl_visibility_manager', 0, 'decl visibility manager pointer'),
    0x59bd8f0: ('resource_manager_ctx',   0, 'resource type-manager context'),
    0x59bd9d0: ('material_manager_ctx',   0, 'material type-manager context'),
    0x6217f90: ('decl_resource_head',     0, 'head of the engine decl resource list-of-lists'),
    0x3082b10: ('type_container',         0, 'reflection type container'),
    0x20499a0: ('palette_vtable',         0, 'editor palette vtable (.rdata)'),
    0x27984a0: ('provider_vtable',        0, 'resource provider vtable, 31 slots (.rdata)'),
    0x2ded690: ('throwinfo_recoverable',  0, 'ThrowInfo for the recoverable level-6 throw (.rdata)'),
    0x2ded990: ('throwinfo_fatal',        0, 'ThrowInfo for the fatal level-7 throw (.rdata)'),

    # CODE addresses that cannot be found by their own bytes, anchored on a call site instead.
    # The arithmetic is identical -- a `call rel32` displacement is decoded exactly like a
    # RIP-relative data displacement -- so they live in the same table and need no new mechanism.
    0x17F7030: ('declmgr_accessor',       0, 'idDeclManager accessor; its prologue is shared by ~47 functions'),
    0x699A60:  ('idlist_grow',            0, 'idList growth helper; too generic to sign by its own bytes'),
    0xD32A30:  ('vis_leaf_lo',            0, 'visibility leaf, entry; too short to anchor uniquely'),
}


def fixed_bytes(pattern):
    return sum(1 for t in pattern.split() if t != '??')


def pick(derivations):
    """Choose the anchor to ship: it must agree with the multi-site consensus, and among
    those we take the most SELECTIVE one (most fixed bytes), since every candidate is
    already required to be unique on both images."""
    if not derivations:
        return None, None, 0
    tally = {}
    for d in derivations:
        tally[d['gl_target_rva']] = tally.get(d['gl_target_rva'], 0) + 1
    consensus = max(tally.items(), key=lambda kv: kv[1])
    agreeing = [d for d in derivations if d['gl_target_rva'] == consensus[0]]
    best = max(agreeing, key=lambda d: (fixed_bytes(d['pattern']), -d['bytes']))
    return best, consensus[0], consensus[1]


def main(json_path, out_path):
    data = json.load(open(json_path))
    callees = os.path.join(os.path.dirname(os.path.abspath(json_path)), 'derived_callees.json')
    if os.path.exists(callees):
        data = data + json.load(open(callees))
    rows = []
    skipped = []
    for rec in data:
        vk = int(rec['target_vk_rva'], 16)
        if vk not in NAMES:
            skipped.append((vk, 'no product consumer mapped'))
            continue
        name, delta, note = NAMES[vk]
        best, gl, agree = pick(rec['derivations'])
        if best is None:
            skipped.append((vk, 'no portable derivation'))
            continue
        rows.append(dict(name=name, note=note, delta=delta, vk=vk, gl=gl,
                         agree=agree, total=len(rec['derivations']),
                         pattern=best['pattern'], slot=best['disp_slot'],
                         site=best['vk_site'], mnemonic=best['mnemonic']))

    rows.sort(key=lambda r: r['name'])
    with open(out_path, 'w', newline='\n') as f:
        f.write('/* engine_globals_table.gen.h -- GENERATED. Do not edit by hand.\n'
                ' *\n'
                ' * Each entry signs the CODE SITE that computes a data global\'s address and records\n'
                ' * where the RIP-relative displacement sits inside that pattern. Resolving reads the\n'
                ' * displacement from the live image, so the same table serves any DOOM build whose\n'
                ' * code shape matches -- no per-build address list exists or is wanted.\n'
                ' *\n'
                ' * Every anchor below was verified to match EXACTLY ONCE on both shipped executables\n'
                ' * (DOOMx64vk.exe and DOOMx64.exe), and each address was cross-checked against\n'
                ' * independent reference sites. Regenerate with the project derivation tooling; never\n'
                ' * hand-edit a pattern.\n'
                ' */\n'
                '#ifndef BACKEND_ENGINE_GLOBALS_TABLE_GEN_H\n'
                '#define BACKEND_ENGINE_GLOBALS_TABLE_GEN_H\n\n'
                '#include "engine_globals.h"\n\n'
                'const global_entry BACKEND_ENGINE_GLOBALS[] = {\n')
        for r in rows:
            f.write('    { "%s",\n' % r['name'])
            f.write('      /* %s.\n' % r['note'])
            f.write('       * Anchor: %s at Vulkan 0x%s (%s).\n'
                    % (r['mnemonic'], r['site'].replace('0x', ''), 'unique on both images'))
            f.write('       * Resolves to Vulkan 0x%X / OpenGL %s; %d of %d reference sites agreed. */\n'
                    % (r['vk'], r['gl'], r['agree'], r['total']))
            f.write('      "%s",\n' % r['pattern'])
            f.write('      %d, %d, 0x%Xu },\n\n' % (r['slot'], r['delta'], r['vk']))
        f.write('    { NULL, NULL, 0, 0, 0 }\n};\n\n'
                '#endif /* BACKEND_ENGINE_GLOBALS_TABLE_GEN_H */\n')

    print('wrote %s: %d entries' % (out_path, len(rows)))
    for vk, why in skipped:
        print('  SKIPPED 0x%X: %s' % (vk, why))
    for r in rows:
        print('  %-24s vk=0x%-9X gl=%-11s slot=%-3d %d/%d sites  %dB'
              % (r['name'], r['vk'], r['gl'], r['slot'], r['agree'], r['total'],
                 len(r['pattern'].split())))


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'derived_globals.json'),
         sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, 'engine_globals_table.gen.h'))
