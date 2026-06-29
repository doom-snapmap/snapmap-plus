/* shield_format_test.c -- pure-logic test for shield_format (no game, no engine). */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/fault_shield/fault_record.h"

int main(void)
{
    shield_fault f = { "load", -1, "access-violation @ 0xdeadbeef", 0x595a40, 0xdeadbeef };
    char buf[256];
    int n = shield_format(buf, sizeof buf, &f);
    assert(n > 0);
    assert(strstr(buf, "class=load"));
    assert(strstr(buf, "sev=-1"));
    assert(strstr(buf, "rva=0x595a40"));
    assert(strstr(buf, "addr=0xdeadbeef"));
    assert(strstr(buf, "access-violation @ 0xdeadbeef"));
    printf("shield_format_test OK: %s\n", buf);
    return 0;
}
