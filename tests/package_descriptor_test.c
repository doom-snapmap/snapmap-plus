#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "package_descriptor.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

static const char *prefix =
    "{\"id\":\"alex.cyberdemon\",\"name\":\"Cyberdemon\"";

static void section_case(const char *section, int expected)
{
    char json[4096], original[4096], error[256];
    sh_package_descriptor descriptor = {0};
    snprintf(json, sizeof(json), "%s%s}", prefix, section);
    memcpy(original, json, strlen(json) + 1u);
    CHECK(sh_package_descriptor_parse(json, strlen(json), &descriptor,
                                        error, sizeof(error)) == expected);
    CHECK(!strcmp(json, original));
    if (expected) {
        CHECK(!strcmp(descriptor.id, "alex.cyberdemon"));
        CHECK(!strcmp(descriptor.name, "Cyberdemon"));
        CHECK(error[0] == '\0');
    } else {
        CHECK(descriptor.fields.members == NULL);
        CHECK(descriptor.id[0] == '\0');
        CHECK(error[0] != '\0');
    }
    sh_package_descriptor_free(&descriptor);
    sh_package_descriptor_free(&descriptor);
}

int main(void)
{
    sh_package_descriptor descriptor = {0};
    char error[128];
    const char *bad[] = {
        "", "{}", "[]", "{} trailing", "{\"schema\":\"future\"}",
        "{\"schema\":\"snapmap-plus.override-package.v1\"}",
        "{\"schema\":null}", "{\"schema\":\"snapmap-plus.package.v2\"}",
        "{\"schema\":\"snapmap-plus.package.v2\",\"id\":\"a\",\"id\":\"b\"}"
    };
    size_t i;
    section_case("", 1);
    section_case(",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}},"
                 "\"strings\":{\"en\":{\"boss_name\":\"Cyberdemon\"},"
                 "\"fr\":{\"boss_name\":\"Cyberdemon\"}},\"hud\":{}", 1);
    section_case(",\"description\":\"Resources stay in assets/\",\"author\":{\"name\":\"Alex\"}", 1);
    section_case(",\"requirements\":[]", 0);
    section_case(",\"hud\":true", 0);
    section_case(",\"strings\":{\"en\":{\"a\":1}}", 0);
    section_case(",\"strings\":{\"en\":{\"a\":\"A\",\"A\":\"B\"}}", 0);
    section_case(",\"strings\":{\"en\":{},\"EN\":{}}", 0);
    section_case(",\"strings\":{\"en\":{\"a\":\"nul\\u0000text\"}}", 0);
    section_case(",\"strings\":{\"en\":{\"nul\\u0000key\":\"text\"}}", 0);
    section_case(",\"strings\":{\"../en\":{}}", 0);
    section_case(",\"strings\":{\"en-US\":{\"a\":\"line\\nquote\\\"\"}}", 1);
    section_case(",\"strings\":{\"en\":{\"a\":\"x\",\"\\u0061\":\"y\"}}", 0);
    /* Native map byte strings do not relax authored package JSON. */
    section_case(",\"description\":\"legacy \x97 text\"", 0);
    section_case(",\"strings\":{\"en\":{\"label\":\"\xe9\"}}", 0);
    section_case(",\"description\":\"UTF-8 \xc3\xa9\"", 1);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!sh_package_descriptor_parse(bad[i], strlen(bad[i]), &descriptor,
                                             error, sizeof(error)));
        CHECK(!descriptor.fields.members);
    }
    CHECK(sh_package_id_valid("a"));
    CHECK(sh_package_id_valid("alex.boss-demons_v2"));
    CHECK(!sh_package_id_valid("../boss"));
    CHECK(!sh_package_id_valid("Boss"));
    CHECK(!sh_package_id_valid("boss..demons"));
    CHECK(!sh_package_id_valid("boss."));
    {
        const char *json = "\xef\xbb\xbf{\"id\":\" \\tAlex.BOSS-Demons\\r\\n\",\"name\":\"Campaign Boss Demons\"}";
        char canonical[SH_PACKAGE_ID_CAP] = "unchanged";
        CHECK(sh_package_descriptor_parse(json, strlen(json), &descriptor, error, sizeof(error)));
        CHECK(!strcmp(descriptor.id, "alex.boss-demons"));
        CHECK(!strcmp(descriptor.name, "Campaign Boss Demons"));
        CHECK(!strcmp(sh_package_descriptor_section(&descriptor, "id"), "\"alex.boss-demons\""));
        sh_package_descriptor_free(&descriptor);
        CHECK(sh_package_id_normalize("  Alex.BOSS-Demons \n", canonical));
        CHECK(!strcmp(canonical, "alex.boss-demons"));
        CHECK(sh_package_id_normalize(canonical, canonical));
        CHECK(!sh_package_id_normalize("Bad ID", canonical));
        CHECK(!sh_package_id_normalize("../Boss", canonical));
        CHECK(!sh_package_id_normalize("Boss..Demon", canonical));
        CHECK(!sh_package_id_normalize(" \r\n ", canonical));
        CHECK(!sh_package_id_normalize("B\xc3\xb6ss", canonical));
        CHECK(!strcmp(canonical, "alex.boss-demons"));
    }
    CHECK(!sh_package_descriptor_parse("{}", 2, &descriptor, error, sizeof(error)));
    CHECK(strstr(error, "missing its required id"));
    {
        const char *invalid = "{\"id\":7,\"name\":\"Name\"}";
        CHECK(!sh_package_descriptor_parse(invalid, strlen(invalid), &descriptor, error, sizeof(error)));
        CHECK(strstr(error, "id must be a string"));
    }
    {
        const char *head = "{\"id\":\"large\",\"name\":\"Large strings\",\"strings\":{\"en\":{\"text\":\"";
        const char *tail = "\"}}}";
        size_t text_size = 2u * 1024u * 1024u;
        size_t length = strlen(head) + text_size + strlen(tail);
        char *json = (char *)malloc(length + 1u);
        CHECK(json);
        if (json) {
            memcpy(json, head, strlen(head));
            memset(json + strlen(head), 'x', text_size);
            memcpy(json + strlen(head) + text_size, tail, strlen(tail) + 1u);
            CHECK(sh_package_descriptor_parse(json, length, &descriptor, error, sizeof(error)));
            CHECK(sh_package_descriptor_section(&descriptor, "strings") != NULL);
            sh_package_descriptor_free(&descriptor);
            json[length - 1] = ']';
            CHECK(!sh_package_descriptor_parse(json, length, &descriptor, error, sizeof(error)));
            CHECK(!descriptor.fields.members);
            free(json);
        }
    }
    CHECK(!sh_package_descriptor_parse("{}", SIZE_MAX,
                                         &descriptor, error, sizeof(error)));
    CHECK(!sh_package_descriptor_parse(NULL, 0, &descriptor, error, sizeof(error)));
    if (failures) return 1;
    puts("package descriptor checks passed");
    return 0;
}
