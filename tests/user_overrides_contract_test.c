#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

#define SOURCE_LIMIT (1024u * 1024u)

static char *read_file(const char *root, const char *relative)
{
    char path[1024];
    FILE *file = NULL;
    long length;
    char *bytes;

    if (_snprintf_s(path, sizeof(path), _TRUNCATE,
                    "%s\\%s", root, relative) < 0)
        return NULL;
    if (fopen_s(&file, path, "rb") != 0 || !file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || (unsigned long)length > SOURCE_LIMIT ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (char *)malloc((size_t)length + 1);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    if (length && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    bytes[length] = '\0';
    return bytes;
}

static const char *matching_brace(const char *open)
{
    const char *p;
    int depth = 0;
    int line_comment = 0;
    int block_comment = 0;
    char quote = '\0';

    if (!open || *open != '{') return NULL;
    for (p = open; *p; p++) {
        if (line_comment) {
            if (*p == '\n') line_comment = 0;
            continue;
        }
        if (block_comment) {
            if (p[0] == '*' && p[1] == '/') {
                block_comment = 0;
                p++;
            }
            continue;
        }
        if (quote) {
            if (*p == '\\' && p[1] != '\0') p++;
            else if (*p == quote) quote = '\0';
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            line_comment = 1;
            p++;
        } else if (p[0] == '/' && p[1] == '*') {
            block_comment = 1;
            p++;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) return p;
            if (depth < 0) return NULL;
        }
    }
    return NULL;
}

/* needle occurs between open and close. */
static int within(const char *open, const char *close, const char *needle)
{
    const char *found = open ? strstr(open, needle) : NULL;
    return found && close && found < close;
}

int main(int argc, char **argv)
{
    char *dllmain;
    char *overrides;
    char *cvars;
    char *commands;
    char *build_script;
    const char *config_init;
    const char *capture;
    const char *install;
    const char *user_gate;
    const char *user_gate_open;
    const char *user_gate_close;
    const char *user_baked_open;
    const char *user_general_open;
    const char *baked_gate;
    const char *baked_gate_open;
    const char *baked_gate_close;
    const char *baked_stream;
    const char *engine_fallback;
    const char *claim, *claim_open, *claim_close;
    const char *body, *mode_gate, *mode_gate_open, *mode_gate_close, *claim_call;
    const char *route, *route_open, *route_close;

    if (argc != 2) {
        fprintf(stderr, "usage: user_overrides_contract_test <repo-root>\n");
        return 2;
    }

    dllmain = read_file(argv[1], "src\\backend\\dllmain.c");
    overrides = read_file(argv[1], "src\\backend\\overrides.c");
    cvars = read_file(argv[1], "src\\backend\\cvars.c");
    commands = read_file(argv[1], "src\\backend\\commands.c");
    build_script = read_file(argv[1], "src\\backend\\build.ps1");
    CHECK(dllmain != NULL);
    CHECK(overrides != NULL);
    CHECK(cvars != NULL);
    CHECK(commands != NULL);
    CHECK(build_script != NULL);
    if (!dllmain || !overrides || !cvars || !commands || !build_script)
        goto done;

    config_init = strstr(dllmain, "sh_config_init();");
    capture = strstr(dllmain, "sh_user_overrides_capture_launch_state();");
    install = strstr(dllmain, "sh_overrides_install(g_doom_base");
    CHECK(config_init != NULL);
    CHECK(capture != NULL);
    CHECK(install != NULL);
    if (config_init && capture) CHECK(config_init < capture);
    if (capture && install) CHECK(capture < install);

    CHECK(strstr(overrides,
          "int user_on = sh_user_overrides_enabled_for_launch();") != NULL);
    CHECK(strstr(overrides, "overrides.disabled") == NULL);
    CHECK(strstr(overrides, "sh_cvar_value_int_reg") == NULL);
    CHECK(strstr(overrides, "g_user_layer_off") == NULL);
    /* One claim chain (ov_claim) answers ordinary opens and the resource
     * manager's file route (R-309). Package content stays inside the launch
     * user gate; the built-in fallback follows that gate, outside it; the
     * chain never reaches the engine's open. */
    claim = strstr(overrides, "static int ov_claim(const char *name, void **out)");
    claim_open = claim ? strchr(claim, '{') : NULL;
    claim_close = matching_brace(claim_open);
    CHECK(claim != NULL);
    CHECK(claim_open != NULL);
    CHECK(claim_close != NULL);
    user_gate = claim_open ? strstr(claim_open, "if (sh_user_overrides_enabled_for_launch()) {") : NULL;
    user_gate_open = user_gate ? strchr(user_gate, '{') : NULL;
    user_gate_close = matching_brace(user_gate_open);
    baked_gate = claim_open ? strstr(claim_open, "if (s == NULL && baked != NULL)") : NULL;
    baked_gate_open = baked_gate ? strchr(baked_gate, '{') : NULL;
    baked_gate_close = matching_brace(baked_gate_open);
    if (claim_close && user_gate_close) CHECK(user_gate_close < claim_close);
    if (claim_close && baked_gate_close) CHECK(baked_gate_close < claim_close);
    if (claim_open && claim_close) CHECK(!within(claim_open, claim_close, "g_orig_open"));
    CHECK(user_gate != NULL);
    CHECK(user_gate_open != NULL);
    CHECK(user_gate_close != NULL);
    CHECK(baked_gate != NULL);
    CHECK(baked_gate_open != NULL);
    CHECK(baked_gate_close != NULL);
    user_baked_open = user_gate
        ? strstr(user_gate,
                 "sh_package_runtime_read_captured(name, &bytes, &length,")
        : NULL;
    user_general_open = user_gate
        ? strstr(user_gate, "stream = make_mem_stream(owned,")
        : NULL;
    CHECK(user_baked_open != NULL);
    CHECK(user_general_open != NULL);
    {
        const char *file_open = user_gate ? strstr(user_gate, "sh_package_runtime_open_file_captured(name,") : NULL;
        CHECK(file_open != NULL);
        if (user_gate_open && user_gate_close && file_open)
            CHECK(user_gate_open < file_open && file_open < user_gate_close);
    }
    if (user_gate_open && user_gate_close && user_baked_open)
        CHECK(user_gate_open < user_baked_open &&
              user_baked_open < user_gate_close);
    if (user_gate_open && user_gate_close && user_general_open)
        CHECK(user_gate_open < user_general_open &&
              user_general_open < user_gate_close);
    if (user_gate_close && baked_gate)
        CHECK(user_gate_close < baked_gate);
    baked_stream = baked_gate
        ? strstr(baked_gate, "s = make_mem_stream(")
        : NULL;
    CHECK(baked_stream != NULL);
    if (baked_gate_open && baked_gate_close && baked_stream)
        CHECK(baked_gate_open < baked_stream &&
              baked_stream < baked_gate_close);

    /* Ordinary opens consult the chain only below mode 2 and for a name, then
     * fall back to the engine's open after both gates. */
    body = strstr(overrides, "static void *ov_open_body(void *self, const char *name,");
    mode_gate = body ? strstr(body, "if (mode < 2 && name) {") : NULL;
    mode_gate_open = mode_gate ? strchr(mode_gate, '{') : NULL;
    mode_gate_close = matching_brace(mode_gate_open);
    claim_call = mode_gate_open ? strstr(mode_gate_open, "if (ov_claim(name, &claimed)) return claimed;") : NULL;
    engine_fallback = body ? strstr(body, "g_orig_open(self, name") : NULL;
    CHECK(body != NULL);
    CHECK(mode_gate != NULL);
    CHECK(mode_gate_close != NULL);
    CHECK(claim_call != NULL);
    CHECK(engine_fallback != NULL);
    if (claim_close && body) CHECK(claim_close < body);
    if (mode_gate_close && claim_call) CHECK(claim_call < mode_gate_close);
    if (mode_gate_close && engine_fallback) CHECK(mode_gate_close < engine_fallback);
    if (user_gate_close && baked_gate_close && engine_fallback)
        CHECK(user_gate_close < engine_fallback &&
              baked_gate_close < engine_fallback);
    /* A stream handed to the engine's own fill is taken only by an ordinary
     * open below mode 2. */
    CHECK(strstr(overrides,
          "if (g_pending_stream && name && name == g_pending_name && mode < 2) {") != NULL);

    /* The resource manager's file route claims through the same chain, only
     * while the shadow is installed, and never chains to the engine. */
    route = strstr(overrides, "int sh_overrides_claim_open(const char *name, void **stream)");
    route_open = route ? strchr(route, '{') : NULL;
    route_close = matching_brace(route_open);
    CHECK(route != NULL);
    CHECK(route_close != NULL);
    CHECK(within(route_open, route_close, "return ov_claim(name, stream);"));
    CHECK(within(route_open, route_close, "if (g_orig_open == NULL) return 0;"));
    CHECK(!within(route_open, route_close, "g_orig_open("));

    CHECK(strstr(cvars, "\"sh_user_overrides\"") == NULL);
    CHECK(strstr(cvars, "sh_cvar_value_int_reg") == NULL);
    CHECK(strstr(commands, "{ \"sh_user_overrides\"") != NULL);
    CHECK(strstr(build_script, "\"user_overrides.c\"") != NULL);

done:
    free(dllmain);
    free(overrides);
    free(cvars);
    free(commands);
    free(build_script);
    if (g_failed) {
        fprintf(stderr, "%d user-overrides contract test(s) failed\n",
                g_failed);
        return 1;
    }
    puts("user overrides contract tests passed");
    return 0;
}
