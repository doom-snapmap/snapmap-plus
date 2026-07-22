/* dumpmap_path_test.c -- pure-logic tests for sh_dumpmap's output-path resolution.
 *
 * The handler hands the resolved path straight to the map writer, so a mistake here writes a file
 * somewhere the user will never find it (or, worse, over something else). None of this needs the game.
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/backend/dumpmap_path.h"

static void test_validate(void)
{
    const char *why;

    assert(dumpmap_validate("arena", &why) == 1);
    assert(dumpmap_validate("arena/pass1", &why) == 1);
    assert(dumpmap_validate("a.b.c", &why) == 1);

    /* the mistake the command's wording invites: handing it a real Windows path */
    assert(dumpmap_validate("C:\\Users\\me\\map.map", &why) == 0 && why != NULL);
    assert(dumpmap_validate("\\\\server\\share", &why) == 0);
    assert(dumpmap_validate("/etc/passwd", &why) == 0);

    /* no escaping out of base\ */
    assert(dumpmap_validate("../../DOOMConfig", &why) == 0);
    assert(dumpmap_validate("sub/../../x", &why) == 0);

    assert(dumpmap_validate("", &why) == 0);
    assert(dumpmap_validate(NULL, &why) == 0);
}

static void test_stem(void)
{
    char s[260];

    /* a bare name gets the default subdirectory */
    assert(dumpmap_stem("arena", s, sizeof s) == 1);
    assert(strcmp(s, DUMPMAP_SUBDIR "/arena") == 0);

    /* a typed extension is dropped (the writer forces .map regardless) */
    assert(dumpmap_stem("arena.map", s, sizeof s) == 1);
    assert(strcmp(s, DUMPMAP_SUBDIR "/arena") == 0);
    assert(dumpmap_stem("arena.bak", s, sizeof s) == 1);
    assert(strcmp(s, DUMPMAP_SUBDIR "/arena") == 0);

    /* only the LAST extension goes */
    assert(dumpmap_stem("a.b.c", s, sizeof s) == 1);
    assert(strcmp(s, DUMPMAP_SUBDIR "/a.b") == 0);

    /* a name that picks its own folder keeps it -- no default subdirectory */
    assert(dumpmap_stem("arena/pass1", s, sizeof s) == 1);
    assert(strcmp(s, "arena/pass1") == 0);
    assert(dumpmap_stem("arena\\pass1", s, sizeof s) == 1);
    assert(strcmp(s, "arena\\pass1") == 0);

    /* a '.' in a DIRECTORY component is not an extension */
    assert(dumpmap_stem("my.dir/pass1", s, sizeof s) == 1);
    assert(strcmp(s, "my.dir/pass1") == 0);
    assert(dumpmap_stem("my.dir\\pass1", s, sizeof s) == 1);
    assert(strcmp(s, "my.dir\\pass1") == 0);

    /* nothing but a directory has no file to write */
    assert(dumpmap_stem("sub/", s, sizeof s) == 0);
    assert(dumpmap_stem("sub/.map", s, sizeof s) == 0);
}

static void test_candidate(void)
{
    char c[260];

    dumpmap_candidate("mapdumps/arena", 1, c, sizeof c);
    assert(strcmp(c, "mapdumps/arena.map") == 0);
    dumpmap_candidate("mapdumps/arena", 2, c, sizeof c);
    assert(strcmp(c, "mapdumps/arena_2.map") == 0);
    dumpmap_candidate("mapdumps/arena", 999, c, sizeof c);
    assert(strcmp(c, "mapdumps/arena_999.map") == 0);

    /* every candidate ends in .map, which is what makes the writer's own SetFileExtension a no-op:
     * pass it "x_2.map" and it strips ".map" and re-appends it, landing on the same name we probed */
    dumpmap_candidate("a.b", 2, c, sizeof c);
    assert(strcmp(c, "a.b_2.map") == 0);
}

static void test_ospath(void)
{
    char o[260];

    dumpmap_ospath("D:\\Games\\DOOM\\base", "mapdumps/arena.map", o, sizeof o);
    assert(strcmp(o, "D:\\Games\\DOOM\\base\\mapdumps\\arena.map") == 0);

    /* separators normalize so the printed path is a real, copy-pasteable Windows path */
    dumpmap_ospath("D:\\base", "a/b/c.map", o, sizeof o);
    assert(strcmp(o, "D:\\base\\a\\b\\c.map") == 0);
    dumpmap_ospath("D:\\base", "a\\b.map", o, sizeof o);
    assert(strcmp(o, "D:\\base\\a\\b.map") == 0);
}

static void test_truncation(void)
{
    /* a bounded buffer must truncate and stay NUL-terminated, never run off the end */
    char tiny[8];
    assert(dumpmap_stem("arena", tiny, sizeof tiny) == 1);
    assert(strlen(tiny) < sizeof tiny);

    dumpmap_candidate("aaaaaaaaaaaaaaaa", 2, tiny, sizeof tiny);
    assert(strlen(tiny) < sizeof tiny);

    dumpmap_ospath("D:\\some\\long\\base", "x.map", tiny, sizeof tiny);
    assert(strlen(tiny) < sizeof tiny);
}

int main(void)
{
    test_validate();
    test_stem();
    test_candidate();
    test_ospath();
    test_truncation();
    printf("dumpmap_path_test: OK\n");
    return 0;
}
