#include <stdio.h>
#include "package_owners.h"

static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); failed++; } } while (0)

int main(void)
{
    sh_package_owners a = {0}, b = {0};
    const size_t indices[] = {0, 1, 63, 64, 127, 128, 4097, 1048579};
    size_t i;
    CHECK(!sh_package_owners_contains(NULL, 0));
    CHECK(!sh_package_owners_add(NULL, 0));
    for (i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
        CHECK(sh_package_owners_add(&a, indices[i]));
        CHECK(sh_package_owners_add(&a, indices[i]));
        CHECK(sh_package_owners_contains(&a, indices[i]));
        CHECK(sh_package_owners_count(&a) == i + 1);
    }
    CHECK(!sh_package_owners_contains(&a, 62));
    CHECK(!sh_package_owners_contains(&a, 65));
    CHECK(!sh_package_owners_contains(&a, SIZE_MAX));
    CHECK(sh_package_owners_add(&b, 65));
    CHECK(sh_package_owners_union(&b, &a));
    CHECK(sh_package_owners_union(&b, &b));
    CHECK(sh_package_owners_count(&b) == 9);
    CHECK(sh_package_owners_count(&a) == 8);
    CHECK(sh_package_owners_within(&a, 1048580));
    CHECK(!sh_package_owners_within(&a, 1048579));
    sh_package_owners_free(&a);
    CHECK(!a.bits && !a.words && !a.more);
    for (i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) CHECK(sh_package_owners_contains(&b, indices[i]));
    CHECK(sh_package_owners_add(&a, 63) && !a.more && !a.words);
    CHECK(sh_package_owners_within(&a, 64) && !sh_package_owners_within(&a, 63));
    CHECK(!sh_package_owners_reserve(&a, SIZE_MAX));
    CHECK(sh_package_owners_count(&a) == 1 && sh_package_owners_contains(&a, 63));
    sh_package_owners_free(&a); sh_package_owners_free(&b); sh_package_owners_free(NULL);
    puts(failed ? "package ownership checks failed" : "package ownership checks passed");
    return failed ? 1 : 0;
}
