/* clipboard.h -- a clean-room CF_TEXT clipboard-set helper
 * (port of OG XINPUT1_3 FUN_1800053f0). Used by the sh_listres handler (and, in later
 * tranches, sh_dumpdef / sh_spawninfo / sh_type) to copy a generated text list onto the
 * Windows clipboard.
 *
 * OG sequence (clipboard_0x53f0.txt, DIRECT): OpenClipboard(NULL) -> strlen -> GlobalAlloc
 * (GMEM_MOVEABLE, len+1) -> GlobalLock -> memcpy(dst, text, len+1) -> GlobalUnlock ->
 * EmptyClipboard -> SetClipboardData(CF_TEXT, hMem) -> CloseClipboard. On SetClipboardData
 * success the OS owns the handle (do NOT free); OG leaks on the failure path -- the clone
 * GlobalFree()s on failure. Pure Win32, no engine deps.
 *
 * Clean-room: ported from our own RE (the FUN_1800053f0 decompile + capstone disasm). Zero
 * OG SnapHak bytes.
 */
#ifndef BACKEND_B2_CLIPBOARD_H
#define BACKEND_B2_CLIPBOARD_H

/* Set the Windows clipboard (CF_TEXT) to the NUL-terminated ASCII string `text`. No-op + 0
 * on a NULL arg or any Win32 failure (OpenClipboard/GlobalAlloc/GlobalLock/SetClipboardData);
 * the whole call is hardened so a clipboard hiccup never destabilizes the editor. Returns 1
 * on a confirmed SetClipboardData success, 0 otherwise. Leak-free: on success the clipboard
 * owns the handle, on any failure path the handle is GlobalFree()d. */
int sh_clipboard_set(const char *text);

/* Read the Windows clipboard's CF_TEXT into the caller-supplied buffer `out` (capacity `cap`, always
 * NUL-terminated on success). Used by sh_spawninfo to read back the engine `getviewpos` output (the
 * engine writes the current view position/orientation to the clipboard, which sh_spawninfo parses as
 * "%f %f %f %f %f"). OpenClipboard(NULL) -> GetClipboardData(CF_TEXT) -> GlobalLock -> copy (truncated
 * to cap-1) -> GlobalUnlock -> CloseClipboard. No-op + 0 on a NULL/zero buffer, no CF_TEXT present, or
 * any Win32 failure; the whole call is SEH-guarded so a clipboard hiccup never destabilizes the editor.
 * Returns 1 on a confirmed read (out holds the NUL-terminated text), 0 otherwise. */
int sh_clipboard_get(char *out, int cap);

#endif /* BACKEND_B2_CLIPBOARD_H */
