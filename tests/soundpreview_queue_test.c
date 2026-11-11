/* soundpreview_queue_test.c -- exact rollback and ordering for the main-thread audio queue. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/backend/soundpreview.c"

static int failures;
static int kick_count;
static int overflow_logs;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

void backend_log(const char *msg)
{
    if (msg && strstr(msg, "queue full")) overflow_logs++;
}

int sh_imgpreview_has(int kind, const char *name)
{
    (void)kind;
    return name && name[0];
}

uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name)
{
    (void)results; (void)n; (void)name;
    return 0;
}

uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{
    (void)module_base; (void)name; (void)out_status;
    return 0;
}

static void buffer_ok(void *cmdsys, const char *text)
{
    (void)cmdsys;
    CHECK(strcmp(text, SP_CMD_NAME "\n") == 0);
    kick_count++;
}

static void buffer_fault(void *cmdsys, const char *text)
{
    (void)cmdsys; (void)text;
    kick_count++;
    RaiseException(0xE0000001u, 0, 0, NULL);
}

static const sp_item *queue_at(int logical_index)
{
    return &g_queue[(g_qhead + logical_index) % SP_QUEUE_MAX];
}

static void reset_queue(void)
{
    AcquireSRWLockExclusive(&g_qlock);
    memset(g_queue, 0, sizeof g_queue);
    g_qhead = 0;
    g_qcount = 0;
    ReleaseSRWLockExclusive(&g_qlock);
    kick_count = 0;
    overflow_logs = 0;
    g_cmdsys = (void *)1;
    g_cmd_registered = 1;
}

/* A cvar system holding the three sound settings, laid out as the registry reader expects. */
static __declspec(align(16)) unsigned char fake_cvar[3][0x100];
static unsigned char *fake_rows[3];
static __declspec(align(16)) unsigned char fake_system[0x40];
static const unsigned char *fake_slot;
static char console_log[1024];

static void fake_exec(void *cmdsys, const char *text)
{
    (void)cmdsys;
    strcat_s(console_log, sizeof console_log, text);
    strcat_s(console_log, sizeof console_log, "\n");
}
static void (*fake_cmd_vtbl[16])(void *, const char *);
static void *fake_cmd_object = fake_cmd_vtbl;

static void fake_setting(int i, const char *name, int value, const char *text)
{
    memset(fake_cvar[i], 0, sizeof fake_cvar[i]);
    *(const char **)(fake_cvar[i] + SH_IDCVAR_NAME) = name;
    *(int *)(fake_cvar[i] + SH_IDCVAR_VALUE_INT) = value;
    *(int *)(fake_cvar[i] + 0x08) = (int)strlen(text);
    *(const char **)(fake_cvar[i] + 0x10) = text;
    fake_rows[i] = fake_cvar[i];
}

static void check_settings_restored(void)
{
    fake_setting(0, "s_playSoundInBackground", 0, "0");
    fake_setting(1, "s_forceListener", 2, "2");
    fake_setting(2, "s_soloSound", 0, "player_music");
    *(unsigned char ***)(fake_system + CVARSYS_LIST_ARRAY_OFF) = fake_rows;
    *(unsigned *)(fake_system + CVARSYS_LIST_COUNT_OFF) = 3;
    fake_slot = fake_system;
    g_cvar_slot = (const uint8_t *)&fake_slot;
    fake_cmd_vtbl[0x48 / sizeof fake_cmd_vtbl[0]] = fake_exec;
    g_cmdsys = &fake_cmd_object;

    console_log[0] = '\0';
    CHECK(sp_enter_mode() == 1);
    CHECK(strcmp(console_log, "s_playSoundInBackground 1\n") == 0);
    /* While previewing the engine moves solo and the listener; leaving puts the player's back. */
    fake_setting(2, "s_soloSound", 0, "preview_sound");
    console_log[0] = '\0';
    sp_leave_mode();
    CHECK(strcmp(console_log, "s_soloSound \"player_music\"\ns_forceListener 2\n"
                              "s_playSoundInBackground 0\n") == 0);
    console_log[0] = '\0';
    sp_leave_mode();                                   /* already off: nothing more is written */
    CHECK(console_log[0] == '\0');

    /* Settings that cannot be read are never overwritten. */
    *(unsigned *)(fake_system + CVARSYS_LIST_COUNT_OFF) = 2;
    CHECK(sp_enter_mode() == 0);
    CHECK(console_log[0] == '\0' && g_mode_on == 0);
    g_cvar_slot = NULL;
    g_cmdsys = (void *)1;
}

static void check_refusals(void)
{
    char why[200];
    g_preview = NULL;
    CHECK(sh_soundpreview_play("any") == 0);
    CHECK(sh_soundpreview_refusal(why, sizeof why) == 1 && strstr(why, "not available") != NULL);
    g_preview = (snd_preview_fn)1;
    g_slot = NULL;                                     /* no sound world: not in a map */
    CHECK(sh_soundpreview_play("any") == 0);
    CHECK(sh_soundpreview_refusal(why, sizeof why) == 1 && strcmp(why, WHY_NO_MAP) == 0);
    CHECK(sh_soundpreview_play("") == 0);
    g_preview = NULL;
}

int main(void)
{
    reset_queue();
    g_buffer_cmd = buffer_fault;
    CHECK(sp_post(SP_OP_PLAY, "failed") == 0);
    CHECK(g_qcount == 0);

    g_buffer_cmd = buffer_ok;
    CHECK(sp_post(SP_OP_PLAY, "next") == 1);
    CHECK(g_qcount == 1);
    CHECK(strcmp(queue_at(0)->name, "next") == 0);

    g_buffer_cmd = buffer_fault;
    CHECK(sp_post(SP_OP_STOP, NULL) == 0);
    CHECK(g_qcount == 1);
    CHECK(queue_at(0)->op == SP_OP_PLAY);
    CHECK(strcmp(queue_at(0)->name, "next") == 0);

    reset_queue();
    g_buffer_cmd = buffer_ok;
    for (int i = 0; i < SP_QUEUE_MAX; ++i) {
        char name[32];
        _snprintf_s(name, sizeof name, _TRUNCATE, "sound-%d", i);
        CHECK(sp_post(SP_OP_PLAY, name) == 1);
    }
    CHECK(g_qcount == SP_QUEUE_MAX);
    g_buffer_cmd = buffer_fault;
    CHECK(sp_post(SP_OP_PLAY, "refused-overflow") == 0);
    CHECK(g_qcount == SP_QUEUE_MAX);
    CHECK(strcmp(queue_at(0)->name, "sound-0") == 0);
    CHECK(strcmp(queue_at(SP_QUEUE_MAX - 1)->name, "sound-7") == 0);
    CHECK(overflow_logs == 0);

    g_buffer_cmd = buffer_ok;
    CHECK(sp_post(SP_OP_PLAY, "accepted-overflow") == 1);
    CHECK(g_qcount == SP_QUEUE_MAX);
    CHECK(strcmp(queue_at(0)->name, "sound-1") == 0);
    CHECK(strcmp(queue_at(SP_QUEUE_MAX - 1)->name, "accepted-overflow") == 0);
    CHECK(overflow_logs == 1);

    {
        char too_long[SP_NAME_CAP + 1];
        memset(too_long, 'x', sizeof too_long - 1);
        too_long[sizeof too_long - 1] = '\0';
        int before_count = g_qcount;
        int before_kicks = kick_count;
        CHECK(sp_post(SP_OP_PLAY, too_long) == 0);
        CHECK(g_qcount == before_count);
        CHECK(kick_count == before_kicks);
    }

    check_settings_restored();
    check_refusals();

    if (failures) {
        fprintf(stderr, "%d sound queue test(s) failed\n", failures);
        return 1;
    }
    puts("sound preview queue tests passed");
    return 0;
}
