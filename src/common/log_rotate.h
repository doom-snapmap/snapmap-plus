/* Rotate oversized append-only logs once at startup. Avoid per-line filesystem
 * checks and keep each session together instead of splitting it during writes. */
#ifndef SNAPMAP_PLUS_LOG_ROTATE_H
#define SNAPMAP_PLUS_LOG_ROTATE_H

/* Shared by the C backend and C++ frontend. */
#ifdef __cplusplus
extern "C" {
#endif

/* Startup rollover threshold; logs can grow beyond it during a session. */
#define LOG_ROTATE_CAP_BYTES (4ull * 1024ull * 1024ull)

/* Move path to path.prev at or above cap_bytes, replacing the prior roll.
 * Call before the first log write. Return 1 if moved, otherwise 0; failures
 * are silent because logging is not initialized yet. */
int log_rotate_if_large(const char *path, unsigned long long cap_bytes);

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_LOG_ROTATE_H */
