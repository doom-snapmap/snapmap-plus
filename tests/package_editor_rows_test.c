#include "../src/backend/package_editor_rows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void add(sh_editor_rows *rows, const char *name, const char *text)
{ CHECK(sh_editor_rows_add_text(rows, name, text, strlen(text)) == 1); }

int main(void)
{
    sh_editor_rows shipped = {0}, package = {0}, unread = {0};
    sh_decl_carrier_limit limit;
    char reason[320];
    const char *why;

    /* Shipped roster: an abstract parent without entityDef, two placing rows
     * and a child row that only inherits one of them. */
    add(&shipped, "logic_base", "{ edit = { previewImage = \"icons/base\"; } }");
    add(&shipped, "audio/speaker", "{ inherit = \"logic_base\"; edit = { entityDef = \"stock/speaker\"; } }");
    add(&shipped, "prop/crate", "{ inherit = \"logic_base\"; edit = { entityDef = \"stock/crate\"; } }");
    add(&shipped, "audio/speaker_alt", "{ inherit = \"audio/speaker\"; edit = { previewImage = \"icons/alt\"; } }");
    /* A shipped entry repeating fields (fx/ambient_embers states fxInfo twice)
     * reads as the native reader applies it: the last occurrence counts. */
    add(&shipped, "fx/embers", "{ inherit = \"logic_base\"; edit = { fxInfo = { fxDecl = \"a\"; } entityDef = \"stock/old\"; "
        "fxInfo = { fxDecl = \"a\"; } } edit = { entityDef = \"stock/embers\"; } }");
    CHECK(shipped.unreadable == 0);
    CHECK(sh_editor_rows_add_text(&shipped, "broken", "{ edit = {", 10) == 0 && shipped.unreadable == 1);
    shipped.unreadable = 0; /* the unreadable path is exercised separately below */
    CHECK(sh_editor_rows_finish(&shipped, NULL));
    CHECK(shipped.placeable_count == 4); /* speaker twice (own and inherited), crate, embers */
    for (size_t i = 0; i < shipped.count; i++) if (!strcmp(shipped.items[i].name, "fx/embers"))
        CHECK(!strcmp(shipped.items[i].parent, "logic_base") && !strcmp(shipped.items[i].entity_def, "stock/embers") &&
            !shipped.items[i].plain);

    /* A base some shipped row places is admitted, with or without package rows. */
    CHECK(!sh_editor_rows_placement(&shipped, NULL, "smp/p0", NULL, "stock/speaker", &limit, reason, sizeof(reason)));
    /* A base no shipped row places would be rejected by the map validator. */
    why = sh_editor_rows_placement(&shipped, NULL, "smp/p0", NULL, "stock/hidden", &limit, reason, sizeof(reason));
    CHECK(why && limit == SH_DECL_CARRIER_ENGINE && strstr(why, "no SnapMap editor entry"));
    /* An abstract editor declaration places nothing. */
    why = sh_editor_rows_placement(&shipped, NULL, "smp/p0", NULL, "logic_base", &limit, reason, sizeof(reason));
    CHECK(why && limit == SH_DECL_CARRIER_ENGINE);

    /* Package rows (compiled spellings are private; authored ones may remain). */
    add(&package, "smp/row_plain", "{ inherit = \"audio/speaker\"; edit = { entityDef = \"smp/p1\"; } }");
    add(&package, "smp/row_extra", "{ inherit = \"audio/speaker\"; edit = { entityDef = \"smp/p2\"; previewImage = \"icons/p2\"; } }");
    add(&package, "smp/row_other", "{ inherit = \"prop/crate\"; edit = { entityDef = \"smp/p3\"; } }");
    add(&package, "smp/row_authored", "{ inherit = \"audio/speaker\"; edit = { entityDef = \"pkg/p4\"; } }");
    add(&package, "smp/row_chained", "{ inherit = \"smp/row_plain\"; edit = { previewImage = \"icons/p1\"; } }");
    add(&package, "smp/row_top", "{ inherit = \"audio/speaker\"; displayName = \"x\"; edit = { entityDef = \"smp/p6\"; } }");
    CHECK(sh_editor_rows_finish(&package, &shipped));

    /* A plain copy of the shipped row placing base converts. */
    CHECK(!sh_editor_rows_placement(&shipped, &package, "smp/p5", NULL, "stock/speaker", &limit, reason, sizeof(reason)));
    CHECK(!sh_editor_rows_placement(&shipped, &package, "smp/p4", "pkg/p4", "stock/speaker", &limit, reason, sizeof(reason)));
    /* Presentation added to the row, a copy of another shipped row, a row
     * chained through another package row, or members outside edit: kept. */
    why = sh_editor_rows_placement(&shipped, &package, "smp/p2", NULL, "stock/speaker", &limit, reason, sizeof(reason));
    CHECK(why && limit == SH_DECL_CARRIER_GAP && strstr(why, "smp/row_extra") && strstr(why, "not a plain copy"));
    why = sh_editor_rows_placement(&shipped, &package, "smp/p3", NULL, "stock/speaker", &limit, reason, sizeof(reason));
    CHECK(why && limit == SH_DECL_CARRIER_GAP && strstr(why, "smp/row_other"));
    why = sh_editor_rows_placement(&shipped, &package, "smp/p1", NULL, "stock/speaker", &limit, reason, sizeof(reason));
    CHECK(why && limit == SH_DECL_CARRIER_GAP && strstr(why, "smp/row_chained"));
    why = sh_editor_rows_placement(&shipped, &package, "smp/p6", NULL, "stock/speaker", &limit, reason, sizeof(reason));
    CHECK(why && limit == SH_DECL_CARRIER_GAP && strstr(why, "smp/row_top"));

    /* When part of the shipped roster is unreadable, absence is unproven. */
    add(&unread, "audio/speaker", "{ edit = { entityDef = \"stock/speaker\"; } }");
    CHECK(sh_editor_rows_add_text(&unread, "broken", "{ edit = {", 10) == 0);
    CHECK(sh_editor_rows_finish(&unread, NULL));
    why = sh_editor_rows_placement(&unread, NULL, "smp/p0", NULL, "stock/crate", &limit, reason, sizeof(reason));
    CHECK(why && limit == SH_DECL_CARRIER_GAP && strstr(why, "could not be read"));
    CHECK(!sh_editor_rows_placement(&unread, NULL, "smp/p0", NULL, "stock/speaker", &limit, reason, sizeof(reason)));

    sh_editor_rows_free(&shipped); sh_editor_rows_free(&package); sh_editor_rows_free(&unread);
    if (failures) { fprintf(stderr, "%d editor row check(s) failed\n", failures); return 1; }
    puts("editor row placement checks passed");
    return 0;
}
