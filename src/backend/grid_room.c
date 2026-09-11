#include "grid_room.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const stock_names[] = {
    NULL, "maps/modules/classic/classic_blank_room",
    "maps/modules/ind_dlc/ind_totally_blank_room_4x"
};

const char *sh_grid_stock_name(sh_grid_kind kind)
{
    return kind == SH_GRID_CLASSIC || kind == SH_GRID_MODERN ? stock_names[kind] : NULL;
}

sh_grid_kind sh_grid_stock_kind(const char *name)
{
    unsigned i;
    if (!name) return SH_GRID_NONE;
    for (i = 1; i <= 2; ++i) {
        size_t n = strlen(stock_names[i]);
        if (!strncmp(name, stock_names[i], n) &&
            (!name[n] || !strcmp(name + n, ".decl"))) return (sh_grid_kind)i;
    }
    return SH_GRID_NONE;
}

int sh_grid_default(sh_grid_kind kind, sh_grid_size *out)
{
    sh_grid_size value;
    if (!out || !sh_grid_stock_name(kind)) return 0;
    value.kind = kind;
    value.xyz[0] = value.xyz[1] = kind == SH_GRID_MODERN ? 5120 : 2560;
    value.xyz[2] = kind == SH_GRID_MODERN ? 3392 : 2048;
    *out = value;
    return 1;
}

int sh_grid_minimum(sh_grid_kind kind, sh_grid_size *out)
{
    if(!out||!sh_grid_stock_name(kind))return 0;
    out->kind=kind;
    out->xyz[0]=kind==SH_GRID_MODERN?864:416;
    out->xyz[1]=kind==SH_GRID_MODERN?272:416;
    out->xyz[2]=kind==SH_GRID_MODERN?432:304;
    return 1;
}

int sh_grid_maximum(sh_grid_kind kind,sh_grid_size *out)
{
    if(!out||!sh_grid_stock_name(kind))return 0;
    out->kind=kind;
    /* X is centered; modern Y is centered at 1280; Z starts at zero.
     * AAS extends past the shell by the 64-unit agent radius, plus 32
     * units on modern X. Include that margin in signed-short bounds. */
    out->xyz[0]=kind==SH_GRID_MODERN?65342:65406;
    out->xyz[1]=kind==SH_GRID_MODERN?62846:65406;
    out->xyz[2]=32767;
    return 1;
}

int sh_grid_clamp(sh_grid_kind kind,const float requested[3],sh_grid_size *out)
{
    sh_grid_size size,maximum;unsigned i;
    if(!requested||!out||!sh_grid_minimum(kind,&size)||!sh_grid_maximum(kind,&maximum))return 0;
    for(i=0;i<3;++i){
        float value=requested[i];
        if(!isfinite(value))return 0;
        if(value<(float)size.xyz[i])value=(float)size.xyz[i];
        if(value>(float)maximum.xyz[i])value=(float)maximum.xyz[i];
        size.xyz[i]=(unsigned)floorf(value+.5f);
    }
    *out=size;return 1;
}

int sh_grid_warp_init(const sh_grid_size *size, sh_grid_warp *out)
{
    sh_grid_warp value;
    sh_grid_size minimum,maximum;
    unsigned i, j;
    float x, y, z;
    if (!size || !out || !sh_grid_minimum(size->kind,&minimum)||!sh_grid_maximum(size->kind,&maximum)) return 0;
    for (i = 0; i < 3; ++i)
        if (size->xyz[i] < minimum.xyz[i] || size->xyz[i] > maximum.xyz[i]) return 0;
    x = (float)size->xyz[0]; y = (float)size->xyz[1]; z = (float)size->xyz[2];
    memset(&value, 0, sizeof value);
    value.size = *size;
    if (size->kind == SH_GRID_MODERN) {
        const sh_grid_axis axes[3] = {
            {6, {-2560,-2464,-320,320,2464,2560}, {-x/2,-x/2+96,-320,320,x/2-96,x/2}},
            {4, {-1280,-1152,3712,3840}, {1280-y/2,1408-y/2,1152+y/2,1280+y/2}},
            {4, {0,352,3328,3392}, {0,352,z-64,z}}
        };
        memcpy(value.axis, axes, sizeof axes);
    } else {
        const sh_grid_axis axes[3] = {
            {6, {-1280,-1248,-160,160,1248,1280}, {-x/2,-x/2+32,-160,160,x/2-32,x/2}},
            {6, {-1280,-1248,-160,160,1248,1280}, {-y/2,-y/2+32,-160,160,y/2-32,y/2}},
            {4, {0,256,2016,2048}, {0,256,z-32,z}}
        };
        memcpy(value.axis, axes, sizeof axes);
    }
    for (i = 0; i < 3; ++i)
        for (j = 1; j < value.axis[i].count; ++j)
            if (value.axis[i].target[j] <= value.axis[i].target[j-1]) return 0;
    *out = value;
    return 1;
}

int sh_grid_name(const sh_grid_size *size, char *out, size_t capacity)
{
    sh_grid_warp warp;
    int n;
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!sh_grid_warp_init(size, &warp)) return 0;
    n = snprintf(out, capacity, "maps/modules/smpgrid/v1/%c/%u_%u_%u",
        size->kind == SH_GRID_MODERN ? 'm' : 'c', size->xyz[0], size->xyz[1], size->xyz[2]);
    if (n < 0 || (size_t)n >= capacity) { out[0] = 0; return 0; }
    return 1;
}

int sh_grid_parse_name(const char *name, sh_grid_size *out)
{
    const char prefix[] = "maps/modules/smpgrid/v1/";
    const char *p;
    sh_grid_size size;
    sh_grid_warp warp;
    unsigned i;
    if (!name || !out || strncmp(name, prefix, sizeof prefix - 1)) return 0;
    p = name + sizeof prefix - 1;
    if (*p != 'c' && *p != 'm') return 0;
    size.kind = *p++ == 'm' ? SH_GRID_MODERN : SH_GRID_CLASSIC;
    if (*p++ != '/') return 0;
    for (i = 0; i < 3; ++i) {
        unsigned n = 0;
        if (*p < '1' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (unsigned)(*p++ - '0');
            if (n > 65534) return 0;
        }
        size.xyz[i] = n;
        if (i < 2 && *p++ != '_') return 0;
    }
    if (*p && strcmp(p, ".decl")) return 0;
    if (!sh_grid_warp_init(&size, &warp)) return 0;
    *out = size;
    return 1;
}

void sh_grid_affine(const sh_grid_warp *warp, unsigned axis, float value,
                    float *scale, float *shift)
{
    const sh_grid_axis *a = &warp->axis[axis];
    unsigned i;
    *scale = 1;
    if (value < a->source[0]) { *shift = a->target[0] - a->source[0]; return; }
    for (i = 1; i < a->count; ++i) if (value < a->source[i]) {
        *scale = (a->target[i] - a->target[i-1]) / (a->source[i] - a->source[i-1]);
        *shift = a->target[i-1] - *scale * a->source[i-1];
        return;
    }
    *shift = a->target[a->count-1] - a->source[a->count-1];
}

float sh_grid_coordinate(const sh_grid_warp *warp, unsigned axis, float value)
{
    float scale, shift;
    sh_grid_affine(warp, axis, value, &scale, &shift);
    return scale * value + shift;
}

void sh_grid_point(const sh_grid_warp *warp, const float source[3], float target[3])
{
    unsigned i;
    for (i = 0; i < 3; ++i) target[i] = sh_grid_coordinate(warp, i, source[i]);
}

int sh_grid_light(const sh_grid_warp *warp, const float origin[3],
                   const float radius[3], const float center[3], int roof_anchor,
                   float new_origin[3], float new_radius[3], float new_center[3])
{
    sh_grid_size stock;
    float roof, old_roof, pivot[3] = {0,0,0}, scale[3], gap;
    float o[3], r[3], c[3];
    unsigned i;
    if (!warp || !origin || !radius || !center || !new_origin || !new_radius ||
        !new_center || !sh_grid_default(warp->size.kind, &stock)) return 0;
    old_roof = (float)stock.xyz[2]; roof = (float)warp->size.xyz[2];
    if (warp->size.kind == SH_GRID_MODERN) { old_roof -= 48; roof -= 48; pivot[1] = 1280; }
    for (i = 0; i < 3; ++i) {
        if (!isfinite(origin[i]) || !isfinite(radius[i]) || !isfinite(center[i]) || radius[i] < 0) return 0;
        scale[i] = i == 2 ? roof / old_roof : (float)warp->size.xyz[i] / stock.xyz[i];
        o[i] = pivot[i] + (origin[i] - pivot[i]) * scale[i];
        r[i] = radius[i] * scale[i]; c[i] = center[i] * scale[i];
    }
    if (roof_anchor) {
        gap = old_roof - origin[2] - center[2];
        if (gap < 0) gap = 0;
        if (gap > roof * .25f) gap = roof * .25f;
        c[2] = roof - gap - o[2];
    }
    for (i = 0; i < 3; ++i) {
        float lo = i == 2 ? 0 : pivot[i] - (float)warp->size.xyz[i] / 2;
        float hi = i == 2 ? roof : pivot[i] + (float)warp->size.xyz[i] / 2;
        if (!isfinite(o[i]) || !isfinite(r[i]) || !isfinite(c[i]) || o[i] < lo || o[i] > hi) return 0;
    }
    memcpy(new_origin, o, sizeof o); memcpy(new_radius, r, sizeof r); memcpy(new_center, c, sizeof c);
    return 1;
}
