/* modclaim_fake.c -- see modclaim_fake.h. Calls arrive on the modnet worker; the test reads the record once the
 * publish is idle again. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modclaim_fake.h"
#include "../src/common/text.h"

fc_state g_fc;

void fc_reset(void) { memset(&g_fc, 0, sizeof g_fc); }

void fc_set_unit(const char *hash, int state, unsigned long long bytes)
{
    int i;
    for (i = 0; i < g_fc.nunits; i++) if (!strcmp(g_fc.units[i].hash, hash)) break;
    if (i == g_fc.nunits) {
        if (g_fc.nunits == FC_MAX) return;
        g_fc.nunits++;
    }
    strcpy_s(g_fc.units[i].hash, sizeof g_fc.units[i].hash, hash);
    g_fc.units[i].state = state;
    g_fc.units[i].bytes = bytes;
}

static int fc_take(int *code)
{
    int c = *code;
    *code = 0;
    return c;
}

static void fc_why(char **why)
{
    if (why && g_fc.why[0]) *why = sh_text(g_fc.why);
}

static void fc_know(const char *ref, int known)
{
    int i;
    for (i = 0; i < g_fc.nknown; i++) if (!strcmp(g_fc.known[i], ref)) break;
    if (known && i == g_fc.nknown && g_fc.nknown < FC_MAX) strcpy_s(g_fc.known[g_fc.nknown++], 64, ref);
    if (!known && i < g_fc.nknown) {
        memmove(&g_fc.known[i], &g_fc.known[i + 1], (size_t)(g_fc.nknown - i - 1) * sizeof g_fc.known[0]);
        g_fc.nknown--;
    }
}

void modclaim_plan_free(modclaim_plan *plan)
{
    if (!plan) return;
    free(plan->units);
    free(plan->state);
    free(plan->bytes);
    memset(plan, 0, sizeof *plan);
}

int modclaim_signer(modnet_signer *out, int *made, char **why)
{
    (void)out;
    g_fc.signers++;
    if (made) *made = g_fc.made_key;
    g_fc.made_key = 0;
    if (g_fc.signer_fails) {
        g_fc.signer_fails = 0;
        if (why) *why = sh_text("Snapmap+ could not make this computer's publishing key");
        return 0;
    }
    return 1;
}

void modclaim_set_key_folder(const char *folder) { (void)folder; }
void modclaim_set_root(const char *mods_root) { (void)mods_root; }
void modclaim_set_budget(unsigned long long ms) { (void)ms; }

int modclaim_plan_units(const char *const *units, size_t n, modclaim_plan *plan, const modnet_io *io, char **why)
{
    size_t i;
    int code;
    (void)io;
    memset(plan, 0, sizeof *plan);
    g_fc.plans++;
    if ((code = fc_take(&g_fc.plan_code)) != 0) { fc_why(why); return code; }
    plan->units = calloc(n ? n : 1, sizeof *plan->units);
    plan->state = calloc(n ? n : 1, sizeof *plan->state);
    plan->bytes = calloc(n ? n : 1, sizeof *plan->bytes);
    if (!plan->units || !plan->state || !plan->bytes) { modclaim_plan_free(plan); return MODNET_E_NOMEM; }
    plan->n = n;
    for (i = 0; i < n; i++) {
        int k, state = MODCLAIM_HELD;
        unsigned long long bytes = 0;
        strcpy_s(plan->units[i], MODNET_HASH_HEX + 1, units[i]);
        for (k = 0; k < g_fc.nunits; k++)
            if (!strcmp(g_fc.units[k].hash, units[i])) { state = g_fc.units[k].state; bytes = g_fc.units[k].bytes; }
        plan->state[i] = state;
        if (state == MODCLAIM_MISSING) { plan->bytes[i] = bytes; plan->missing_bytes += bytes; plan->missing++; }
        if (state == MODCLAIM_ABSENT) plan->absent++;
        if (state == MODCLAIM_REMOVED) plan->removed++;
    }
    return MODNET_OK;
}

int modclaim_upload(const modclaim_plan *plan, const modnet_io *io, char **why)
{
    unsigned step;
    int code;
    g_fc.uploads++;
    for (step = 1; step <= 4 && g_fc.upload_delay_ms; step++) {
        Sleep(g_fc.upload_delay_ms / 4);
        if (io && io->progress) io->progress(io->context, plan->missing_bytes * step / 4, plan->missing_bytes);
    }
    if ((code = fc_take(&g_fc.upload_code)) != 0) { fc_why(why); return code; }
    g_fc.uploaded_bytes += plan->missing_bytes;
    for (size_t i = 0; i < plan->n; i++)
        if (plan->state[i] == MODCLAIM_MISSING) fc_set_unit(plan->units[i], MODCLAIM_HELD, 0);
    return MODNET_OK;
}

int modclaim_claim(const char *ref, const char *const *units, size_t n, char (**removed)[MODNET_HASH_HEX + 1],
                   size_t *nremoved, const modnet_io *io, char **why)
{
    int code;
    size_t i;
    (void)io;
    if (removed) *removed = NULL;
    if (nremoved) *nremoved = 0;
    g_fc.claims++;
    strcpy_s(g_fc.claim_ref, sizeof g_fc.claim_ref, ref);
    g_fc.claim_n = n;
    for (i = 0; i < n && i < FC_MAX; i++) strcpy_s(g_fc.claim_units[i], MODNET_HASH_HEX + 1, units[i]);
    if ((code = fc_take(&g_fc.claim_code)) != 0) { fc_why(why); return code; }
    fc_know(ref, 1);
    return MODNET_OK;
}

int modclaim_release(const char *ref, const modnet_io *io, char **why)
{
    int code;
    (void)io;
    g_fc.releases++;
    strcpy_s(g_fc.release_ref, sizeof g_fc.release_ref, ref);
    if ((code = fc_take(&g_fc.release_code)) != 0) { fc_why(why); return code; }
    fc_know(ref, 0);
    return MODNET_OK;
}

int modclaim_queue(const char *ref, const char *const *units, size_t n)
{
    (void)units;
    g_fc.queued++;
    strcpy_s(g_fc.queued_ref, sizeof g_fc.queued_ref, ref);
    g_fc.queued_n = n;
    fc_know(ref, 1);
    return 1;
}

int modclaim_launch(const modnet_io *io, modclaim_launch_stats *out)
{
    (void)io;
    g_fc.launches++;
    if (out) { memset(out, 0, sizeof *out); out->retried = g_fc.queued ? 1 : 0; }
    return MODNET_OK;
}

void modclaim_set_mods(const char *ref, const char *const *salts, size_t n)
{
    (void)ref;
    g_fc.mods_n = n;
    if (n && salts[0]) strcpy_s(g_fc.mods0, sizeof g_fc.mods0, salts[0]);
}

int modclaim_known(const char *ref)
{
    for (int i = 0; i < g_fc.nknown; i++) if (!strcmp(g_fc.known[i], ref)) return 1;
    return 0;
}

size_t modclaim_held(char (**out)[MODNET_HASH_HEX + 1])
{
    if (out) *out = NULL;
    return 0;
}

int modclaim_asked(modclaim_asked_fn fn, void *context, const modnet_io *io, char **why)
{
    (void)fn; (void)context; (void)io; (void)why;
    return MODNET_OK;
}
