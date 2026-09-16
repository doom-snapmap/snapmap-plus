#include "decl_md6_compose.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define INIT "init { mesh \"model.md6mesh\" offset ( 0 0 0 ) calcRefBoundsFromJoints 0 } "
#define MD6(i,g,e,a,p) "{" i "jointGroups {" g "} events {" e "} aliases {" a "} props {" p "}}"
#define G "damageGroup \"head\" { head } "
#define P "painGroup \"head\" = damageGroup \"head\" "
#define A "alias { name \"idle\" anim \"idle.md6anim\" } "
#define B "alias { name \"attack\" anim \"attack.md6anim\" } "
#define E "anim \"idle.md6anim\" { event \"ae_sound\" { frame 1 } event \"ae_sound\" { frame 1 } } "
/* One hit-test group whose single joint carries the given payload lines, one
 * mesh-kit or eye-info section, and one animation event body. The shipped cooked
 * format writes one key per line and these fixtures follow it. */
#define HIT(k) "hitTestGroup \"a\" { head {\n" k "} } "
#define SECTIONS(s) "{" INIT "jointGroups {} events {} aliases {} props {} " s "}"
#define KITS(k) SECTIONS("meshKits { " k " }")
#define EYES(e) SECTIONS("eyeInfoCollection " e)
#define EV(k) "anim \"a.md6anim\" { event \"ae_sound\" {\nframe 1\nrow 0\n" k "} } "
static sh_decl_source view(const char *s) { return (sh_decl_source){s, strlen(s)}; }
static char *flat(const char *s)
{
    char *out = _strdup(s); size_t n = 0; int quoted = 0;
    if (!out) return NULL;
    for (size_t i = 0; s[i]; i++) {
        if (!quoted && isspace((unsigned char)s[i])) {
            if (n && out[n - 1] != ' ') out[n++] = ' ';
            continue;
        }
        out[n++] = s[i];
        if (s[i] == '"') quoted = !quoted;
    }
    if (n && out[n - 1] == ' ') n--;
    out[n] = 0; return out;
}
static int contains(const char *out, const char *needle)
{
    char *copy = flat(needle), *body = flat(out);
    int found = copy && body && strstr(body, copy) != NULL;
    free(copy); free(body); return found;
}
static char *compose(const char *base, const char *a, const char *b, char *error)
{
    sh_decl_source sources[] = {view(a), view(b)};
    size_t length = 999; sh_decl_conflict conflict;
    char *out = sh_decl_md6_compose(view(base), sources, 2, &length, error, 512, &conflict);
    CHECK(out ? strlen(out) == length : length == 0); return out;
}
static void pass(const char *base, const char *a, const char *b, const char *x, const char *y)
{
    char error[512], *out = compose(base, a, b, error), *reverse;
    CHECK(out);
    if (!out) { fprintf(stderr, "unexpected refusal: %s\n", error); return; }
    if (x) CHECK(contains(out, x)); if (y) CHECK(contains(out, y));
    CHECK(!strstr(out, "item[") && !strstr(out, "target =") && !strstr(out, "edit ="));
    reverse = compose(base, b, a, error); CHECK(reverse && !strcmp(reverse, out));
    free(reverse); free(out);
}
static void refuse(const char *base, const char *a, const char *b, const char *reason)
{
    char error[512], *out = compose(base, a, b, error);
    CHECK(!out && strstr(error, reason));
    if (out || !strstr(error, reason)) fprintf(stderr, "expected %s; got %s\n", reason, out ? out : error);
    free(out);
}
int main(void)
{
    pass(MD6(INIT,G,E,A,""), MD6(INIT,G P,E,A,""), MD6(INIT,G,E,A B,""), P, B);
    pass("{}", MD6(INIT,G P,E,A,""), MD6(INIT,G,E,A B,""), P, B);
    pass(MD6(INIT,G,E,A,""), MD6(INIT,G,E,A,"prop \"one\" { tag \"t\" {} }"),
        MD6(INIT,G P,E,A,""), "prop \"one\"", E);
    pass(MD6(INIT,G,E,A,""), MD6(INIT,"damageGroup \"head\" { neck } ",E,A,""),
        MD6(INIT,G,E,B,""), "{ neck }", B);
    pass(MD6(INIT,G,E,A,""), MD6(INIT,G,E "anim \"attack.md6anim\" { event \"ae_attack\" { frame 2 } }",A,""),
        MD6(INIT,G,E,A B,""), E, "event \"ae_attack\"");
    /* Animation variants append within one alias, and flags compose separately. */
    pass(MD6(INIT,G,"",A,""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"idle.md6anim\" anim \"look.md6anim\" }", ""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"idle.md6anim\" anim \"wait.md6anim\" }", ""),
        "anim \"look.md6anim\"", "anim \"wait.md6anim\"");
    pass(MD6(INIT,G,"",A,""),
        MD6(INIT,G,"","alias { name \"idle\" flags { forceLoad } anim \"idle.md6anim\" }", ""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"idle.md6anim\" anim \"wait.md6anim\" }", ""),
        "flags { forceLoad }", "anim \"wait.md6anim\"");
    refuse(MD6(INIT,G,"",A,""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"look.md6anim\" }", ""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"wait.md6anim\" }", ""), "same field");
    pass(MD6(INIT,G,"","alias { name \"idle\" anim \"a.md6anim\" anim \"b.md6anim\" }", ""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"x.md6anim\" anim \"b.md6anim\" }", ""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"a.md6anim\" anim \"y.md6anim\" }", ""),
        "anim \"x.md6anim\" anim \"y.md6anim\"", NULL);
    refuse(MD6(INIT,G,"","alias { name \"idle\" flags { forceLoad } anim \"idle.md6anim\" }",""),
        MD6(INIT,G,"",A,""),
        MD6(INIT,G,"","alias { name \"idle\" flags { forceLoad ignoredNativeFlag } anim \"idle.md6anim\" }", ""),
        "same field");
    refuse("{}", MD6(INIT,G,"","alias { name \"idle\" unknown \"x\" }", ""), "{}", "unknown field");
    refuse("{}", MD6(INIT,G,"","alias { name \"idle\" flags { { forceLoad } } }", ""), "{}", "nested alias flags");
    pass(MD6(INIT,G,"",A,""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"idle.md6anim\" anim \"clips\\one.md6anim\" }", ""),
        MD6(INIT,G,"","alias { name \"idle\" flags { forceLoad } anim \"idle.md6anim\" }", ""),
        "anim \"clips\\one.md6anim\"", "forceLoad");
    /* Literal trailing backslashes do not escape the closing quote. */
    pass("{}", MD6(INIT,G,"","alias { name \"idle\" anim \"clips\\\" }", ""),
        MD6(INIT,G,"","alias { name \"idle\" anim \"clips\\\" }", ""), "clips\\", NULL);
    /* The native list retains duplicate animations; equal added occurrences
     * from two owners do not double their multiplicity in the effective list. */
    {
        const char *base = MD6(INIT,G,"","alias { name \"idle\" anim \"idle.md6anim\" anim \"idle.md6anim\" }", "");
        const char *added = MD6(INIT,G,"","alias { name \"idle\" anim \"idle.md6anim\" anim \"idle.md6anim\" anim \"idle.md6anim\" }", "");
        char error[512], *out = compose(base, added, added, error);
        CHECK(out);
        if (out) {
            size_t count = 0; const char *p = out;
            while ((p = strstr(p, "anim \"idle.md6anim\""))) { count++; p++; }
            CHECK(count == 3);
        }
        free(out);
    }
    refuse(MD6(INIT,G,"",A,""), MD6(INIT,G G,"",A,""), MD6(INIT,G,"",A B,""), "duplicate named record");
    refuse(MD6(INIT,G,"",A,""), MD6(INIT,G,"",A A,""), MD6(INIT,G,"",A B,""), "duplicate named record");
    refuse(MD6(INIT,G,E,A,""), MD6(INIT,G,E E,A,""), MD6(INIT,G,E,A B,""), "duplicate named record");
    refuse(MD6(INIT,G,E,A,""), MD6(INIT,G,"anim \"/IDLE.MD6ANIM\" {} " E,A,""),
        MD6(INIT,G,E,A B,""), "duplicate named record");
    refuse(MD6(INIT,G,"",A,""), MD6(INIT,P G,"",A,""), MD6(INIT,G,"",A B,""), "earlier local group");
    refuse(MD6(INIT,G,"",A,""), MD6(INIT,"","",A,""), MD6(INIT,G P,"",A B,""), "earlier local group");
    /* Independent joint additions to one group combine; the removal both
     * contributions agree on still applies. */
    pass(MD6(INIT,G,"",A,""), MD6(INIT,"damageGroup \"head\" { neck } ","",A,""),
        MD6(INIT,"damageGroup \"head\" { spine } ","",A,""), "neck", "spine");
    /* Two different payloads for one joint remain a conflict, and different keys
     * of that payload combine. Each key owns the value tokens on its line, the
     * way the native reader takes them. */
    refuse(MD6(INIT,HIT("radius 1\n"),"",A,""), MD6(INIT,HIT("radius 2\n"),"",A,""),
        MD6(INIT,HIT("radius 3\n"),"",A,""), "same field");
    pass(MD6(INIT,HIT("radius 1\n"),"",A,""), MD6(INIT,HIT("radius 2\n"),"",A,""),
        MD6(INIT,HIT("radius 1\noffset ( 1 2 3 )\n"),"",A,""), "radius 2", "offset ( 1 2 3 )");
    /* A payload authored on one line is one native key: equal contributions
     * coalesce and divergent ones conflict at that whole key. */
    refuse(MD6(INIT,HIT("radius 1 "),"",A,""), MD6(INIT,HIT("radius 1 offset ( 1 2 3 ) "),"",A,""),
        MD6(INIT,HIT("radius 2 "),"",A,""), "same field");
    /* A repeated joint name is native: each occurrence keeps its own payload. */
    pass(MD6(INIT,"hitTestGroup \"a\" { head {\nradius 1\n} head {\nradius 2\n} } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { head {\nradius 1\n} head {\nradius 2\n} neck {\nradius 3\n} } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { head {\nradius 1\n} head {\nradius 9\n} } ","",A,""),
        "radius 9", "neck { radius 3 }");
    /* args sets the running default for the joints listed after it: an
     * independent args key composes, and a joint that would inherit a different
     * default after composition is a conflict. */
    pass(MD6(INIT,"hitTestGroup \"a\" { args {\nsurfType Metal\n} head {\nradius 1\n} } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { args {\nsurfType Metal\n} head {\nradius 2\n} } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { args {\nsurfType Metal\nvec3 ( 1 0 0 )\n} head {\nradius 1\n} } ","",A,""),
        "radius 2", "vec3 ( 1 0 0 )");
    refuse(MD6(INIT,"hitTestGroup \"a\" { args {\nvec3 ( 1 0 0 )\n} head } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { args {\nvec3 ( 1 0 0 )\n} head neck } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { args {\nvec3 ( 2 0 0 )\n} head } ","",A,""),
        "args default");
    /* surfType and contentsFlags are one group-wide value wherever they appear,
     * so a contribution whose write another one would overwrite is diagnosed. */
    refuse(MD6(INIT,"hitTestGroup \"a\" { head neck } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { head {\nsurfType Concrete\n} neck } ","",A,""),
        MD6(INIT,"hitTestGroup \"a\" { head neck {\nsurfType Metal\n} } ","",A,""), "surfType write");
    /* A limb-loss index is authored, so a composed group cannot claim one twice. */
    refuse(MD6(INIT,"limblossGroup \"a\" { head {\nindex 1\n} } ","",A,""),
        MD6(INIT,"limblossGroup \"a\" { head {\nindex 1\n} neck {\nindex 2\n} } ","",A,""),
        MD6(INIT,"limblossGroup \"a\" { head {\nindex 1\n} spine {\nindex 2\n} } ","",A,""),
        "claiming");
    /* Category names are matched the way the native reader matches them, misc is
     * its alias for eyeGroup, and a bare jointGroup token is ignored. */
    refuse(MD6(INIT,"eyeGroup \"a\" { head } ","",A,""),
        MD6(INIT,"EYEGROUP \"a\" { neck } ","",A,""),
        MD6(INIT,"misc \"a\" { spine } ","",A,""), "same field");
    pass(MD6(INIT,"jointGroup eyeGroup \"a\" { head } ","",A,""),
        MD6(INIT,"misc \"a\" { head neck } ","",A,""),
        MD6(INIT,"eyeGroup \"a\" { head spine } ","",A,""), "neck", "spine");
    /* The five obsolete group kinds the reader skips stay composable. */
    pass(MD6(INIT,G "reparentGroup \"a\" { head } ","",A,""),
        MD6(INIT,G "reparentGroup \"a\" { head } ","",A B,""),
        MD6(INIT,G P "reparentGroup \"a\" { head } ","",A,""), "reparentGroup \"a\"", P);
    /* Mesh kit entries from two owners combine and the native count is rebuilt. */
    pass(KITS("Gore 1 { \"a\" = \"a\" }"), KITS("Gore 2 { \"a\" = \"a\" \"b\" = \"b\" }"),
        KITS("Gore 2 { \"a\" = \"a\" \"c\" = \"c\" }"), "Gore 3 {", "\"c\" = \"c\"");
    refuse(KITS("Gore 1 { \"a\" = \"a\" }"), KITS("Gore 1 { \"a\" = \"b\" }"),
        KITS("Gore 1 { \"a\" = \"c\" }"), "same field");
    refuse("{}", KITS("Gore 0 {} Heads 0 {}"), "{}", "out-of-order mesh kit");
    /* An eye info record is positional, and the collection count is rebuilt. */
    pass(EYES("1 { eyeInfo {\njointName \"head\"\n} }"),
        EYES("2 { eyeInfo {\njointName \"head\"\n} eyeInfo {\njointName \"neck\"\n} }"),
        EYES("1 { eyeInfo {\njointName \"head\"\n} }"), "eyeInfoCollection 2", "jointName \"neck\"");
    /* Animation event payloads compose per key; one event is identified by its
     * frame command with the frame and row it is placed at. */
    pass(MD6(INIT,G,EV("locked 0\nsound \"one\"\n"),A,""),
        MD6(INIT,G,EV("locked 1\nsound \"one\"\n"),A,""),
        MD6(INIT,G,EV("locked 0\nsound \"two\"\n"),A,""), "locked 1", "sound \"two\"");
    refuse(MD6(INIT,G,EV("locked 0\nsound \"one\"\n"),A,""),
        MD6(INIT,G,EV("locked 0\nsound \"two\"\n"),A,""),
        MD6(INIT,G,EV("locked 0\nsound \"three\"\n"),A,""), "same field");
    /* Two events of one animation that differ only in frame or row are separate
     * records, so independent additions to that animation combine. */
    pass(MD6(INIT,G,"anim \"a.md6anim\" { event \"ae_sound\" {\nframe 1\nrow 0\n} } ",A,""),
        MD6(INIT,G,"anim \"a.md6anim\" { event \"ae_sound\" {\nframe 1\nrow 0\n} event \"ae_sound\" {\nframe 2\nrow 0\n} } ",A,""),
        MD6(INIT,G,"anim \"a.md6anim\" { event \"ae_sound\" {\nframe 1\nrow 0\n} event \"ae_sound\" {\nframe 1\nrow 3\n} } ",A,""),
        "frame 2", "row 3");
    /* A flag enum takes several constants on its line. */
    pass(MD6(INIT,G,EV("aiCollisionFlags_t AICF_WORLD AICF_AI\n"),A,""),
        MD6(INIT,G,EV("aiCollisionFlags_t AICF_WORLD AICF_AI AICF_PLAYERS\n"),A,""),
        MD6(INIT,G,EV("aiCollisionFlags_t AICF_WORLD AICF_AI\n"),A,""),
        "AICF_WORLD AICF_AI AICF_PLAYERS", NULL);
    refuse("{}", MD6(INIT,G,"anim \"a.md6anim\" { frame 1 } ",A,""), "{}", "only event records");
    refuse("{}", MD6(INIT,G,"anim \"a.md6anim\" { event \"ae_x\" {\nframe\n} } ",A,""), "{}",
        "no value on its line");
    refuse(MD6(INIT,G,"",A,""), MD6(INIT,"","",A,""),
        MD6(INIT,"damageGroup \"head\" { neck } ","",A,""), "same field");
    refuse(MD6(INIT,G,"",A,""), MD6("init { mesh \"other.md6mesh\" } ",G,"",A,""),
        MD6(INIT,G P,"",A,""), "binding changed");
    /* Offset changes do not select a different model. */
    pass(MD6(INIT,G,"",A,""), MD6("init { mesh \"model.md6mesh\" offset ( 1 2 3 ) } ",G,"",A,""),
        MD6(INIT,G P,"",A,""), "offset ( 1 2 3 )", P);
    /* Separate native init setters must not compete as one opaque block. */
    pass(MD6(INIT,G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 1 2 3 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 0 0 0 ) calcRefBoundsFromJoints 1 } ",G,"",A,""),
        "offset ( 1 2 3 )", "calcRefBoundsFromJoints 1");
    pass(MD6(INIT,G,"",A,""),
        MD6("init { mesh \"other.md6mesh\" offset ( 0 0 0 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 1 2 3 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        "mesh \"other.md6mesh\"", "offset ( 1 2 3 )");
    /* Inheritance writes the model, offset and bounds flag as it is read. */
    pass(MD6("init { inherit \"parent\" offset ( 0 0 0 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { inherit \"parent\" offset ( 1 2 3 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { inherit \"parent\" offset ( 0 0 0 ) calcRefBoundsFromJoints 1 } ",G,"",A,""),
        "offset ( 1 2 3 )", "calcRefBoundsFromJoints 1");
    refuse(MD6("init { mesh \"model.md6mesh\" } ",G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 1 2 3 ) } ",G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" inherit \"parent\" } ",G,"",A,""),
        "init write");
    /* Explicit values after inheritance remain independent of parent changes. */
    pass(MD6("init { inherit \"parent\" offset ( 0 0 0 ) } ",G,"",A,""),
        MD6("init { inherit \"other\" offset ( 0 0 0 ) } ",G,"",A,""),
        MD6("init { inherit \"parent\" offset ( 1 2 3 ) } ",G,"",A,""),
        "inherit \"other\"", "offset ( 1 2 3 )");
    refuse(MD6(INIT,G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 1 0 0 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 2 0 0 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        "same field");
    /* Repeated writes retain their sequence and final value. */
    pass(MD6("init { mesh \"model.md6mesh\" offset ( 0 0 0 ) offset ( 1 1 1 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 0 0 0 ) offset ( 2 2 2 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { mesh \"model.md6mesh\" offset ( 0 0 0 ) offset ( 1 1 1 ) calcRefBoundsFromJoints 1 } ",G,"",A,""),
        "offset ( 2 2 2 )", "calcRefBoundsFromJoints 1");
    pass(MD6("init { inherit \"parent\" inherit \"\" offset ( 0 0 0 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { inherit \"parent\" inherit \"\" offset ( 1 2 3 ) calcRefBoundsFromJoints 0 } ",G,"",A,""),
        MD6("init { inherit \"parent\" inherit \"\" offset ( 0 0 0 ) calcRefBoundsFromJoints 1 } ",G,"",A,""),
        "inherit \"\"", "calcRefBoundsFromJoints 1");
    /* A peer cannot expose another package's changed, formerly hidden write. */
    refuse(MD6("init { mesh \"model\" offset ( 0 0 0 ) offset ( 1 1 1 ) } ",G,"",A,""),
        MD6("init { mesh \"model\" offset ( 2 2 2 ) offset ( 1 1 1 ) } ",G,"",A,""),
        MD6("init { mesh \"model\" offset ( 0 0 0 ) } ",G,"",A,""), "init write");
    refuse("{}", "{ init {} props {} }", MD6(INIT,G,"",A,""), "missing required");
    refuse("{}", "{ unknown {} }", "{}", "unknown");
    refuse("{}", "{ init { mesh \"a\" } jointGroups {", "{}", "unclosed");
    refuse("{}", "{/*", "{}", "unterminated comment");
    refuse("{}", "{ init { mesh \"a\n\" } }", "{}", "control byte");
    refuse("{}", MD6(INIT,G,"","alias { anim \"a\" name \"x\" }", ""), "{}", "alias must begin");
    /* Optional opaque sections preserve nested syntax, counts and comments. */
    pass("{}", "{" INIT "jointGroups {} events {} aliases {} props {} eyeInfoCollection 0 {} meshKits { Gore 1 { \"a\" = \"b\" } }}",
        "{" INIT "jointGroups {} events {} aliases {} props {} eyeInfoCollection 0 {} meshKits { Gore 1 { \"a\" = \"b\" } }}",
        "eyeInfoCollection 0", "\"a\" = \"b\"");
    pass("{}", "{ init { inherit \"\" mesh \"m\" } jointGroups {} events {} aliases {} props {} baseUserChannel { \"idle\" } userChannelWeightGroupOverride { \"MD6_WEIGHTGROUP_ALL\" } rigs { \"body.md6rig\" } }",
        "{ init { inherit \"\" mesh \"m\" } jointGroups {} events {} aliases {} props {} baseUserChannel { \"idle\" } userChannelWeightGroupOverride { \"MD6_WEIGHTGROUP_ALL\" } rigs { \"body.md6rig\" } }",
        "inherit \"\"", "rigs { \"body.md6rig\" }");
    /* Delimiters in comments/quoted payloads do not change envelope nesting. */
    pass("{}", MD6(INIT,G,E,A,"prop \"a\" { /* } */ tag \"{[()] }\" { } }"),
        MD6(INIT,G,E,A,"prop \"a\" { /* } */ tag \"{[()] }\" { } }"), "/* } */", "tag \"{[()] }\"");
    refuse("{}", MD6(INIT,"damageGroup \"he\\ad\" {}","",A,""), "{}", "escaped");
    refuse("{}", "{ init { offset ( 0 0 0 ] } }", "{}", "mismatched");
    refuse("{}", "{ init { mesh \"m\" } jointGroups {} events {} props {} aliases {} }", "{}", "out-of-order");
    /* No fixed delimiter-stack budget or call-stack recursion. */
    {
        size_t depth = 4096, prefix, n, out_length;
        const char *start = "{" INIT "jointGroups {} events {} aliases {} props { ";
        char *large = malloc(strlen(start) + depth * 2 + 5), error[512], *out;
        CHECK(large);
        if (large) {
            prefix = strlen(start); memcpy(large, start, prefix); n = prefix;
            for (size_t i = 0; i < depth; i++) large[n++] = '{';
            for (size_t i = 0; i < depth; i++) large[n++] = '}';
            large[n++] = '}'; large[n++] = '}'; large[n] = 0;
            sh_decl_source same[] = {view(large), view(large)};
            out = sh_decl_md6_compose(view(large), same, 2, &out_length, error, sizeof(error), NULL);
            CHECK(out && out_length > depth * 2); free(out); free(large);
        }
    }
    printf("MD6 composition: %d failure(s)\n", failures); return failures != 0;
}
