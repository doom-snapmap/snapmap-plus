/* Pure category arithmetic of the Snapmap+ settings tab. Native code wraps
 * modulo 2 (and 0x80000001); the tab wraps modulo the session's count. Also
 * the float-row unwrap helper (rows are now created without the native wrap).
 * Compile settings_tab.c with /DSH_SETTINGS_TAB_PURE. */
#include "../src/backend/settings_tab.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
int main(void)
{
    int c,i;
    /* Two categories reproduce the native cycle in both directions. */
    assert(sh_settings_tab_wrap(0,1,2)==1&&sh_settings_tab_wrap(1,1,2)==0);
    assert(sh_settings_tab_wrap(0,-1,2)==1&&sh_settings_tab_wrap(1,-1,2)==0);
    /* Three categories visit the Snapmap+ tab and come back. */
    for(c=0,i=0;i<3;i++)c=sh_settings_tab_wrap(c,1,3);
    assert(c==0);
    assert(sh_settings_tab_wrap(1,1,3)==2&&sh_settings_tab_wrap(2,1,3)==0);
    assert(sh_settings_tab_wrap(0,-1,3)==2&&sh_settings_tab_wrap(2,-1,3)==1);
    /* A remembered category beyond a smaller count folds back into range. */
    assert(sh_settings_tab_wrap(2,0,2)==0);
    assert(sh_settings_tab_wrap(INT_MAX,1,3)>=0&&sh_settings_tab_wrap(INT_MIN,-1,3)>=0);
    assert(sh_settings_tab_wrap(5,0,0)==0);
    /* Float rows wrap natively; the tab clamps toward the limit stepped at.
     * View Distance 0..200000 (256 / 4096): 0 down -> 200000 is 0. */
    assert(sh_settings_tab_unwrap(0,200000,0,200000,256,4096)==0);
    assert(sh_settings_tab_unwrap(200000,0,0,200000,256,4096)==200000);
    /* An off-grid start near a limit and a large step wrap the same way. */
    assert(sh_settings_tab_unwrap(100,199844,0,200000,256,4096)==0);
    assert(sh_settings_tab_unwrap(199000,4000,0,200000,256,4096)==200000);
    /* Unlock Map Bounds 16000..32671: 16000 down -> 32671 stays 16000. */
    assert(sh_settings_tab_unwrap(16000,32671,16000,32671,256,4096)==16000);
    assert(sh_settings_tab_unwrap(32671,16000,16000,32671,256,4096)==32671);
    /* Ordinary steps, steps onto a limit and steps away from it are kept. */
    assert(sh_settings_tab_unwrap(8192,7936,0,200000,256,4096)==7936);
    assert(sh_settings_tab_unwrap(256,0,0,200000,256,4096)==0);
    assert(sh_settings_tab_unwrap(0,256,0,200000,256,4096)==256);
    assert(sh_settings_tab_unwrap(200000,199744,0,200000,256,4096)==199744);
    assert(sh_settings_tab_unwrap(199744,200000,0,200000,256,4096)==200000);
    assert(sh_settings_tab_unwrap(16000,20096,16000,32671,256,4096)==20096);
    /* A long jump that is not end to end is not a wrap. */
    assert(sh_settings_tab_unwrap(200000,190000,0,200000,256,4096)==190000);
    /* Ranges too narrow to tell a wrap from a step, and bad input, pass through. */
    assert(sh_settings_tab_unwrap(0,10,0,10,4,8)==10);
    assert(sh_settings_tab_unwrap(0,200000,0,200000,0,0)==200000);
    assert(isnan(sh_settings_tab_unwrap(0,NAN,0,200000,256,4096)));
    assert(sh_settings_tab_unwrap(NAN,5,0,200000,256,4096)==5);
    printf("settings_tab_test: all passed\n");
    return 0;
}
