/* Pure map-bounds setting: storage format, clamping and derived limits.
 * Compile map_bounds.c with /DSH_MAP_BOUNDS_PURE (no engine adapters). */
#include "../src/backend/map_bounds.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    sh_map_bounds a,b;char text[33];
    assert(SH_BOUNDS_CEILING==32767.0f-96.0f);
    sh_map_bounds_default(&a);assert(a.limit==0&&sh_map_bounds_valid(&a));
    assert(!sh_map_bounds_overrides(&a)&&sh_map_bounds_effective(&a)==SH_BOUNDS_NATIVE);
    assert(sh_map_bounds_encode(&a,text,sizeof text)&&!strcmp(text,"1;0"));
    assert(sh_map_bounds_decode(text,&b)&&b.limit==0);
    /* Clamping: native or less is native, more clamps to the format ceiling. */
    assert(sh_map_bounds_clamp(0)==0&&sh_map_bounds_clamp(16000)==0&&sh_map_bounds_clamp(-5)==0);
    assert(sh_map_bounds_clamp(16000.5f)==0&&sh_map_bounds_clamp(16001)==16001);
    assert(sh_map_bounds_clamp(24000.7f)==24000);
    assert(sh_map_bounds_clamp(40000)==SH_BOUNDS_CEILING&&sh_map_bounds_clamp(INFINITY)==0);
    assert(sh_map_bounds_clamp(NAN)==0);
    a.limit=24000;assert(sh_map_bounds_valid(&a)&&sh_map_bounds_overrides(&a));
    assert(sh_map_bounds_effective(&a)==24000);
    assert(sh_map_bounds_encode(&a,text,sizeof text)&&!strcmp(text,"1;24000"));
    assert(sh_map_bounds_decode(text,&b)&&b.limit==24000);
    assert(sh_map_bounds_decode("1;32671",&b)&&b.limit==SH_BOUNDS_CEILING);
    /* Out-of-range stored values keep their intent instead of refusing. */
    assert(sh_map_bounds_decode("1;99999",&b)&&b.limit==SH_BOUNDS_CEILING);
    assert(sh_map_bounds_decode("1;12000",&b)&&b.limit==0);
    /* Malformed or unknown versions refuse and leave the output untouched. */
    b.limit=123;
    assert(!sh_map_bounds_decode("2;24000",&b)&&b.limit==123);
    assert(!sh_map_bounds_decode("1;",&b)&&!sh_map_bounds_decode("1;24000x",&b));
    assert(!sh_map_bounds_decode("1;-1",&b)&&!sh_map_bounds_decode("1;nan",&b));
    assert(!sh_map_bounds_decode("1;inf",&b)&&!sh_map_bounds_decode(NULL,&b));
    assert(!sh_map_bounds_decode("1;24000;0",&b)&&b.limit==123);
    /* Invalid in-memory values never encode. */
    a.limit=12000;assert(!sh_map_bounds_valid(&a)&&!sh_map_bounds_encode(&a,text,sizeof text));
    assert(sh_map_bounds_effective(&a)==SH_BOUNDS_NATIVE);
    a.limit=40000;assert(!sh_map_bounds_valid(&a));
    assert(!sh_map_bounds_encode(NULL,text,sizeof text));
    a.limit=24000;assert(!sh_map_bounds_encode(&a,text,4));
    /* The settings row is a toggle: on stores the ceiling, off native, and
     * any stored value above native (the earlier numeric row) reads as on. */
    sh_map_bounds_from_toggle(1,&a);assert(a.limit==SH_BOUNDS_CEILING&&sh_map_bounds_valid(&a));
    assert(sh_map_bounds_encode(&a,text,sizeof text)&&!strcmp(text,"1;32671"));
    assert(sh_map_bounds_decode(text,&b)&&sh_map_bounds_enabled(&b)&&sh_map_bounds_effective(&b)==SH_BOUNDS_CEILING);
    sh_map_bounds_from_toggle(0,&a);assert(a.limit==0&&!sh_map_bounds_enabled(&a));
    assert(sh_map_bounds_encode(&a,text,sizeof text)&&!strcmp(text,"1;0"));
    assert(sh_map_bounds_decode("1;28288",&b)&&sh_map_bounds_enabled(&b)&&sh_map_bounds_effective(&b)==28288);
    assert(sh_map_bounds_decode("1;16000",&b)&&!sh_map_bounds_enabled(&b));
    sh_map_bounds_default(&b);assert(!sh_map_bounds_enabled(&b)&&!sh_map_bounds_enabled(NULL));
    /* Blueprint floors keep the native 25-floors-per-16000 proportion. */
    assert(sh_map_bounds_floors(SH_BOUNDS_NATIVE)==SH_BOUNDS_NATIVE_FLOORS);
    assert(sh_map_bounds_floors(0)==SH_BOUNDS_NATIVE_FLOORS);
    assert(sh_map_bounds_floors(32000)==50);
    assert(sh_map_bounds_floors(SH_BOUNDS_CEILING)==52);
    assert(sh_map_bounds_floors(1e9f)==52);
    printf("map_bounds_test: all passed\n");
    return 0;
}
