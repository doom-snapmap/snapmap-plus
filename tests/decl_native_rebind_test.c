#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decl_native_rebind.h"

typedef struct fixture { char replacement[512]; const char *family; size_t calls; int keep, refuse; } fixture;
static int bind(void *context, const char *family, const char *name, const char **replacement)
{
    fixture *f = context;
    f->calls++;
    if (f->refuse) return -1;
    if (f->keep || (f->family && strcmp(f->family, family))) return 0;
    snprintf(f->replacement, sizeof(f->replacement), "scope/red/%s", name);
    *replacement = f->replacement; return 1;
}
static int equal(sh_decl_source text, const char *value)
{ return text.length == strlen(value) && !memcmp(text.text, value, text.length); }
static int schema_read(void *context, const char *family, sh_decl_source name, int *kind)
{
    (void)context;
    if (!strcmp(family, "renderparm")) {
        if (name.length > 10 && !memcmp(name.text, "scope/red/", 10)) { name.text += 10; name.length -= 10; }
        if (equal(name, "color") || equal(name, "time")) *kind = 0;
        else if (equal(name, "tex")) *kind = 1;
        else if (equal(name, "sample")) *kind = 10;
        else if (equal(name, "program")) *kind = 11;
        else if (equal(name, "label")) *kind = 12;
        else if (equal(name, "VertexData")) *kind = 14;
        else return 0;
    }
    return 1;
}
static void material(void)
{
    const char *text = "{\n // exact comment\n color.xy {1,2}\n tex clamp linear \"models/rock/body.tga\"\n"
        " label models/rock/body.tga\n sample nearest\n}";
    sh_decl_material_schema schema = {NULL, schema_read};
    sh_decl_source source = {text, strlen(text)};
    fixture f = {0};
    char error[256], *out, *again;
    size_t length, changed, copied;
    out = sh_decl_native_rebind(SH_BINDING_MATERIAL, source, &schema, bind, &f,
        &length, &changed, error, sizeof(error));
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out && changed == 5);
    assert(!strcmp(out, "{\n // exact comment\n scope/red/color.xy {1,2}\n scope/red/tex clamp linear \"scope/red/models/rock/body.tga\"\n"
        " scope/red/label models/rock/body.tga\n scope/red/sample nearest\n}"));
    f.keep = 1;
    again = sh_decl_native_rebind(SH_BINDING_MATERIAL, (sh_decl_source){out, length}, &schema, bind, &f,
        &copied, &changed, error, sizeof(error));
    assert(again && copied == length && !changed && !memcmp(again, out, length));
    free(again); free(out);
    text = "{ StructuredBuffer writable VertexData coherent }";
    f = (fixture){0};
    out = sh_decl_native_rebind(SH_BINDING_RENDERPARM, (sh_decl_source){text, strlen(text)}, &schema,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(!out && !length && !changed && f.calls == 1);
    f.keep = 1;
    out = sh_decl_native_rebind(SH_BINDING_RENDERPARM, (sh_decl_source){text, strlen(text)}, &schema,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(out && !strcmp(out, text) && !changed); free(out);
}

static void md6(void)
{
    const char *text = "{ init { inherit \"actors/base\" mesh \"models/robot.md6mesh\" }\n"
        "events { anim \"clip\" { event \"event_label\" {\n entityDef \"actors/tool\"\n string \"actors/tool\"\n joint \"actors/tool\"\n } } }\n"
        "aliases { alias { name \"models/robot.md6anim\" anim \"models/robot.md6anim\" } } }";
    fixture f = {0};
    size_t length, changed;
    char error[256], *out;
    out = sh_decl_native_rebind(SH_BINDING_MD6_DEFINITION, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out && changed == 4 && f.calls == 4);
    assert(strstr(out, "inherit \"scope/red/actors/base\"") && strstr(out, "mesh \"scope/red/models/robot.md6mesh\""));
    assert(strstr(out, "entityDef \"scope/red/actors/tool\"") && strstr(out, "string \"actors/tool\"") && strstr(out, "joint \"actors/tool\""));
    assert(strstr(out, "name \"models/robot.md6anim\" anim \"scope/red/models/robot.md6anim\""));
    free(out);
    f.keep = 1;
    out = sh_decl_native_rebind(SH_BINDING_MD6_DEFINITION, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(out && !strcmp(out, text) && !changed); free(out);
    text = "{ init { mesh \"models/robot.md6mesh\" unknownField 1 } }";
    f.keep = 0;
    out = sh_decl_native_rebind(SH_BINDING_MD6_DEFINITION, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(!out && !length && !changed && error[0]);
}
static void table(void)
{
    const char *valid[] = {"{}", "{\n // numeric curve\n SNAP clamp spline min -2 max 3 left -.5 right 1e+2\n"
        " { -1:0, .5, 3:2e-3 }\n}", "{ { 0 } { 1, 2 } }"};
    const char *invalid[] = {"{ { } }", "{ { 1, } }", "{ { 1 2 } }", "{ { 1: } }", "{ { 1::2 } }",
        "{ model \"not/a/table\" }", "{ min \"2\" }", "{ { \"asset/name\" } }", "{ { 0 }; }", "{} extra"};
    fixture f = {0};
    char error[256], *out, many[256] = "{ { ";
    size_t length, changed;
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        out = sh_decl_native_rebind(SH_BINDING_TABLE, (sh_decl_source){valid[i], strlen(valid[i])}, NULL,
            bind, &f, &length, &changed, error, sizeof(error));
        assert(out && !changed && !f.calls && length == strlen(valid[i]) && !memcmp(out, valid[i], length));
        free(out);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        out = sh_decl_native_rebind(SH_BINDING_TABLE, (sh_decl_source){invalid[i], strlen(invalid[i])}, NULL,
            bind, &f, &length, &changed, error, sizeof(error));
        assert(!out && !length && !changed && error[0] && !f.calls);
    }
    for (size_t i = 0; i < 64; i++) strcat(many, i ? ",0" : "0");
    strcat(many, " } }");
    out = sh_decl_native_rebind(SH_BINDING_TABLE, (sh_decl_source){many, strlen(many)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(out && !changed && !f.calls); free(out);
    strcpy(many + strlen(many) - 4, ",0 } }");
    out = sh_decl_native_rebind(SH_BINDING_TABLE, (sh_decl_source){many, strlen(many)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(!out && !length && !changed && strstr(error, "64 curve points"));
}
static void articulated_figure(void)
{
    const char *text = "{\n// preserve labels and ignored authoring hints\n"
        "settings { mesh \"models/robot\" anim \"models/robot\" model \"models/robot\" skin \"\" "
        "clipMaterial \"surfaces/body\" base root defaultBouncyness 0 defaultLinearFriction .5 "
        "defaultAngularFriction .5 defaultContactFriction .5 defaultConstraintFriction .1 "
        "suspendSpeed 20,30,40,60 noMoveTime 1 noMoveTranslation 10 noMoveRotation 10 "
        "minMoveTime -1 maxMoveTime 15 contents corpse,solid clipMask solid selfCollision false "
        "solverConstants { errorReduction .5 maxLcpEpsilon 1e-4 universalTorsionLcpEpsilon .01 } "
        "syncSolverConstants { minTwistErrorReduction .1 maxTwistErrorReduction .2 } }\n"
        "body \"models/robot\" { joint \"models/robot\" mod orientation "
        "model custom(\"shapes/body\",(-1,-1,-1),(1,1,1)) origin -joint(root) "
        "angles (0,0,0) density 1 inertiaScale (1 0 0 0 1 0 0 0 1) "
        "linearFriction .5 angularFriction .5 contactFriction .5 contents corpse clipMask solid "
        "selfCollision 0 bouncyness 0 noSyncCollide true clearClipMaskInSolid 1 "
        "containedJoints \"shapes/body\" frictionDirection bonecenter(root,\"tip\") "
        "contactMotorDirection bonedir(\"root\",tip) }\n"
        "fixed \"shapes/body\" { body1 \"models/robot\" body2 \"world\" syncConstraint 0 }\n"
        "ballAndSocketJoint \"ball\" { body1 \"models/robot\" body2 \"world\" anchor (0,0,0) "
        "shafts (1,0,0),(0,1,0) conelimit (1,0,0),45,(0,0,1) "
        "pyramidlimit (1,0,0),10,20,30,(0,0,1) limitFudge false friction .1 "
        "syncConstraint true minTwist 1 maxTwist 80 }\n"
        "universalJoint \"u\" { body1 \"models/robot\" body2 \"world\" anchor joint(root) "
        "shafts (1,0,0),(0,1,0) conelimit (1,0,0),45 pyramidlimit (1,0,0),10,20,30 "
        "friction .1 syncConstraint 0 limitFudge 1 minTwist 1 maxTwist 80 }\n"
        "hinge \"h\" { body1 \"models/robot\" body2 \"world\" anchor (0,0,0) axis (1,0,0) "
        "shafts (1,0,0),(0,1,0) limit 0,45,90,1 friction .1 syncConstraint 0 "
        "limitFudge 1 minTwist 1 maxTwist 80 }\n"
        "slider \"s\" { body1 \"models/robot\" body2 \"world\" axis (1,0,0) friction .1 syncConstraint 0 }\n"
        "spring \"p\" { body1 \"models/robot\" body2 \"world\" anchor1 (0,0,0) anchor2 joint(tip) "
        "friction .1 stretch 2 compress 3 damping 4 restLength 5 minLength 1 maxLength 8 syncConstraint 0 }\n}";
    fixture f = {0};
    char error[256], *out;
    size_t length, changed;
    out = sh_decl_native_rebind(SH_BINDING_ARTICULATED_FIGURE, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out && changed == 3 && f.calls == 3);
    assert(strstr(out, "model \"scope/red/models/robot\"") &&
        strstr(out, "clipMaterial \"scope/red/surfaces/body\"") &&
        strstr(out, "custom(\"scope/red/shapes/body\""));
    assert(strstr(out, "mesh \"models/robot\" anim \"models/robot\"") &&
        strstr(out, "body \"models/robot\" { joint \"models/robot\"") &&
        strstr(out, "containedJoints \"shapes/body\"") && strstr(out, "fixed \"shapes/body\""));
    free(out);
    for (size_t i = 0; i < 4; i++) {
        const char *families[] = {"md6def", "material", "cm", "unrelated"};
        f.family = families[i]; f.calls = 0;
        out = sh_decl_native_rebind(SH_BINDING_ARTICULATED_FIGURE, (sh_decl_source){text, strlen(text)}, NULL,
            bind, &f, &length, &changed, error, sizeof(error));
        assert(out && changed == (i < 3 ? 1 : 0) && f.calls == 3); free(out);
    }
    f.keep = 1;
    out = sh_decl_native_rebind(SH_BINDING_ARTICULATED_FIGURE, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(out && !changed && length == strlen(text) && !memcmp(out, text, length)); free(out);
    {
        const char *shapes[] = {"box", "octahedron", "dodecahedron", "cylinder", "cone", "bone"};
        char source[256];
        for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
            snprintf(source, sizeof(source), "{ body \"body\" { joint \"root\" model %s((0,0,0),(1,1,1)%s) } }",
                shapes[i], i < 3 ? "" : i < 5 ? ",8" : ",.5");
            f.calls = 0;
            out = sh_decl_native_rebind(SH_BINDING_ARTICULATED_FIGURE, (sh_decl_source){source, strlen(source)}, NULL,
                bind, &f, &length, &changed, error, sizeof(error));
            assert(out && !changed && !f.calls && !strcmp(out, source)); free(out);
        }
    }
    {
        const char *invalid[] = {"{ settings { model \"models/robot\" unknown \"hidden/asset\" } }",
            "{ settings { model \"models/robot\" skin \"unverified/skin\" } }",
            "{ body \"body\" { model custom(\"shapes/body\",(0,0,0)) } }",
            "{ body \"body\" { model cylinder((0,0,0),(1,1,1),.5) } }",
            "{ body \"body\" { origin joint(root,tip) } }",
            "{ body \"body\" { inertiaScale (1,0,0,0,1,0,0,0,1) } }",
            "{ settings { suspendSpeed 1,2,3 } }",
            "{ settings { solverConstants { model \"hidden/asset\" } } }",
            "{ settings { model models/robot } }", "{ future { model \"hidden/asset\" } }",
            "{ settings { model \"models/robot\" }", "{} trailing",
            "{ universalJoint \"u\" { conelimit (0,0,1),45,(1,0,0) } }",
            "{ ballAndSocketJoint \"b\" { conelimit (0,0,1),45 } }"};
        f = (fixture){0};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            out = sh_decl_native_rebind(SH_BINDING_ARTICULATED_FIGURE, (sh_decl_source){invalid[i], strlen(invalid[i])},
                NULL, bind, &f, &length, &changed, error, sizeof(error));
            assert(!out && !changed && !length && error[0]);
        }
    }
}
typedef struct breakable_sites { const char *source; size_t count; } breakable_sites;
static int breakable_site(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    static const char *const families[] = {"discreteanimation", "particle", "particle", "particle",
        "particle", "fx", "particle", "particle", "particle", "material", "material", "material",
        "material", "particle", "particle", "material"};
    breakable_sites *sites = context;
    assert(sites->count < sizeof(families) / sizeof(families[0]));
    assert(!strcmp(family, families[sites->count++]));
    assert(strlen(name) == length && !memcmp(sites->source + offset, name, length));
    return 1;
}
static void breakable(void)
{
    const char *text = "{\n // references and local labels deliberately use the same spelling\n"
        "model \"models\\piece.lwo\" impactParticle effects/a deathParticle \"effects/b\" harmlessParticle effects/c "
        "pieceNames { 1 \"models\\piece.lwo\", 2 \"effects/a\" } useablePieces {1 0} touchingBasePieces {0} "
        "armoredPieces {1 2, 2 3} healthPieces {1 8, 2 9} damageSpheres {(0 0 0 2)} "
        "gravity 0 0 -750 linearFriction .5 angularFriction .6 contactFriction .7 "
        "linearFrictionWater 1 angularFrictionWater 1 bouncyness .3 dampeningDecay .2 "
        "worldCollisionOnly true simplePointCollision 0 noShadows false crazyBounceChance .1 "
        "maxSimulationTime 9 stopSpeed 1 maxLinearVelocity 100 maxAngularVelocity 90 clipMask solid,corpse "
        "explosion { name \"effects/a\" particle effects/d fx fx/impact position -1 0 1 radius 10 "
        "impulse 4 falloff .5 angular 1 delay 0 activateAllDelay 2 duration 3 excludePieces {1 2} } "
        "trail { name effects/a particleTrail effects/e particleBounce effects/f particleDie effects/g "
        "glowQuadMaterial materials/a glowQuadSize 8 fadeInDuration .2 fadeOutDuration .3 "
        "minTrailVelocity 0 minBounceVelocity 0 trailSpacing 4 trailVelocityScale 1 "
        "delay 0 duration 3 randomPercentage 70 pieces {all} } "
        "decal { name materials/a maxSizeX 8 maxSizeY 9 minSizeX 5 minSizeY 6 lifetime 2 fadetime 1 "
        "fadeInEndTime .1 decalSpacing 1 minBounceVelocity 0 splatterOnBounce true splatterOnRest false "
        "depth 1 dripMinSizeX 6 dripMinSizeY 7 floorDeclMinSizeX 8 floorDeclMinSizeY 9 "
        "materials {materials/b \"materials/c\"} dripMaterials {materials/d} ceilingParticles {effects/h} "
        "floorParticleFromCeilingDrip {effects/i} floorDecalFromCeilingDrip {materials/e} pieces {1 2} }\n}";
    char error[256], *out;
    size_t length, changed;
    fixture f = {0}; breakable_sites sites = {text, 0};
    sh_decl_native_binding_format format;
    assert(sh_decl_native_binding_family("Breakable", &format) && format == SH_BINDING_BREAKABLE);
    assert(!sh_decl_native_binding_family("unregistered", NULL));
    assert(sh_decl_native_reference_sites(format, (sh_decl_source){text, strlen(text)}, NULL,
        breakable_site, &sites, error, sizeof(error)) && sites.count == 16);
    out = sh_decl_native_rebind(format, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    if (!out) fprintf(stderr, "breakable: %s\n", error);
    assert(out && changed == 16 && f.calls == 16);
    assert(strstr(out, "model \"scope/red/models\\piece.lwo\""));
    assert(strstr(out, "pieceNames { 1 \"models\\piece.lwo\", 2 \"effects/a\" }"));
    assert(strstr(out, "name \"effects/a\" particle scope/red/effects/d"));
    assert(strstr(out, "materials {scope/red/materials/b \"scope/red/materials/c\"}"));
    assert(strstr(out, "gravity 0 0 -750") && strstr(out, "clipMask solid,corpse")); free(out);
    f = (fixture){0}; f.keep = 1;
    out = sh_decl_native_rebind(format, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(out && !changed && f.calls == 16 && length == strlen(text) && !memcmp(out, text, length)); free(out);
    text = "{ model models/part.lwo explosion { particle \"\" fx \"\" } decal { materials {} } }";
    f = (fixture){0};
    out = sh_decl_native_rebind(format, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(out && f.calls == 1 && changed == 1); free(out);
    {
        const char *invalid[] = {"{ model }", "{ particleTrail effects/a }", "{ model a; }",
            "{ gravity 1 2 }", "{ explosion { material a } }", "{ trail { particleTrail } }",
            "{ decal { materials {a, b} } }", "{ decal { materials { a } }", "{ model a } trailing"};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            out = sh_decl_native_rebind(format, (sh_decl_source){invalid[i], strlen(invalid[i])}, NULL,
                bind, &f, &length, &changed, error, sizeof(error));
            assert(!out && !length && !changed && error[0]);
        }
    }
}
static void animation_web(void)
{
    const char *text = "{\nprops { editPos (1 -2 3) gridSize 32 showGrid true "
        "alwaysShowIntraSubwebLinks false snapToGrid 1 alwaysShowNodeProps 0 "
        "showAbbreviatedNodeProps 0 showIncomingLinks 1 defaultBlendOutWindow 100 "
        "defaultBlendDuration 200 defaultBlendType BLEND_LINEAR subWebBlend \"sourceNode\" "
        "mergeModels true showAnimName 1 showWrapMode 0 showRate 1 showDelta 1 showBlendEq 0 "
        "showTransition 0 showWeakRefs 1 models {\"models/robot\"} "
        "modelInfos {\"models/other\" \"models/robot\"} }\n"
        "layers {layer \"models/robot\" {visible true subwebs {\"anims/move.md6anim\"}}}\n"
        "states {state \"anims/move.md6anim\" {destination} state \"idle\" {}}\n"
        "scalars {speed 1 turn 0}\nsubWebs {subWeb \"anims/move.md6anim\" {"
        "props {color (1 0 .5 1) hub false visible true}\nnode \"anims/move.md6anim\" \"idle\" {"
        "props {pos (0 0 0) delta DELTA_DEFAULT startTransition stopTransition transition "
        "stepTransition turnTransition genericTransition focusTransition\n"
        "customFlags TEST_FLAG\nmetaData = { ignored = { anim = \"anims/move.md6anim\"; }; } }\n"
        "blendTrees {tree {modelIndex 1 blendEq \"anim0\" "
        "tagGroup \"anims/move.md6anim\" {tag \"models/robot\" 1} anims {"
        "alias {name \"anims/move.md6anim\" wrap WRAP_REPEAT rate .5 randomStart "
        "coordinate (-1, .5, 2)\ntags models/robot\n} "
        "alias {name \"models/robot\" wrap clamp} alias {name \"_default.md6anim\"} "
        "alias {name \"_DEFAULT.md6anim\"} alias {name \"anims\\turn.MD6ANIM\"} }} }\n"
        "edges {edge {toState \"anims/move.md6anim\" toSubWeb \"anims/move.md6anim\" "
        "weightScale .5 randomizeWeight true blendParms {srcAnim \"anims/start.md6anim\" "
        "exceptSrcAnim \"anims/skip.md6anim\" destAnim \"anims/stop.md6anim\" "
        "exceptLooping sourceStartFrame -1 sourceDuration 4 destStartFrame 1 destDuration 3 "
        "duration 100 durationf 3 srcFirstFrame 0 srcLastFrame 4 destFrame 1 "
        "originBlend ORIGINBLEND_DEFAULT blendType BLEND_LINEAR sourceEndRelative destEndRelative} "
        "customFlags TEST_FLAG\n} edge {toState \"idle\" blendParms {srcAnim \"\" destAnim \"\"}}} }}}}";
    fixture f = {0}; char error[256], *out; size_t length, changed;
    sh_decl_native_binding_format format;
    assert(sh_decl_native_binding_family("AnimWeb", &format) && format == SH_BINDING_ANIMATION_WEB);
    out = sh_decl_native_rebind(format, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    if (!out) fprintf(stderr, "animweb: %s\n", error);
    assert(out && changed == 8 && f.calls == 8);
    assert(strstr(out, "models {\"scope/red/models/robot\"}"));
    assert(strstr(out, "modelInfos {\"scope/red/models/other\" \"models/robot\"}"));
    assert(strstr(out, "alias {name \"models/robot\"") && strstr(out, "alias {name \"_default.md6anim\"}"));
    assert(strstr(out, "alias {name \"scope/red/_DEFAULT.md6anim\"}"));
    assert(strstr(out, "name \"scope/red/anims\\turn.MD6ANIM\""));
    assert(strstr(out, "toState \"anims/move.md6anim\" toSubWeb \"anims/move.md6anim\""));
    assert(strstr(out, "anim = \"anims/move.md6anim\";") && strstr(out, "tags models/robot\n"));
    assert(strstr(out, "srcAnim \"scope/red/anims/start.md6anim\"")); free(out);
    f = (fixture){0}; f.keep = 1;
    out = sh_decl_native_rebind(format, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(out && !changed && length == strlen(text) && !memcmp(out, text, length)); free(out);
    for (size_t i = 0; i < 3; i++) {
        const char *families[] = {"md6def", "anim", "animweb"};
        f = (fixture){0}; f.family = families[i];
        out = sh_decl_native_rebind(format, (sh_decl_source){text, strlen(text)}, NULL,
            bind, &f, &length, &changed, error, sizeof(error));
        assert(out && changed == (i == 0 ? 2 : i == 1 ? 6 : 0)); free(out);
    }
    f = (fixture){0}; f.refuse = 1;
    out = sh_decl_native_rebind(format, (sh_decl_source){text, strlen(text)}, NULL,
        bind, &f, &length, &changed, error, sizeof(error));
    assert(!out && !length && !changed);
    {
        const char *composite = "{ props {composite {\"webs/red\", webs/green} mergeModels true} }";
        f = (fixture){0}; f.family = "animweb";
        out = sh_decl_native_rebind(format, (sh_decl_source){composite, strlen(composite)}, NULL,
            bind, &f, &length, &changed, error, sizeof(error));
        assert(out && changed == 2 && f.calls == 2 && strstr(out, "\"scope/red/webs/red\", scope/red/webs/green"));
        free(out);
    }
    {
        const char *valid[] = {"{}", "{props {} states {} subWebs {}}", "{props {composite {}}}",
            "{props {} states {} subWebs {subWeb \"w\" {node \"s\" {props {} blendTrees {} edges {}}}}}"};
        f = (fixture){0};
        for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
            out = sh_decl_native_rebind(format, (sh_decl_source){valid[i], strlen(valid[i])}, NULL,
                bind, &f, &length, &changed, error, sizeof(error));
            assert(out && !changed && !f.calls && !strcmp(valid[i], out)); free(out);
        }
    }
    {
        const char *bad[] = {"{} trailing", "{props {models {unquoted}} states {} subWebs {}}",
            "{props {modelInfos {\"model\"}} states {} subWebs {}}", "{props {} subWebs {}}",
            "{props {} states {} subWebs {subWeb \"w\" {node \"s\" {props {newField \"a\"} blendTrees {} edges {}}}}}",
            "{props {} states {} subWebs {subWeb \"w\" {node \"s\" {props {} blendTrees {tree {blendEq \"anim0\" anims {alias {name \"bad.bmd6anim\"}}}} edges {}}}}}",
            "{props {} states {} subWebs {subWeb \"w\" {node \"s\" {props {} blendTrees {} edges {edge {destAnim \"a\"}}}}}}",
            "{props {} states {} subWebs {subWeb \"w\" {node \"s\" {props {customFlags FLAG } blendTrees {} edges {}}}}}",
            "{props {} states {} subWebs {subWeb \"w\" {node \"s\" {props {} blendTrees {} edges {edge {toState \"s\" blendParms {srcAnim}}}}}}}"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            out = sh_decl_native_rebind(format, (sh_decl_source){bad[i], strlen(bad[i])}, NULL,
                bind, &f, &length, &changed, error, sizeof(error));
            assert(!out && !length && !changed && error[0]);
        }
    }
}
static void native_whitespace(void)
{
    char error[256]; size_t length, changed; fixture f = {0}; f.keep = 1;
    /* Every byte that the native signed comparison skips between tokens stays
     * present in a no-op result. DEL and NUL are not silently accepted. */
    for (unsigned byte = 0; byte < 256; byte++) {
        if (byte > 32 && byte < 127) continue;
        char source[] = {'{', ' ', (char)byte, ' ', '{', '0', ',', '1', '}', '}', 0};
        char *out = sh_decl_native_rebind(SH_BINDING_TABLE, (sh_decl_source){source, sizeof(source) - 1}, NULL,
            bind, &f, &length, &changed, error, sizeof(error));
        if (byte && byte != 127) assert(out && !changed && length == sizeof(source) - 1 && !memcmp(out, source, length));
        else assert(!out && !length && !changed && error[0]);
        free(out);
    }
    {
        const char source[] = "{props {models {\"models/high\xcc\"}}\x11\x04 states {} subWebs {}}";
        char *out = sh_decl_native_rebind(SH_BINDING_ANIMATION_WEB, (sh_decl_source){source, sizeof(source) - 1}, NULL,
            bind, &f, &length, &changed, error, sizeof(error));
        assert(out && !changed && length == sizeof(source) - 1 && !memcmp(out, source, length)); free(out);
    }
}
int main(void) { material(); md6(); table(); articulated_figure(); breakable(); animation_web();
    native_whitespace(); puts("native declaration rebind tests passed"); return 0; }
