/* SnapMap's bit-flag property inspector over package spawn-type keys. */
#ifndef BACKEND_KEY_INSPECTOR_H
#define BACKEND_KEY_INSPECTOR_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"
#include "package_keys.h"

/* Adapt Model<idSnapBitFlagInspector>'s build and apply for spawnAIType_t
 * inspectors (the AI filter's choices). Both routines are verified byte for
 * byte, relocations aside, before either is detoured; any mismatch installs
 * neither and the stock inspector stays. Returns 1 when installed. */
int sh_key_inspector_install(const sig_result *results, size_t count, const uint8_t *base);
int sh_key_inspector_active(void);

#ifdef SH_KEY_INSPECTOR_TESTING
/* Synthetic native state: the engine helpers the replacements call, the
 * native routines they fall back to and the key contract they read. */
typedef struct sh_key_inspector_native {
    void (*str_ctor)(void *str);
    void (*str_dtor)(void *str);
    char (*read)(void *object, void *path, void *str, char clear);
    char (*write)(void *object, void *path, const char *text, void *types);
    void *(*types)(void);
    uint32_t (*parse)(void *types, void *enum_type, const char *text, uint32_t fallback);
    void *(*lookup)(void *types, void *enum_type, const char *name, char create);
    void (*text)(void *types, void *enum_type, uint32_t value, void *str);
    void (*resize)(void *list, int count);
    void (*grow)(void *list);
    const char *(*label)(const int *id);
    void *(*create)(void *parent, const char *label, const int *label_id, const int *description_id,
        char enabled, char checked);
    void (*native_build)(uint8_t *model, void *parent, void *defaults);
    void (*native_apply)(uint8_t *model);
    int (*keys_active)(void);
    void (*decode)(uint32_t value, sh_package_key_set *keys, uint32_t *historical);
    int (*code)(const sh_package_key_set *keys, uint32_t *code);
} sh_key_inspector_native;
void sh_key_inspector_test_bind(const sh_key_inspector_native *native);
void sh_key_inspector_test_build(uint8_t *model, void *parent, void *defaults);
void sh_key_inspector_test_apply(uint8_t *model);
/* 1 when bytes match the verified build or apply routine (relocations aside);
 * the pattern each is verified against. */
int sh_key_inspector_test_verify(const uint8_t *build, const uint8_t *apply);
const char *sh_key_inspector_test_code(int apply);
#endif

#endif
