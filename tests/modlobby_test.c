/* modlobby_test -- a co-op lobby for a modded map (modlobby.h): the state codes a joiner sends in the ready
 * command's ignored bytes, the host's table of what members said, the host's start decision and its sentence, and
 * the host's waiting line. The engine binding is not linked (SH_MODLOBBY_PURE). */
#include <stdio.h>
#include <string.h>
#include "../src/backend/modlobby.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static void codes(void)
{
    int state = -1, percent = -1;
    printf("-- the codes live in the bytes the engine ignores, and decode back\n");
    /* The engine's ready command applies 0 toggle, 1 set and 2 unset: none of them is ours. */
    for (unsigned b = 0; b <= 2; b++) CHECK(!sh_modlobby_decode(b, &state, &percent));
    CHECK(!sh_modlobby_decode(0x7F, NULL, NULL) && !sh_modlobby_decode(0xE5, NULL, NULL) && !sh_modlobby_decode(0xFF, NULL, NULL));
    CHECK(sh_modlobby_progress_code(0, 0) == SH_MODLOBBY_CODE_PROGRESS);
    CHECK(sh_modlobby_progress_code(42, 100) == SH_MODLOBBY_CODE_PROGRESS + 40);   /* steps of 5 % */
    CHECK(sh_modlobby_progress_code(300, 100) == SH_MODLOBBY_CODE_PROGRESS + 100);
    CHECK(sh_modlobby_decode(SH_MODLOBBY_CODE_PROGRESS + 40, &state, &percent) && state == SH_MODLOBBY_DOWNLOADING && percent == 40);
    CHECK(sh_modlobby_decode(SH_MODLOBBY_CODE_HOLDS, &state, &percent) && state == SH_MODLOBBY_HOLDS);
    CHECK(sh_modlobby_decode(SH_MODLOBBY_CODE_FAILED, &state, &percent) && state == SH_MODLOBBY_FAILED);
    for (unsigned b = 3; b < 256; b++) {
        int s;
        if (sh_modlobby_decode(b, &s, NULL)) CHECK(b >= SH_MODLOBBY_CODE_PROGRESS);
    }
}

static void table(void)
{
    int percent;
    printf("-- the host keeps what each member said last; an engine byte is never a state\n");
    sh_modlobby_test_reset();
    CHECK(sh_modlobby_state_of(7, &percent) == SH_MODLOBBY_UNKNOWN);
    sh_modlobby_note(7, 1);                                  /* an engine READY_SET */
    CHECK(sh_modlobby_state_of(7, &percent) == SH_MODLOBBY_UNKNOWN);
    sh_modlobby_note(7, SH_MODLOBBY_CODE_PROGRESS + 25);
    CHECK(sh_modlobby_state_of(7, &percent) == SH_MODLOBBY_DOWNLOADING && percent == 25);
    sh_modlobby_note(7, SH_MODLOBBY_CODE_HOLDS);
    CHECK(sh_modlobby_state_of(7, &percent) == SH_MODLOBBY_HOLDS);
    for (uint64_t u = 100; u < 140; u++) sh_modlobby_note(u, SH_MODLOBBY_CODE_FAILED);   /* the table stays bounded */
    CHECK(sh_modlobby_state_of(139, NULL) == SH_MODLOBBY_FAILED);
    sh_modlobby_forget();
    CHECK(sh_modlobby_state_of(139, NULL) == SH_MODLOBBY_UNKNOWN);
}

static void gate(void)
{
    char out[256];
    sh_modlobby_member m[5] = {
        { "Sam", 1, 1 }, { "Alex", 2, 0 }, { "Robin", 3, 1 }, { "", 4, 1 }, { "Kai", 5, 0 },
    };
    printf("-- the host's start: held while a member has not answered the map check, with one sentence\n");
    sh_modlobby_test_reset();
    CHECK(sh_modlobby_gate(NULL, 0, out, sizeof out) == 1 && !out[0]);
    CHECK(sh_modlobby_gate(&m[1], 1, out, sizeof out) == 1);                 /* everyone answered */
    sh_modlobby_note(1, SH_MODLOBBY_CODE_PROGRESS + 40);
    sh_modlobby_note(3, SH_MODLOBBY_CODE_FAILED);
    CHECK(sh_modlobby_gate(m, 5, out, sizeof out) == 0);
    CHECK(!strcmp(out, "The match starts when every player has this map's mod content. Still getting it: "
                       "Sam (40%), Robin (could not get it) and a player."));
    CHECK(strlen(out) < 256);
    /* A member who said it holds the content but has not answered yet (its CacheReady runs) is still waited for. */
    sh_modlobby_note(1, SH_MODLOBBY_CODE_HOLDS);
    CHECK(sh_modlobby_gate(m, 1, out, sizeof out) == 0 && strstr(out, "Still getting it: Sam.") != NULL);

    printf("-- the host's waiting line names each member still getting the content\n");
    sh_modlobby_test_reset();
    sh_modlobby_note(1, SH_MODLOBBY_CODE_PROGRESS + 40);
    CHECK(sh_modlobby_waiting_line(m, 3, out, sizeof out) == 1);
    CHECK(!strcmp(out, "Waiting for this map's mod content: Sam 40%, Robin."));
    CHECK(sh_modlobby_waiting_line(&m[1], 1, out, sizeof out) == 0 && !out[0]);

    printf("-- long names give way to \"and N more\" within the dialog's 255 bytes\n");
    {
        static char names[8][64];
        sh_modlobby_member many[8];
        for (int i = 0; i < 8; i++) {
            memset(names[i], 'n', 60); names[i][60] = 0; names[i][0] = (char)('A' + i);
            many[i].name = names[i]; many[i].user = 50 + (uint64_t)i; many[i].pending = 1;
        }
        CHECK(sh_modlobby_gate(many, 8, out, sizeof out) == 0 && strlen(out) < 256 && strstr(out, "more.") != NULL);
    }
}

/* Every player plays the same bytes; stand-ins for content nobody can get are the same everywhere. */
static void standins(void)
{
    char out[256];
    int state = -1;
    sh_modlobby_member m[3] = { { "Sam", 1, 0 }, { "Alex", 2, 0 }, { "Robin", 3, 0 } };
    printf("-- the host and its joiners play the same content: stand-ins for what nobody can get, or the content\n");
    sh_modlobby_test_reset();
    CHECK(sh_modlobby_decode(SH_MODLOBBY_CODE_STANDS_IN, &state, NULL) && state == SH_MODLOBBY_STANDS_IN);
    /* solo: nothing to agree with */
    CHECK(sh_modlobby_standins_agree(1, NULL, 0, out, sizeof out) == 1 && !out[0]);
    CHECK(sh_modlobby_standins_agree(0, NULL, 0, out, sizeof out) == 1);
    /* the host stands in; joiners that stand in alike, or said nothing, agree */
    sh_modlobby_note(1, SH_MODLOBBY_CODE_STANDS_IN);
    CHECK(sh_modlobby_standins_agree(1, m, 3, out, sizeof out) == 1 && !out[0]);
    /* a joiner holds the real content the host cannot get: the start is refused, naming it */
    sh_modlobby_note(2, SH_MODLOBBY_CODE_HOLDS);
    CHECK(sh_modlobby_standins_agree(1, m, 3, out, sizeof out) == 0);
    CHECK(!strcmp(out, "The match cannot start: Alex has this map's mod content, which this computer cannot get, and "
                       "every player needs the same content. Ask Alex to host it.") || (printf("%s\n", out), 0));
    sh_modlobby_note(3, SH_MODLOBBY_CODE_HOLDS);
    CHECK(sh_modlobby_standins_agree(1, m, 3, out, sizeof out) == 0 && strstr(out, "Alex and Robin have") &&
          strstr(out, "Ask one of them to host it.") && strlen(out) < 256);
    /* the host holds the content; a joiner standing in for it cannot play the same bytes */
    CHECK(sh_modlobby_standins_agree(0, m, 3, out, sizeof out) == 0 && strstr(out, "Sam cannot get some of this map's") &&
          strlen(out) < 256);
    sh_modlobby_note(1, SH_MODLOBBY_CODE_HOLDS);
    CHECK(sh_modlobby_standins_agree(0, m, 3, out, sizeof out) == 1);
    sh_modlobby_test_reset();
}

int main(void)
{
    codes();
    table();
    gate();
    standins();
    printf("modlobby_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
