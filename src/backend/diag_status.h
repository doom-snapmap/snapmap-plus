/* diag_status.h -- the diagnostic build's status export for the development
 * daemon. Built only with build.ps1 -Diag (SH_DIAG); a release build has no
 * such export and none of this code.
 *
 * sh_diag_status is exported at ordinal 300 by xinput1_4_diag.def. It writes
 * one JSON object into out and returns its length, or, when cap is too small,
 * the length it needs (out is then left empty when cap > 0):
 *
 *   {"protocol":1,"version":"<sh_product_version()>","iface_vtbl":<bytes>,
 *    "features_off":["<feature name>",...],
 *    "rawmap":{"opens":<n>,"generation":<n>,"pending":<0|1>,"faulted":<0|1>,
 *              "last":{"seq":<n>,"path":"<utf-8>",
 *                      "state":"queued|opening|opened|refused|failed|faulted",
 *                      "rc":<int>,"reason":"<plain reason or empty>",
 *                      "bytes":<n>,"mods":<n>}}}
 *
 * last.state is "" and last.seq 0 before the first open. Any thread. */
#ifndef BACKEND_DIAG_STATUS_H
#define BACKEND_DIAG_STATUS_H

#ifdef SH_DIAG
#include <stddef.h>

#define SH_DIAG_STATUS_PROTOCOL 1

int sh_diag_status(char *out, int cap);

/* The object, malloc'd and sized to it; NULL when memory runs out. */
char *sh_diag_status_build(void);
#endif

#endif /* BACKEND_DIAG_STATUS_H */
