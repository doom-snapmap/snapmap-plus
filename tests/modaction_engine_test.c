/* modaction_engine_test.c -- String Set "smp:" commands through their engine
 * half (modaction.c) against a faked engine table: the decl manager's event
 * interface with a small registry, ProcessEventArgs, the argument clear and
 * entity conversion, the player casts and the console. Any registered event
 * runs with the arguments its own signature types; an unknown event, a wrong
 * count or a value that does not parse prints one line and runs nothing. */
#include "../src/backend/modaction.c"

static int t_failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        t_failed++;                                                             \
    }                                                                           \
} while (0)

/* ------------------------------------------------------------------ console and link stand-ins */

static char g_console[8192];
void sh_printf(const char *fmt, ...)
{
    size_t at = strlen(g_console);
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g_console + at, sizeof g_console - at, _TRUNCATE, fmt, ap);
    va_end(ap);
}
static int console_lines(void)
{
    int n = 0;
    const char *p;
    for (p = g_console; *p; ++p) n += *p == '\n';
    return n;
}
void backend_log(const char *line) { (void)line; }
const char *cmd_argv(idCmdArgs *a, int n) { (void)a; (void)n; return NULL; }
int cmd_argc(idCmdArgs *a) { (void)a; return 0; }
int sh_safe_read(const uint8_t *src, uint8_t *dst, size_t n) { memcpy(dst, src, n); return 1; }
uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name) { (void)results; (void)n; (void)name; return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen) { (void)target; (void)detour; (void)stolen; return NULL; }
sh_patch_status hook_commit(void *tramp) { (void)tramp; return (sh_patch_status)0; }
int hook_unpatch(void *tramp) { (void)tramp; return 0; }

/* ------------------------------------------------------------------ the faked engine */

typedef struct fe_rec {                  /* an event record: +0x08 format, +0x2C args, +0x34 number */
    const char *name;
    const char *format;
    const char *types;
    const char *names;
    unsigned char pad[0x0c];
    int numargs;
    int pad2;
    int eventnum;
} fe_rec;

static fe_rec g_recs[2];
static unsigned char g_player[0x400];
static int g_fired, g_fired_n;
static fe_rec *g_fired_rec;
static ma_event_arg g_fired_args[SMP_MAX_ARGS];

static void *fe_find(void *self, const char *name)
{
    int i;
    (void)self;
    for (i = 0; i < 2; ++i) if (!strcmp(g_recs[i].name, name)) return &g_recs[i];
    return NULL;
}
static void *g_evmgr_vt[8] = { 0 };
static void *g_evmgr[1] = { g_evmgr_vt };
static void *fe_evmgr(void *self) { (void)self; return g_evmgr; }
static void *g_declmgr_vt[0x98 / 8] = { 0 };
static void *g_declmgr[1] = { g_declmgr_vt };
void *sh_typeinfo_get_declmgr(void) { return g_declmgr; }

static unsigned char g_gm[0x2BB3A8];
static void *g_logic[16];
static int g_logic_mgr;
void *sh_entity_gamemgr(void) { return g_gm; }
void *sh_entity_find(const char *name) { (void)name; return NULL; }

static void *fe_process(void *obj, void *ret, void *evdef, int n, ma_event_arg *a0, ma_event_arg *a1, ma_event_arg *a2,
                        ma_event_arg *a3, ma_event_arg *a4, ma_event_arg *a5, ma_event_arg *a6, ma_event_arg *a7)
{
    ma_event_arg *a[8];
    int i;
    (void)ret;
    a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; a[5] = a5; a[6] = a6; a[7] = a7;
    if (obj != g_player) return NULL;
    g_fired++;
    g_fired_rec = (fe_rec *)evdef;
    g_fired_n = n;
    for (i = 0; i < n; ++i) g_fired_args[i] = *a[i];
    return NULL;
}
static void fe_clear(void *arg) { (void)arg; }
static void *fe_entity(void *arg, void *entity) { (void)arg; (void)entity; return NULL; }
static void *fe_cast_player(void *obj) { return obj == g_player ? obj : NULL; }
static void *fe_cast_owned(void *obj) { (void)obj; return NULL; }
static const int g_no_variable = -1;

/* The build's RVAs point at the fakes: each is the fake's offset from g_base. */
uint32_t sh_host_rva(uint32_t vk, uint32_t gl)
{
    const void *p = NULL;
    (void)gl;
    if (vk == 0xB47350u) p = (const void *)fe_process;
    else if (vk == 0xB44640u) p = (const void *)fe_clear;
    else if (vk == 0xB44300u) p = (const void *)fe_entity;
    else if (vk == 0x14AF840u) p = (const void *)fe_cast_player;
    else if (vk == 0x142A230u) p = (const void *)fe_cast_owned;
    else if (vk == 0x236E9D0u) p = (const void *)&g_no_variable;
    return p ? (uint32_t)((uintptr_t)p - (uintptr_t)g_base) : 0;
}

static void fe_reset(void)
{
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    g_evmgr_vt[MA_EV_FIND_VSLOT / 8] = (void *)fe_find;
    g_declmgr_vt[MA_EVMGR_VSLOT / 8] = (void *)fe_evmgr;
    g_recs[0].name = "setPlayerThing"; g_recs[0].format = "ifb"; g_recs[0].numargs = 3; g_recs[0].eventnum = 901;
    g_recs[1].name = "forcePraetorHandsAndBody"; g_recs[1].format = "b"; g_recs[1].numargs = 1; g_recs[1].eventnum = 678;
    g_gm[MA_GM_ISSERVER_OFF] = 1;
    g_logic[MA_LOGIC_MANAGER_OFF / 8] = &g_logic_mgr;
    *(void **)(g_gm + MA_GM_LOGIC_OFF) = g_logic;
    *(const char **)(g_player + MA_ENTITY_NAME_OFF) = "player1";
    g_fired = 0;
    g_console[0] = 0;
}

/* Any registered event on the activating player, typed by its own format. */
static void test_any_event_runs(void)
{
    float f;
    fe_reset();
    ma_run_literal("smp:event setPlayerThing 7 2.5 yes", g_player);
    CHECK(g_fired == 1 && g_fired_rec == &g_recs[0] && g_fired_n == 3);
    memcpy(&f, &g_fired_args[1].value, 4);
    CHECK(g_fired_args[0].type == 'i' && g_fired_args[0].value == 7);
    CHECK(g_fired_args[1].type == 'f' && f == 2.5f);
    CHECK(g_fired_args[2].type == 'i' && g_fired_args[2].value == 1);   /* a bool travels as 'i' */
    CHECK(console_lines() == 1 && strstr(g_console, "SMP: event setPlayerThing(7, 2.5, yes) (number 901, format 'ifb') on player1: ok"));
    fe_reset();
    ma_run_literal("smp:event forcePraetorHandsAndBody 1", g_player);
    CHECK(g_fired == 1 && g_fired_rec == &g_recs[1]);
}

/* An unknown event, a wrong count or a value that does not parse: one line, nothing run. */
static void test_refusals_run_nothing(void)
{
    fe_reset();
    ma_run_literal("smp:event noSuchEvent 1", g_player);
    CHECK(!g_fired && console_lines() == 1 && strstr(g_console, "no event named 'noSuchEvent'; nothing done"));
    fe_reset();
    ma_run_literal("smp:event setPlayerThing 7 2.5", g_player);
    CHECK(!g_fired && console_lines() == 1 && strstr(g_console, "event takes 3 argument(s), got 2"));
    fe_reset();
    ma_run_literal("smp:event setPlayerThing seven 2.5 yes", g_player);
    CHECK(!g_fired && console_lines() == 1 && strstr(g_console, "not an int: 'seven'; nothing done"));
    fe_reset();
    ma_run_literal("smp:event setPlayerThing 7 2.5 maybe", g_player);
    CHECK(!g_fired && console_lines() == 1 && strstr(g_console, "not a bool: 'maybe'; nothing done"));
}

int main(void)
{
    test_any_event_runs();
    test_refusals_run_nothing();
    if (t_failed) {
        fprintf(stderr, "modaction_engine_test: %d check(s) failed\n--- console ---\n%s", t_failed, g_console);
        return 1;
    }
    printf("modaction_engine_test: all checks passed\n");
    return 0;
}
