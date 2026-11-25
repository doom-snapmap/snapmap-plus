/* The ribbon mask uses trail position in X and remaining node life in Y. */
static char *rb_text(const char *text, size_t len, const char *uv, const char *coords,
                     const char *rgba, const char *opaque, size_t *out_len)
{
    const char *a = mp_find(text, len, uv), *b, *end = text + len;
    mp_buf out = { 0 };
    if (!a || !(b = mp_find(a, (size_t)(end - a), rgba)) ||
        mp_find(a + 1, (size_t)(end - a - 1), uv) || mp_find(b + 1, (size_t)(end - b - 1), rgba)) return NULL;
    mp_put(&out, text, (size_t)(a - text));
    mp_puts(&out, coords);
    a += strlen(uv);
    mp_put(&out, a, (size_t)(b - a));
    mp_puts(&out, opaque);
    b += strlen(rgba);
    mp_put(&out, b, (size_t)(end - b));
    return mp_take(&out, out_len);
}

static char *rb_decl(const char *text, size_t len, size_t *out_len)
{
    return rb_text(text, len, "result.texcoord1.xyz = vertex.tangent.xyz;",
                   "result.texcoord1.xyz = float3( float2( vertex.tangent.x, vertex.color.a ) * 0.99609375 + 0.001953125, vertex.tangent.z );",
                   "DeGamma( vertex.color )", "DeGamma( float4( vertex.color.rgb, 1.0 ) )", out_len);
}

static char *rb_glsl(const char *text, size_t len, unsigned tex_end, unsigned shift, size_t *out_len)
{
    (void)tex_end;
    (void)shift;
    return rb_text(text, len, "vofi_TexCoord1.xyz = in_Tangent.xyz;",
                   "vofi_TexCoord1.xyz = vec3( vec2( in_Tangent.x, in_Color.a ) * 0.99609375 + 0.001953125, in_Tangent.z );",
                   "DeGamma( in_Color )", "DeGamma( vec4( in_Color.rgb, 1.0 ) )", out_len);
}

static uint32_t *rb_splice(const uint32_t *stock, size_t words, unsigned tex_end, unsigned shift,
                           size_t *out_words, const char **why)
{
    mp_ctx c = { 0 };
    mp_list code = { 0 }, empty = { 0 };
    size_t first, last, tangent_at = 0, color_at = 0, i;
    uint32_t tangent, color, *out = NULL;
    (void)tex_end;
    (void)shift;
    if (!mp_parse(&c.S, stock, words) || !mp_main(&c, &first, &last)) MP_FAIL(&c, "its vertex module has no main");
    tangent = mp_named(&c.S, "in_Tangent", 0);
    color = mp_named(&c.S, "in_Color", 0);
    for (i = first; i < last; i++) {
        const mp_ins *ins = &c.S.ins[i];
        if (MP_OP(ins) != OpLoad || MP_N(ins) != 4) continue;
        if (ins->w[3] == tangent) {
            if (tangent_at) MP_FAIL(&c, "its vertex has a second tangent load");
            tangent_at = i;
        }
        if (ins->w[3] == color) {
            if (color_at) MP_FAIL(&c, "its vertex has a second colour load");
            color_at = i;
        }
    }
    if (!tangent_at || color_at <= tangent_at) MP_FAIL(&c, "its vertex does not read the tangent followed by the colour");
    {
        uint32_t *ti = c.S.ins[tangent_at].w, *ci = c.S.ins[color_at].w;
        const uint32_t vec = ti[1], result = ti[2], result_color = ci[2];
        const mp_ins *v = mp_def(&c.S, vec), *f;
        uint32_t scalar, one, scale, half, raw_tan, raw_rgba, u, y, ux, uy, x, life, with_x;
        if (ci[1] != vec || !v || MP_OP(v) != OpTypeVector || v->w[3] != 4 ||
            !(f = mp_def(&c.S, v->w[2])) || MP_OP(f) != OpTypeFloat || f->w[2] != 32)
            MP_FAIL(&c, "its tangent and colour are not four floats");
        scalar = v->w[2];
        one = c.S.bound++; scale = c.S.bound++; half = c.S.bound++;
        raw_tan = c.S.bound++; raw_rgba = c.S.bound++; u = c.S.bound++; y = c.S.bound++;
        ux = c.S.bound++; uy = c.S.bound++; x = c.S.bound++; life = c.S.bound++; with_x = c.S.bound++;
#define RB_OP(list, op, ...) do { const uint32_t args[] = { __VA_ARGS__ }; \
    if (!mp_emit(&c.S, list, op, args, sizeof args / sizeof args[0])) MP_FAIL(&c, "out of memory"); } while (0)
        RB_OP(&c.extra, OpConstant, scalar, one, 0x3f800000u);
        RB_OP(&c.extra, OpConstant, scalar, scale, 0x3f7f0000u);
        RB_OP(&c.extra, OpConstant, scalar, half, 0x3b000000u);
        uint32_t *load = mp_own(&c.S, 4), *opaque = mp_own(&c.S, 6);
        if (!load || !opaque) MP_FAIL(&c, "out of memory");
        memcpy(load, ti, 4 * sizeof *load);
        load[2] = raw_tan;
        c.S.ins[tangent_at].w = load;
        RB_OP(&code, OpLoad, vec, raw_rgba, color);
        RB_OP(&code, OpCompositeExtract, scalar, u, raw_tan, 0);
        RB_OP(&code, OpCompositeExtract, scalar, y, raw_rgba, 3);
        RB_OP(&code, 133 /* OpFMul */, scalar, ux, u, scale);
        RB_OP(&code, 133, scalar, uy, y, scale);
        RB_OP(&code, 129 /* OpFAdd */, scalar, x, ux, half);
        RB_OP(&code, 129, scalar, life, uy, half);
        RB_OP(&code, OpCompositeInsert, vec, with_x, x, raw_tan, 0);
        RB_OP(&code, OpCompositeInsert, vec, result, life, with_x, 1);
#undef RB_OP
        opaque[0] = (6u << 16) | OpCompositeInsert;
        opaque[1] = vec; opaque[2] = result_color; opaque[3] = one; opaque[4] = raw_rgba; opaque[5] = 3;
        c.S.ins[color_at].w = opaque;
    }
    out = mp_reassemble(&c, (size_t)-1, &empty, tangent_at, &code, &empty, out_words);
done:
    *why = c.why;
    free(code.v);
    mp_ctx_free(&c);
    return out;
}
