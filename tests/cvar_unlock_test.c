/* The cvar unlock aliases the developer table only after registration ends. */
#include "../src/backend/cvar_unlock.c"
#include <assert.h>
#include <stdio.h>

void backend_log(const char *message) { (void)message; }
const uint8_t *sh_host_image_base(void) { return NULL; }
int sh_host_is_supported_build(void) { return 0; }
uint32_t sh_host_rva(uint32_t vk, uint32_t gl) { (void)vk; (void)gl; return 0; }

enum { NCVARS = 6 };
static __declspec(align(16)) uint8_t g_sys[0x90];
static __declspec(align(16)) uint8_t g_cvar[NCVARS][0x98];
static void *g_full[16], *g_dev[16];
static int32_t g_full_buckets[64], g_full_chain[16], g_dev_buckets[16], g_dev_chain[16];

static void set_list(size_t off, void **arr, int32_t n, int32_t cap)
{
    memcpy(g_sys + off, &arr, 8);
    memcpy(g_sys + off + 8, &n, 4);
    memcpy(g_sys + off + 12, &cap, 4);
}
static void set_hash(size_t off, int32_t *buckets, int32_t *chain, int32_t size)
{
    int32_t mask = size - 1, lookup = -1;
    memcpy(g_sys + off, &buckets, 8);
    memcpy(g_sys + off + 8, &chain, 8);
    memcpy(g_sys + off + 0x10, &size, 4);
    memcpy(g_sys + off + 0x1c, &mask, 4);
    memcpy(g_sys + off + 0x20, &lookup, 4);
}
/* Chain head g_cvar[0] .. tail g_cvar[NCVARS-1]; cvars 1 and 4 are exposed. */
static void build_chain(void)
{
    int i;
    memset(g_cvar, 0, sizeof g_cvar);
    for (i = 0; i < NCVARS; i++) {
        void *next = i + 1 < NCVARS ? g_cvar[i + 1] : NULL;
        uint32_t flags = (i == 1 || i == 4) ? CVAR_FLAG_EXPOSE : 0;
        memcpy(g_cvar[i] + SH_IDCVAR_NEXT, &next, 8);
        memcpy(g_cvar[i] + SH_IDCVAR_FLAGS, &flags, 4);
        g_full[i] = g_cvar[i];
    }
    g_dev[0] = g_cvar[1];
    g_dev[1] = g_cvar[4];
}

int main(void)
{
    uint32_t n, len;
    memset(g_sys, 0, sizeof g_sys);
    build_chain();

    /* Nothing registered yet. */
    assert(cvu_registration_state(g_sys, &n, &len) == CVU_REG_NOT_STARTED);

    /* Midway: the full list stops short of the chain (the case that crashed). */
    set_list(CVARSYS_FULL_LIST_OFF, g_full, 3, 16);
    set_list(CVARSYS_DEV_LIST_OFF, g_dev, 1, 16);
    assert(cvu_registration_state(g_sys, &n, &len) == CVU_REG_RUNNING && n == 3 && len == NCVARS);
    assert(apply_unlock(g_sys) == 0 && g_cvu_running_polls == 1 && g_cvu_running_first == 3);

    /* The full list is complete but the last exposed cvar's developer append is pending. */
    {
        void *last = g_cvar[4];
        set_list(CVARSYS_FULL_LIST_OFF, g_full, NCVARS, 16);
        set_list(CVARSYS_DEV_LIST_OFF, g_dev, 1, 16);
        assert(cvu_registration_state(g_sys, &n, &len) == CVU_REG_RUNNING);
        /* A developer list whose last entry is not the last exposed cvar is not final. */
        g_dev[1] = g_cvar[3];
        set_list(CVARSYS_DEV_LIST_OFF, g_dev, 2, 16);
        assert(cvu_registration_state(g_sys, &n, &len) == CVU_REG_RUNNING);
        g_dev[1] = last;
    }

    /* Registration finished. */
    set_list(CVARSYS_DEV_LIST_OFF, g_dev, 2, 16);
    set_hash(CVARSYS_FULL_HASH_OFF, g_full_buckets, g_full_chain, 64);
    set_hash(CVARSYS_DEV_HASH_OFF, g_dev_buckets, g_dev_chain, 16);
    assert(cvu_registration_state(g_sys, &n, &len) == CVU_REG_DONE && n == NCVARS && len == NCVARS);

    /* A full hash smaller than the developer hash would let old masks overrun it. */
    set_hash(CVARSYS_FULL_HASH_OFF, g_full_buckets, g_full_chain, 8);
    assert(alias_dev_to_full(g_sys) == 0);
    assert(memcmp(g_sys + CVARSYS_DEV_HASH_OFF, g_sys + CVARSYS_FULL_HASH_OFF, SIZEOF_IDHASHINDEX) != 0);
    set_hash(CVARSYS_FULL_HASH_OFF, g_full_buckets, g_full_chain, 64);

    /* The alias copies both headers and marks every registered cvar settable. */
    assert(apply_unlock(g_sys) == 1 && g_cvu_aliased && g_cvu_alias_count == NCVARS);
    assert(!memcmp(g_sys + CVARSYS_DEV_LIST_OFF, g_sys + CVARSYS_FULL_LIST_OFF, SIZEOF_IDLIST));
    assert(!memcmp(g_sys + CVARSYS_DEV_HASH_OFF, g_sys + CVARSYS_FULL_HASH_OFF, SIZEOF_IDHASHINDEX));
    {
        int i;
        for (i = 0; i < NCVARS; i++)
            assert(*(uint32_t *)(g_cvar[i] + SH_IDCVAR_FLAGS) & CVAR_FLAG_NOCHEAT);
    }
    assert(cvu_registration_state(g_sys, &n, &len) == CVU_REG_DONE);

    /* A refresh with nothing changed leaves the headers alone and logs no change. */
    assert(apply_unlock(g_sys) == 1 && g_cvu_changes == 0);

    /* A full table that changes after the alias is noticed and aliased again. */
    set_list(CVARSYS_FULL_LIST_OFF, g_full, NCVARS, 32);
    assert(apply_unlock(g_sys) == 1 && g_cvu_changes == 1);
    assert(!memcmp(g_sys + CVARSYS_DEV_LIST_OFF, g_sys + CVARSYS_FULL_LIST_OFF, SIZEOF_IDLIST));

    /* An unreadable object is refused, not faulted on. */
    assert(cvu_registration_state((const uint8_t *)(uintptr_t)0x10, &n, &len) == CVU_REG_NOT_STARTED);
    assert(alias_dev_to_full((uint8_t *)(uintptr_t)0x10) == 0);

    printf("cvar_unlock_test: ok\n");
    return 0;
}
