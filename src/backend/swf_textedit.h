/* swf_textedit.h -- clipboard copy/paste for the editor's SWF text fields (datapad / transmission
 * message bodies, and every other free-text property that uses the `textinspector`).
 *
 * WHY THIS EXISTS. Vanilla SnapMap has no text clipboard support at all: you can paste into the dev
 * console, but not into any in-editor text field. This is NOT a disabled feature -- the engine's SWF
 * text-edit key handler has no Ctrl-modifier branch whatsoever (only shift, for selection), so there
 * is nothing to unlock. This adds it.
 *
 * MECHANISM (DIRECT, our own RE -- doom-re campaign `text-inspector-input-path`, 2026-07-27).
 * A SnapMap text property is an `idSnapTextInspector`, whose live widget is an
 * `idMenuPropertyStringInput`, which is backed by a Scaleform `idSWFTextInstance`. Keyboard input for
 * a FOCUSED text instance arrives at exactly one place:
 *
 *     idSWFScriptObject_TextInstancePrototype::idSWFScriptFunction_onKey::Call(self, ret, thisObj, parms)
 *
 * We detour it. That single site hands us everything: `thisObj` is the "TextField" script object whose
 * +0xC0 is the `idSWFTextInstance`, and `parms` carries (scancode, isDown). Focus needs no lookup --
 * the handler only ever runs for the field that has it, so if we are executing, that IS the target
 * field. (The engine does keep a focused-instance pointer at `idSWF+0x1a8`, but we never need it.)
 *
 * Modifier state needs no engine global either: the stock handler tracks SHIFT itself from the
 * (scancode, isDown) pair it is handed, so we track CTRL the identical way.
 *
 * PORTABILITY: the handler resolves by SIGNATURE (never a hardcoded RVA -- the doom-re project proved
 * cited RVAs go stale even against a byte-identical build). The idSWFTextInstance field offsets ARE
 * build-specific -- RE-DERIVE per DOOM build by decompiling that onKey::Call (the constants it
 * dereferences are exactly these).
 *
 * Clean-room: ported from our own RE. Zero OG SnapHak bytes (the original never had this feature).
 */
#ifndef BACKEND_SWF_TEXTEDIT_H
#define BACKEND_SWF_TEXTEDIT_H

#include <stdint.h>

/* Detour the SWF text-instance onKey handler so Ctrl+C copies the focused text field's selection
 * (whole field when nothing is selected) to the Windows clipboard. No-op + logged reason if the
 * signature does not resolve cleanly. Safe to call once at backend install. */
void sh_swf_textedit_install(const uint8_t *module_base);

/* Reverse the detour (leaves the engine byte-clean). Idempotent. */
void sh_swf_textedit_uninstall(void);

#endif /* BACKEND_SWF_TEXTEDIT_H */
