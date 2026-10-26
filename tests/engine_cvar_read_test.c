#include <assert.h>
#include <stdio.h>
#include "../src/backend/engine_cvar_read.h"

int main(void)
{
    unsigned char system[32] = {0}, cv[96] = {0};
    void *rows[2] = {NULL, cv};
    void *slot = system;
    int value = -1;
    *(void ***)(system + 8) = rows;
    *(uint32_t *)(system + 0x10) = 2;
    *(const char **)(cv + 0x50) = "snapEdit_enableCopyPaste";
    assert(!sh_engine_cvar_find(NULL, 0x50, "snapEdit_enableCopyPaste"));
    assert(!sh_engine_cvar_find(&slot, 0x50, "missing"));
    assert(sh_engine_cvar_find(&slot, 0x50, "snapEdit_enableCopyPaste") == cv);
    /* Offset 0 (registry layout not established) refuses every lookup. */
    assert(!sh_engine_cvar_find(&slot, 0, "snapEdit_enableCopyPaste"));
    assert(sh_engine_cvar_read_int(cv, &value) && value == 0);
    *(int *)(cv + 0x30) = 1;
    assert(sh_engine_cvar_read_int(cv, &value) && value == 1);
    assert(!sh_engine_cvar_read_int(NULL, &value));
    assert(!sh_engine_cvar_read_int((void *)1, &value));
    *(uint32_t *)(system + 0x10) = 100001;
    assert(!sh_engine_cvar_find(&slot, 0x50, "snapEdit_enableCopyPaste"));
    slot = (void *)1;
    assert(!sh_engine_cvar_find(&slot, 0x50, "snapEdit_enableCopyPaste"));
    puts("engine_cvar_read_test OK");
    return 0;
}
