#include <assert.h>
#include <stdio.h>
#include "../src/backend/engine_cvar_read.h"

int main(void)
{
    unsigned char system[32] = {0}, cv[80] = {0};
    void *rows[2] = {NULL, cv};
    void *slot = system;
    int value = -1;
    *(void ***)(system + 8) = rows;
    *(uint32_t *)(system + 0x10) = 2;
    *(const char **)(cv + 0x40) = "snapEdit_enableCopyPaste";
    assert(!sh_engine_cvar_find(NULL, "snapEdit_enableCopyPaste"));
    assert(!sh_engine_cvar_find(&slot, "missing"));
    assert(sh_engine_cvar_find(&slot, "snapEdit_enableCopyPaste") == cv);
    assert(sh_engine_cvar_read_int(cv, &value) && value == 0);
    *(int *)(cv + 0x30) = 1;
    assert(sh_engine_cvar_read_int(cv, &value) && value == 1);
    assert(!sh_engine_cvar_read_int(NULL, &value));
    assert(!sh_engine_cvar_read_int((void *)1, &value));
    *(uint32_t *)(system + 0x10) = 100001;
    assert(!sh_engine_cvar_find(&slot, "snapEdit_enableCopyPaste"));
    slot = (void *)1;
    assert(!sh_engine_cvar_find(&slot, "snapEdit_enableCopyPaste"));
    puts("engine_cvar_read_test OK");
    return 0;
}
