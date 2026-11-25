/* The particle variant shares modprog.c's private shader assembly helpers. */

/* The particle program (programs/shaders/particles.rp): the parameters appended to the fragment stage's material
 * uniforms, its added texture, the module's samplers (the first three bound to the program's own textures), the stage
 * inputs and the program's own uniforms it reads ('h' the material block, 'l' the view block). */
const char *const SH_MODPROG_EP_TEXTURES[] = { "particleemissivemap" };
const unsigned SH_MODPROG_EP_TEXTURE_COUNT = sizeof SH_MODPROG_EP_TEXTURES / sizeof SH_MODPROG_EP_TEXTURES[0];
static const char *const EP_BLOCK_PARMS[] = { "smokebrightness", "projectionmatrixz", "globalvieworigin" };
#define EP_NBLOCK_PARMS (sizeof EP_BLOCK_PARMS / sizeof EP_BLOCK_PARMS[0])
static const char *const EP_SAMPLERS[] = { "ep_trans", "ep_depth", "ep_atlas", "ep_emissive" };
static const char *const EP_SAMPLER_STOCK[] = { "samp_transmap", "samp_viewdepthmap", "samp_particleslightatlasmap", NULL };
#define EP_NSAMPLERS (sizeof EP_SAMPLERS / sizeof EP_SAMPLERS[0])
static const char *const EP_VARYINGS[] = {
    "vofi_Color", "vofi_TexCoord0", "vofi_TexCoord1", "vofi_TexCoord2", "vofi_TexCoord4", "vofi_TexCoord6", "gl_FragCoord",
};
#define EP_NVARYINGS (sizeof EP_VARYINGS / sizeof EP_VARYINGS[0])
static const char *const EP_UNIFORMS[] = {
    "hrenderpositiontoviewtexture", "lparticleatlasparms", "lfoglightinscattercolor", "lfoglightinscatterintensity",
    "lfoglightoppositecolor", "lfogcolor", "ltransparencylightmult",
    "hparticleuberparms1", "hparticleuberparms2", "hparticleuberparms3", "hemissive",
};
#define EP_NUNIFORMS (sizeof EP_UNIFORMS / sizeof EP_UNIFORMS[0])

static char *ep_patch_decl(const char *text, size_t len, size_t *out_len, const char *header)
{
    static const char anchor[] = "PREMULTIPLY_ALPHA_BLENDED( result );";
    const char *nl = mp_find(text, len, "\r\n") ? "\r\n" : "\n", *end = text + len;
    const char *fp = mp_find(text, len, "hlsl_fp {"), *call, *eol, *ls;
    mp_buf b = { 0 };
    char *prefix, *t;
    size_t n;
    if (!fp) return NULL;
    call = mp_find(fp, (size_t)(end - fp), anchor);
    if (!call || mp_find(call + 1, (size_t)(end - call - 1), anchor)) return NULL;
    if (!(eol = mp_find(call, (size_t)(end - call), nl))) return NULL;
    for (ls = call; ls > fp && ls[-1] != '\n'; ) ls--;
    eol += strlen(nl);
    /* the call after the premultiply, at its indentation */
    if (!(prefix = (char *)malloc((size_t)(call - ls) + 1))) return NULL;
    memcpy(prefix, ls, (size_t)(call - ls));
    prefix[call - ls] = 0;
    mp_put(&b, text, (size_t)(eol - text));
    mp_put_lines(&b, SH_MODPROG_EP_CALL, prefix, nl, 0);
    mp_put(&b, eol, (size_t)(end - eol));
    free(prefix);
    if (!(t = mp_take(&b, &n))) return NULL;
    /* the functions, in the header section */
    prefix = mp_add_header(t, n, header, nl, out_len);
    free(t);
    return prefix;
}

static char *ep_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len, const char *glsl)
{
    static const char ubo[] = "uniform freqHigh_fragmentUniforms_ubo {";
    mp_buf b = { 0 };
    const char *end, *u, *close, *first, *mainp, *last;
    unsigned k;
    char *t1;
    size_t n1;
    if (!(t1 = mp_glsl_bindings(text, len, tex_end, shift, &n1))) return NULL;
    end = t1 + n1;
    if (!(u = mp_find(t1, n1, ubo)) || !(close = memchr(u, '}', (size_t)(end - u)))) { free(t1); return NULL; }
    first = mp_find(t1, n1, "layout( location");
    mainp = mp_find(t1, n1, "void main() {");
    /* the end of main: its closing brace, the text's last */
    last = end;
    while (last > t1 && (last[-1] == '\n' || last[-1] == '\r' || last[-1] == ' ' || last[-1] == '\t')) last--;
    if (!first || !mainp || last <= mainp || last[-1] != '}' || first < close || mainp < first) { free(t1); return NULL; }
    last--;
    mp_put(&b, t1, (size_t)(close - t1));
    for (k = 0; k < EP_NBLOCK_PARMS; k++) mp_printf(&b, "\tvec4 %s;\n", EP_BLOCK_PARMS[k]);
    mp_put(&b, close, (size_t)(first - close));
    for (k = 0; k < SH_MODPROG_EP_TEXTURE_COUNT; k++)
        mp_printf(&b, "layout( binding = %u ) uniform sampler2D samp_%s;\n", tex_end + k, SH_MODPROG_EP_TEXTURES[k]);
    mp_put(&b, first, (size_t)(mainp - first));
    mp_puts(&b, glsl);
    mp_put(&b, mainp, (size_t)(last - mainp));
    mp_puts(&b, "\tep_shade( out_FragColor0, vec4[");
    mp_printf(&b, "%u]( ", (unsigned)EP_NVARYINGS);
    for (k = 0; k < EP_NVARYINGS; k++) mp_printf(&b, "%s%s", k ? ", " : "", EP_VARYINGS[k]);
    mp_printf(&b, " ), vec4[%u]( ", (unsigned)EP_NBLOCK_PARMS);
    for (k = 0; k < EP_NBLOCK_PARMS; k++) mp_printf(&b, "%sfreqHigh_fragmentUniforms.%s", k ? ", " : "", EP_BLOCK_PARMS[k]);
    mp_printf(&b, " ), vec4[%u]( ", (unsigned)EP_NUNIFORMS);
    for (k = 0; k < EP_NUNIFORMS; k++)
        mp_printf(&b, "%s%s_fragmentUniforms.%s", k ? ", " : "", EP_UNIFORMS[k][0] == 'h' ? "freqHigh" : "freqLow",
                  EP_UNIFORMS[k] + 1);
    mp_puts(&b, " ) );\n");
    mp_put(&b, last, (size_t)(end - last));
    free(t1);
    return mp_take(&b, out_len);
}

static uint32_t *ep_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                               size_t *out_words, const char **why, const unsigned *words, unsigned nwords)
{
    mp_ctx c;
    mp_import_spec sp;
    mp_blocks bk;
    uint32_t *out = NULL;
    size_t i, k;
    uint32_t entry = 0, frag = 0, v4ptr_out = 0, params[4], temps[4], vals[16];
    size_t s_main = 0, s_main_end = 0, anchor = 0, var_end = 0;
    mp_list code = { 0 }, heads = { 0 }, funcs = { 0 };
    memset(&c, 0, sizeof c);
    if (out_words) *out_words = 0;
    (void)shift;
    if (!mp_parse(&c.S, stock, stock_words)) MP_FAIL(&c, "the program's fragment module is not SPIR-V");
    sp.words = words; sp.nwords = nwords; sp.entry = "ep_shade(";
    sp.samplers = EP_SAMPLERS; sp.stock = EP_SAMPLER_STOCK; sp.nsamplers = EP_NSAMPLERS; sp.what = "particle";
    if (!mp_import(&c, &sp, tex_end, &entry, &funcs)) goto done;
    if (!mp_shift_bindings(&c, tex_end, shift)) goto done;
    if (!mp_grow_block(&c, EP_BLOCK_PARMS, EP_NBLOCK_PARMS, &bk)) goto done;
    if (!mp_main(&c, &s_main, &s_main_end)) MP_FAIL(&c, "the program's fragment module has no main");
    /* the anchor: the instruction before main's last return (the stock shading has written its colour) */
    for (i = s_main_end; i > s_main; i--) if (MP_OP(&c.S.ins[i]) == OpReturn) { anchor = i - 1; break; }
    if (!anchor) MP_FAIL(&c, "the program's main does not end in a return");
    frag = mp_named(&c.S, "out_FragColor0", 0);
    if (!frag) MP_FAIL(&c, "the program writes no colour");
    if (!mp_entry_params(&funcs, entry, params, 4)) MP_FAIL(&c, "ep_shade does not take the parameters the splice passes");
    if (!mp_temps(&c, s_main, s_main_end, params, temps, 4, &heads, &var_end)) MP_FAIL(&c, "out of memory");
    {
        /* frag in */
        const mp_ins *pp = mp_def_any(&c, params[0]), *fv = mp_def(&c.S, frag), *fp = fv ? mp_def(&c.S, fv->w[1]) : NULL;
        uint32_t o[3], v;
        if (!pp || !fp || pp->w[3] != fp->w[3]) MP_FAIL(&c, "the program's colour is not a vector");
        v4ptr_out = fp->w[3];
        v = c.S.bound++;
        o[0] = v4ptr_out; o[1] = v; o[2] = frag;
        if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
        o[0] = temps[0]; o[1] = v;
        if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
    }
    {
        /* V: the stage inputs */
        for (k = 0; k < EP_NVARYINGS; k++) {
            uint32_t var = mp_named(&c.S, EP_VARYINGS[k], 0), o[3];
            const mp_ins *d = var ? mp_def(&c.S, var) : NULL, *p = d ? mp_def(&c.S, d->w[1]) : NULL;
            if (!d || MP_OP(d) != OpVariable || d->w[3] != 1 || !p || p->w[3] != v4ptr_out)
                MP_FAIL(&c, "the program's fragment stage lacks an input the particle shading reads");
            vals[k] = c.S.bound++;
            o[0] = v4ptr_out; o[1] = vals[k]; o[2] = var;
            if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
        }
        if (!mp_store_array(&c, &code, params[1], temps[1], vals, EP_NVARYINGS)) MP_FAIL(&c, "ep_shade's input array has no type");
    }
    {
        /* P: the parameters the block grew by */
        for (k = 0; k < EP_NBLOCK_PARMS; k++)
            if (!(vals[k] = mp_load_member(&c, &code, bk.fh, bk.v4, (int)(bk.n0 + k)))) MP_FAIL(&c, "out of memory");
        if (!mp_store_array(&c, &code, params[2], temps[2], vals, EP_NBLOCK_PARMS)) MP_FAIL(&c, "ep_shade's parameter array has no type");
    }
    {
        /* U: the program's own uniforms */
        for (k = 0; k < EP_NUNIFORMS; k++) {
            int high = EP_UNIFORMS[k][0] == 'h';
            const char *name = EP_UNIFORMS[k] + 1;
            int mem = mp_member(&c.S, high ? bk.fh_struct : bk.fl_struct, name);
            if (mem < 0) MP_FAIL(&c, "the program lacks a uniform the particle shading reads");
            if (!(vals[k] = mp_load_member(&c, &code, high ? bk.fh : bk.fl, bk.v4, mem))) MP_FAIL(&c, "out of memory");
        }
        if (!mp_store_array(&c, &code, params[3], temps[3], vals, EP_NUNIFORMS)) MP_FAIL(&c, "ep_shade's uniform array has no type");
    }
    {
        uint32_t o[7], t_void, v;
        { uint32_t ov[1] = { 0 }; t_void = mp_find_or_add(&c, OpTypeVoid, ov, 1, 1); }
        o[0] = t_void; o[1] = c.S.bound++; o[2] = entry;
        memcpy(o + 3, temps, sizeof temps);
        if (!mp_emit(&c.S, &code, OpFunctionCall, o, 7)) MP_FAIL(&c, "out of memory");
        v = c.S.bound++;
        o[0] = v4ptr_out; o[1] = v; o[2] = temps[0];
        if (!mp_emit(&c.S, &code, OpLoad, o, 3)) MP_FAIL(&c, "out of memory");
        o[0] = frag; o[1] = v;
        if (!mp_emit(&c.S, &code, OpStore, o, 2)) MP_FAIL(&c, "out of memory");
    }
    if (c.S.oom) MP_FAIL(&c, "out of memory");
    out = mp_reassemble(&c, var_end, &heads, anchor, &code, &funcs, out_words);
done:
    if (why) *why = c.why;
    free(code.v); free(heads.v); free(funcs.v);
    mp_ctx_free(&c);
    if (c.why) { free(out); return NULL; }
    return out;
}

char *sh_modprog_ep_patch_decl(const char *text, size_t len, size_t *out_len)
{
    return ep_patch_decl(text, len, out_len, SH_MODPROG_EP_HEADER);
}

char *sh_modprog_ep_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len)
{
    return ep_patch_glsl(text, len, tex_end, shift, out_len, SH_MODPROG_EP_GLSL);
}

uint32_t *sh_modprog_ep_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                          size_t *out_words, const char **why)
{
    return ep_splice(stock, stock_words, tex_end, shift, out_words, why, SH_MODPROG_EP_SPIRV, SH_MODPROG_EP_SPIRV_WORDS);
}

static char *ep_base_patch_decl(const char *text, size_t len, size_t *out_len)
{
    return ep_patch_decl(text, len, out_len, SH_MODPROG_EP_BASE_HEADER);
}

static char *ep_base_patch_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len)
{
    return ep_patch_glsl(text, len, tex_end, shift, out_len, SH_MODPROG_EP_BASE_GLSL);
}

static uint32_t *ep_base_splice(const uint32_t *stock, size_t stock_words, unsigned tex_end, unsigned shift,
                          size_t *out_words, const char **why)
{
    return ep_splice(stock, stock_words, tex_end, shift, out_words, why, SH_MODPROG_EP_BASE_SPIRV, SH_MODPROG_EP_BASE_SPIRV_WORDS);
}
