/* Pure map-bounds setting: storage format and derived limits.
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
    assert(!sh_map_bounds_encode(&a,text,sizeof text));
    sh_map_bounds_from_toggle(1,&a);
    assert(sh_map_bounds_overrides(&a) && sh_map_bounds_effective(&a)==SH_BOUNDS_CEILING);
    assert(sh_map_bounds_encode(&a,text,sizeof text) && !strcmp(text,"1;32671"));
    assert(sh_map_bounds_decode(text,&b) && b.limit==a.limit);
    assert(!sh_map_bounds_encode(&a,text,strlen(text)));
    assert(!sh_map_bounds_decode("1;0",&b) && !sh_map_bounds_decode("1;20000",&b));
    assert(!sh_map_bounds_decode("1;32671.0",&b) && !sh_map_bounds_decode("1;32671junk",&b));
    assert(!sh_map_bounds_decode(NULL,&b) && !sh_map_bounds_decode("1;32671",NULL));
    assert(b.limit==SH_BOUNDS_CEILING);
    a.limit=INFINITY;assert(!sh_map_bounds_valid(&a) && !sh_map_bounds_encode(&a,text,sizeof text));
    sh_map_bounds_from_toggle(0,&a);assert(!sh_map_bounds_overrides(&a));
    /* Blueprint floors keep the native 25-floors-per-16000 proportion. */
    assert(sh_map_bounds_floors(SH_BOUNDS_NATIVE)==SH_BOUNDS_NATIVE_FLOORS);
    assert(sh_map_bounds_floors(0)==SH_BOUNDS_NATIVE_FLOORS);
    assert(sh_map_bounds_floors(32000)==50);
    assert(sh_map_bounds_floors(SH_BOUNDS_CEILING)==52);
    assert(sh_map_bounds_floors(1e9f)==52);
    printf("map_bounds_test: all passed\n");
    return 0;
}
