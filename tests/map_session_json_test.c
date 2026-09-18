#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/map_session_json.h"
#include "../src/backend/config_json.h"

#define SLOT "{\"~type\":\"snapLobbySlotSetting_t\",\"race\":0,\"state\":1,\"team\":2}"
#define SLOTS SLOT "," SLOT "," SLOT "," SLOT
static const char defaults[] = "{\"~type\":\"idSnapMap\",\"~version\":110,\"entities\":[],\"variables\":{}}";
static void rejected(const char *json)
{
    char error[256];
    assert(!sh_map_session_json(json, strlen(json), defaults, strlen(defaults), error, sizeof(error)) && *error);
}
int main(void)
{
    const char *source = "{\"entities\":[{\"inherit\":\"missing/boss\",\"health\":10}],"
        "\"variables\":{\"string\":[\"package payload\",\"legacy \x97 text\"]},\"~type\":\"idSnapMap\","
        "\"~version\":91,\"snapSlotSettings\":[" SLOTS "]}";
    char error[256], *out = sh_map_session_json(source, strlen(source), defaults, strlen(defaults), error, sizeof(error));
    assert(out && !*error && !strstr(out, "missing") && !strstr(out, "payload"));
    assert(strstr(out, "110") && strstr(out, "\"state\":1,\"team\":2"));
    assert(sh_json_validate(out, strlen(out), 128, NULL)); free(out);
    rejected("{}"); rejected("[]"); rejected("null"); rejected("{\"~type\":\"idSnapMap\"}");
    rejected("{\"~type\":\"idSnapMap\",\"~version\":71,\"snapSlotSettings\":[" SLOTS "]}");
    rejected("{\"~type\":\"idSnapMap\",\"~version\":110,\"snapSlotSettings\":[" SLOT "]}");
    rejected("{\"~type\":\"idSnapMap\",\"~version\":110,\"snapSlotSettings\":[" SLOTS "," SLOT "]}");
    rejected("{\"~type\":\"idSnapMap\",\"~version\":110,\"snapSlotSettings\":[" SLOTS ",null]}");
    rejected("{\"~type\":\"idSnapMap\",\"~version\":110,\"snapSlotSettings\":[" SLOTS "],\"entities\":[1,]}");
    rejected("{\"~type\":\"idSnapMap\",\"~version\":110,\"snapSlotSettings\":[" SLOTS "],\"entities\":{\"a\":0,\"\\u0061\":1}}");
    {
        char changed[1024];
        snprintf(changed, sizeof(changed), "%s", source);
        char *value = strstr(changed, "\"team\":2"); assert(value); value[7] = 'x'; rejected(changed);
    }
    puts("map_session_json_test: resource-free native settings and complete-input validation passed"); return 0;
}
