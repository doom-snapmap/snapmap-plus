/* A long backend log record is truncated but still ends its line, so the next
 * record starts on its own line. */
#include "../src/backend/backend_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(void)
{
    char path[MAX_PATH], text[8192], big[3000];
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;
    FILE *file;
    size_t read;
    assert(length && length < MAX_PATH);
    slash = strrchr(path, '\\'); assert(slash); slash[1] = 0;
    strcat_s(path, sizeof(path), "snapmap-plus\\logs\\sh_backend.log");
    DeleteFileA(path);
    backend_set_logpath_from_module(NULL);
    memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    backend_log(big);
    backend_log("after");
    backend_log_close();
    file = fopen(path, "rb"); assert(file);
    read = fread(text, 1, sizeof(text) - 1, file); fclose(file); text[read] = 0;
    char *second = strstr(text, "\n[snapmap+] ");
    assert(second && (size_t)(second - text) < 1024 && strstr(second, " after\n"));
    DeleteFileA(path);
    puts("backend_log_test: PASS");
    return 0;
}
