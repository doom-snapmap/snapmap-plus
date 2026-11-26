#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct sh_sound_view {
    void *bytes;
    uint32_t size, id;
    int padded;
} sh_sound_view;

/* Wwise keeps the bank and its embedded media by address until it unloads the bank. */
int sh_sound_view_open(const char *path, uint64_t size, sh_sound_view *out);
void sh_sound_view_close(sh_sound_view *view);
int sh_sound_bank_has_event(const char *path, uint64_t size, uint32_t event);

typedef void (*sh_sound_result_fn)(uint32_t, const void *, int32_t, int32_t, void *);
typedef struct sh_sound_request {
    uint32_t op, reserved;
    sh_sound_result_fn callback;
    void *cookie;
    uint32_t source, reserved2, id;
    int32_t pool;
    const void *bytes;
    uint32_t length, reserved3;
} sh_sound_request;

/* Decode the manager's RIP-relative pointer in the recognized native async LoadBank wrapper. */
void **sh_sound_view_manager(const uint8_t *load);
int sh_sound_view_queue(void *manager, const sh_sound_view *view, int unload, sh_sound_result_fn result, void *cookie);
