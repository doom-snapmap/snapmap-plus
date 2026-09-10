/* CF_TEXT clipboard helpers for generated command output and view-position reads. */
#ifndef BACKEND_B2_CLIPBOARD_H
#define BACKEND_B2_CLIPBOARD_H

/* Copy NUL-terminated text to the clipboard. Return 1 after ownership transfers
 * to Windows, or 0 for invalid input or failure. Guarded against access faults. */
int sh_clipboard_set(const char *text);

/* Read CF_TEXT into out, truncating to cap-1 and NUL-terminating on success.
 * Return 1 after a read, or 0 for invalid input, missing text, or failure. */
int sh_clipboard_get(char *out, int cap);

#endif /* BACKEND_B2_CLIPBOARD_H */
