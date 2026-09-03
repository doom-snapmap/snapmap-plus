/* log_rotate.h -- keep an append-only log from growing without bound.
 *
 * Every log this product writes is opened with FILE_APPEND_DATA and never
 * truncated, which is right for diagnostics and wrong for a user's disk: a
 * measured install reached 47 MB of sh_backend.log in about a week, in a folder
 * the player never asked for and would not know to empty.
 *
 * Rolling once per session, rather than mid-write, is deliberate. A size check
 * on every line costs a syscall per line on the engine's own threads, and a log
 * that rolls mid-session splits one run's evidence across two files, which is
 * exactly when it is hardest to read. One roll at startup keeps the run that
 * just happened whole and the run before it intact, and costs one call.
 */
#ifndef SNAPMAP_PLUS_LOG_ROTATE_H
#define SNAPMAP_PLUS_LOG_ROTATE_H

/* The UI is a separate C++ DLL with its own log and the same problem, so this
 * links into both and needs C linkage from that side. */
#ifdef __cplusplus
extern "C" {
#endif

/* The budget one log is allowed to reach before it is rolled aside. A session's
 * worth of diagnostics is far below this; the logs that pass it are the ones
 * that have been accumulating across many runs. */
#define LOG_ROTATE_CAP_BYTES (4ull * 1024ull * 1024ull)

/* Roll `path` to `<path>.prev` when it is at or over `cap_bytes`, replacing any
 * previous roll. Call once per process, before the first line of the session is
 * written. Returns 1 if it rolled, 0 if it did not need to or could not. A
 * failure is not reported anywhere on purpose: this runs before logging is
 * usable, and a log that could not be rolled is a log that still works. */
int log_rotate_if_large(const char *path, unsigned long long cap_bytes);

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_LOG_ROTATE_H */
