/* Exercise palette capture and concurrent interface reads using the native editor layout. */
#include "../src/backend/budget_table.c"

void backend_log(const char *msg) { (void)msg; }

static unsigned char editor[ED_REACHED_OFF + 8], entry[PAL_STRIDE], sedef[0x500], record[0x100];
static sh_budget_call first;

static DWORD WINAPI read_first(void *unused)
{
    (void)unused;
    first.op = SH_BUDGET_TABLE;
    return sh_budget_table_slot(NULL, &first) ? 0 : 1;
}

int main(void)
{
    char row_name[32] = "initial";
    char model[64], name[64];
    sh_budget_call call = {0};
    editor[ED_INIT_OFF] = 1;
    *(void **)(editor + ED_PALETTE_OFF + PAL_ENTRIES_OFF) = entry;
    *(int *)(editor + ED_PALETTE_OFF + PAL_COUNT_OFF) = 1;
    *(void **)entry = sedef;
    *(void **)(entry + ENTRY_RECORD_OFF) = record;
    *(const char **)(record + RECORD_INHERIT_OFF) = "a/placed/prop";
    *(char **)(sedef + SEDEF_NAME_OFF) = row_name;
    *(const char **)(sedef + SEDEF_RENDERINFO_OFF + RI_MODEL_DATA_OFF) = "first.md6";
    sh_budget_table_tick(editor);
    bt_snapshot *initial = g_snapshot;
    if (!initial || initial->json || initial->head.generation != 1) return 1;
    sh_budget_table_tick(editor);
    if (g_snapshot != initial) return 2;
    if (!(sh_budget_table_row_model("a/placed/prop", name, sizeof name, model, sizeof model, NULL)
          & SH_BT_ROW_MODEL) || strcmp(name, "initial") || strcmp(model, "first.md6")) return 3;
    call.op = SH_BUDGET_USAGE;
    if (!sh_budget_table_slot(NULL, &call) || !call.out || initial->json) return 4;
    sh_iface_text_free(call.out);

    AcquireSRWLockExclusive(&initial->render_lock);
    HANDLE worker = CreateThread(NULL, 0, read_first, NULL, 0, NULL);
    if (!worker) return 5;
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (InterlockedCompareExchange(&initial->refs, 0, 0) != 2) {
        if (GetTickCount64() >= deadline) return 6;
        Sleep(1);
    }
    strcpy_s(row_name, sizeof row_name, "replacement");
    sh_budget_table_palette_rebuilt();
    sh_budget_table_tick(editor);
    if (!g_snapshot || g_snapshot == initial || g_snapshot->json) return 7;
    call.op = SH_BUDGET_TABLE;
    if (!sh_budget_table_slot(NULL, &call) || call.generation != 2 || !strstr(call.out, "replacement")) return 8;
    char *copy = call.out;
    char *cached = g_snapshot->json;
    if (!cached || g_snapshot->table.rows || !sh_budget_table_slot(NULL, &call) ||
        cached != g_snapshot->json || copy == call.out || strcmp(copy, call.out)) return 9;
    sh_iface_text_free(copy);
    sh_iface_text_free(call.out);

    ReleaseSRWLockExclusive(&initial->render_lock);
    if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0 || !first.out || first.generation != 1 ||
        !strstr(first.out, "initial") || strstr(first.out, "replacement")) return 10;
    CloseHandle(worker);
    sh_iface_text_free(first.out);
    bt_snapshot_release(g_snapshot);
    bt_rows_free(&g_rows);
    puts("budget_snapshot_test: OK");
    return 0;
}
