/* store_main.c -- snapmap-plus-store.exe's entry point (store_tool.h). */
#include <windows.h>

#include "store_tool.h"

int wmain(int argc, wchar_t **argv)
{
    /* The tool prints UTF-8; a console shows it as such. */
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR) SetConsoleOutputCP(CP_UTF8);
    return sh_store_tool_run(argc, argv);
}
