/* xinput_ordinal_test.c -- prove the XINPUT1_3.dll ordinal fix at runtime.
 *
 * DOOM imports XINPUT1_3.dll BY ORDINAL: ord 2 = XInputGetState, ord 3 = XInputSetState. This harness
 * resolves those exports BY ORDINAL (exactly as DOOM's loader does) and calls them the way DOOM does --
 * GetState(idx, &XINPUT_STATE), SetState(idx, &XINPUT_VIBRATION). On the BROKEN build ordinal 2 was
 * XInputGetBatteryInformation, so this same call wrote a battery struct through an uninitialised 3rd
 * pointer -> wild write. On the FIXED build it must return a clean code (1167 ERROR_DEVICE_NOT_CONNECTED
 * with no pad, or 0) and leave a guard region untouched. SEH-guarded so a regression shows as a caught AV
 * rather than killing the harness.
 */
#include <windows.h>
#include <stdio.h>

typedef DWORD (WINAPI *fn2)(DWORD, void *);   /* XInputGetState(idx, XINPUT_STATE*)     */
typedef DWORD (WINAPI *fn3)(DWORD, void *);   /* XInputSetState(idx, XINPUT_VIBRATION*) */

int main(int argc, char **argv)
{
    const char *dll = (argc > 1) ? argv[1] : "XINPUT1_3.dll";
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("FAIL: LoadLibrary(%s) err=%lu\n", dll, GetLastError()); return 2; }

    fn2 f2 = (fn2)GetProcAddress(h, (LPCSTR)(ULONG_PTR)2);   /* by ORDINAL 2 */
    fn3 f3 = (fn3)GetProcAddress(h, (LPCSTR)(ULONG_PTR)3);   /* by ORDINAL 3 */
    printf("ordinal 2 -> %p   ordinal 3 -> %p\n", (void *)f2, (void *)f3);
    if (!f2 || !f3) { printf("FAIL: an ordinal did not resolve\n"); return 3; }

    /* XINPUT_STATE is 16 bytes; bracket it with 0xCC guards to catch any overrun. */
    struct { unsigned char pre[32]; unsigned char state[16]; unsigned char post[32]; } b;
    memset(&b, 0xCC, sizeof(b));

    DWORD rc2 = 0xDEADBEEF;
    __try { rc2 = f2(0, b.state); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("FAIL: ordinal-2 call raised 0x%08lx (REGRESSION)\n", GetExceptionCode()); return 4; }

    int overrun = 0;
    for (int i = 0; i < 32; i++) if (b.pre[i] != 0xCC || b.post[i] != 0xCC) overrun = 1;

    /* XINPUT_VIBRATION is 4 bytes; SetState only reads it -> should be a clean no-op without a pad. */
    unsigned char vib[4] = {0,0,0,0};
    DWORD rc3 = 0xDEADBEEF;
    __try { rc3 = f3(0, vib); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("FAIL: ordinal-3 call raised 0x%08lx (REGRESSION)\n", GetExceptionCode()); return 5; }

    printf("XInputGetState(0) via ord 2 -> %lu  (1167=DEVICE_NOT_CONNECTED ok, 0=ok)\n", rc2);
    printf("XInputSetState(0) via ord 3 -> %lu  (1167 ok, 0 ok)\n", rc3);
    printf("guard region intact = %s\n", overrun ? "NO -- OVERRUN (REGRESSION)" : "yes");

    int pass = !overrun && (rc2 == ERROR_DEVICE_NOT_CONNECTED || rc2 == ERROR_SUCCESS)
                        && (rc3 == ERROR_DEVICE_NOT_CONNECTED || rc3 == ERROR_SUCCESS);
    printf("RESULT: %s\n", pass ? "PASS -- ordinals call the correct functions, no corruption" : "FAIL");
    return pass ? 0 : 1;
}
