/* Unicode clipboard helpers for generated command output, view-position reads
 * and SWF text fields. Text crosses this interface as UTF-8. */
#ifndef BACKEND_B2_CLIPBOARD_H
#define BACKEND_B2_CLIPBOARD_H

/* Copy NUL-terminated UTF-8 text to the clipboard as Unicode text. Return 1
 * after ownership transfers to Windows, or 0 for invalid input or failure. */
int sh_clipboard_set(const char *text);

/* Read the clipboard's Unicode text into out as UTF-8, cut at a character
 * boundary to fit cap-1 bytes and NUL-terminated. Return 1 after a read, or 0
 * for invalid input, no text, or failure. */
int sh_clipboard_get(char *out, int cap);

/* The longest prefix of `len` UTF-8 bytes of `text` that fits in `cap` bytes
 * without splitting a character. */
int sh_clipboard_utf8_fit(const char *text, int len, int cap);

#endif /* BACKEND_B2_CLIPBOARD_H */
