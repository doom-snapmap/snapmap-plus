/* String Set "smp:" commands: the literal parser and the argument typing by
 * an event's compact format string. Pure C, no game needed. */
#include <stdio.h>
#include <string.h>

#include "modaction.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static void test_prefix(void)
{
    CHECK(smp_is_command("smp:event x"));
    CHECK(smp_is_command("smp:"));
    CHECK(!smp_is_command("SMP:event x"));      /* case-sensitive: a vanilla string stays a string */
    CHECK(!smp_is_command(" smp:event x"));
    CHECK(!smp_is_command("hello"));
    CHECK(!smp_is_command(NULL));
}

static void test_parse(void)
{
    smp_command c;
    char err[128];

    CHECK(smp_parse("smp:event forcePraetorHandsAndBody 1", &c, err, sizeof err));
    CHECK(!strcmp(c.verb, "event") && c.argc == 2);
    CHECK(!strcmp(c.argv[0], "forcePraetorHandsAndBody") && !strcmp(c.argv[1], "1"));

    CHECK(smp_parse("smp:  doomhands\t0  ", &c, err, sizeof err));
    CHECK(!strcmp(c.verb, "doomhands") && c.argc == 1 && !strcmp(c.argv[0], "0"));

    CHECK(smp_parse("smp:event say \"two words\" \"\"", &c, err, sizeof err));
    CHECK(c.argc == 3 && !strcmp(c.argv[1], "two words") && c.argv[2][0] == 0);

    CHECK(smp_parse("smp:status", &c, err, sizeof err));
    CHECK(!strcmp(c.verb, "status") && c.argc == 0);

    CHECK(!smp_parse("smp:", &c, err, sizeof err) && strstr(err, "empty"));
    CHECK(!smp_parse("smp:   ", &c, err, sizeof err));
    CHECK(!smp_parse("smp:event \"open", &c, err, sizeof err) && strstr(err, "quote"));
    CHECK(!smp_parse("not a command", &c, err, sizeof err));
    CHECK(!smp_parse("smp:event e 1 2 3 4 5 6 7 8 9", &c, err, sizeof err) && strstr(err, "too many"));
    CHECK(smp_parse("smp:event e 1 2 3 4 5 6 7 8", &c, err, sizeof err) && c.argc == 9);

    char longtok[SMP_MAX_TOKEN + 20];
    memset(longtok, 'a', sizeof longtok);
    memcpy(longtok, "smp:x ", 6);
    longtok[sizeof longtok - 1] = 0;
    CHECK(!smp_parse(longtok, &c, err, sizeof err) && strstr(err, "too long"));

    char verb[64];
    snprintf(verb, sizeof verb, "smp:%s", "averbthatislongerthanthirtytwocharacters");
    CHECK(!smp_parse(verb, &c, err, sizeof err));
}

static void test_bool(void)
{
    int v = -1;
    CHECK(smp_parse_bool("1", &v) && v == 1);
    CHECK(smp_parse_bool("TRUE", &v) && v == 1);
    CHECK(smp_parse_bool("on", &v) && v == 1);
    CHECK(smp_parse_bool("yes", &v) && v == 1);
    CHECK(smp_parse_bool("0", &v) && v == 0);
    CHECK(smp_parse_bool("false", &v) && v == 0);
    CHECK(smp_parse_bool("Off", &v) && v == 0);
    CHECK(!smp_parse_bool("2", &v));
    CHECK(!smp_parse_bool("", &v));
}

static void test_typing(void)
{
    smp_command c;
    smp_arg a[SMP_MAX_ARGS];
    char err[128];

    /* forcePraetorHandsAndBody: format "b"; the bool travels as 'i'. */
    CHECK(smp_parse("smp:event forcePraetorHandsAndBody true", &c, err, sizeof err));
    CHECK(smp_type_args("b", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == 1);
    CHECK(a[0].type == 'i' && a[0].i == 1);

    /* spawnSingleAI: format "ies" (an enum travels as its int). */
    CHECK(smp_parse("smp:event spawnSingleAI 3 player1 wave_a", &c, err, sizeof err));
    CHECK(smp_type_args("ies", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == 3);
    CHECK(a[0].type == 'i' && a[0].i == 3);
    CHECK(a[1].type == 'e' && !strcmp(a[1].text, "player1"));
    CHECK(a[2].type == 's' && !strcmp(a[2].text, "wave_a"));

    /* Floats, hex ints and negative values. */
    CHECK(smp_parse("smp:event e 0.25 -7 0x10", &c, err, sizeof err));
    CHECK(smp_type_args("fii", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == 3);
    CHECK(a[0].type == 'f' && a[0].f > 0.249f && a[0].f < 0.251f);
    CHECK(a[1].i == -7 && a[2].i == 16);

    /* No arguments. */
    CHECK(smp_parse("smp:event stopRotating", &c, err, sizeof err));
    CHECK(smp_type_args("", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == 0);

    /* Refusals: count, value and unsupported types. */
    CHECK(smp_parse("smp:event forcePraetorHandsAndBody", &c, err, sizeof err));
    CHECK(smp_type_args("b", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == -1);
    CHECK(strstr(err, "takes 1 argument(s), got 0") != NULL);
    CHECK(smp_parse("smp:event forcePraetorHandsAndBody maybe", &c, err, sizeof err));
    CHECK(smp_type_args("b", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == -1);
    CHECK(strstr(err, "not a bool") != NULL);
    CHECK(smp_parse("smp:event e 1.5x", &c, err, sizeof err));
    CHECK(smp_type_args("f", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == -1);
    CHECK(smp_parse("smp:event e 12abc", &c, err, sizeof err));
    CHECK(smp_type_args("i", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == -1);
    CHECK(smp_parse("smp:event e \"\"", &c, err, sizeof err));
    CHECK(smp_type_args("e", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], c.argc - 1, a, err, sizeof err) == -1);
    CHECK(smp_parse("smp:event e 1 2 3", &c, err, sizeof err));
    CHECK(smp_type_args("v", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], 1, a, err, sizeof err) == -1);
    CHECK(strstr(err, "unsupported argument type 'v'") != NULL);
    CHECK(smp_type_args("iiiiiiiii", (const char (*)[SMP_MAX_TOKEN])&c.argv[1], 9, a, err, sizeof err) == -1);
}

int main(void)
{
    test_prefix();
    test_parse();
    test_bool();
    test_typing();
    if (g_fail) return 1;
    printf("modaction_test: ok\n");
    return 0;
}
