/* editor_undo_test -- Uninstall's undo-history clear (editor_undo.c) against a
 * modelled editor: the idList at *(editor+0x10) and a push that does what the
 * engine's does (Vulkan RVA 0x12039F0, OpenGL RVA 0x1202CE0: advance the
 * cursor, destroy every entry from it to the end, compact, append a copy of
 * the live edit map, cursor on it), plus the decode of the push from
 * EditorCommitEdit's bytes. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "editor_undo.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

/* The undo list's header, as the engine lays it out. */
typedef struct model_list {
    void **data;     /* +0x00 */
    int count;       /* +0x08 */
    int capacity;    /* +0x0c */
    void *pad;       /* +0x10 */
    void **cursor;   /* +0x18 */
} model_list;

typedef struct snapshot { int live_generation; int destroyed; } snapshot;

static uint8_t g_editor[0x20600];
static int g_live_generation;    /* what the live edit map holds now */
static int g_destroyed, g_allocated;
static int g_fault;

static model_list *list_of(void) { return *(model_list **)(g_editor + SH_EDITOR_UNDO_LIST_OFF); }

/* The engine's push, line for line with the decompiled body. */
static void model_push(void *l, void *editor)
{
    model_list *list = (model_list *)l;
    void **p, **w, **end;
    snapshot *s;
    (void)editor;
    if (g_fault) { volatile int *bad = NULL; *bad = 1; }
    list->cursor = list->cursor + 1;
    end = list->data + list->count;
    for (p = list->cursor; p != end; p++)
        if (*p) { ((snapshot *)*p)->destroyed = 1; g_destroyed++; free(*p); }
    w = list->cursor;
    if (w != end) {
        for (p = end; p != list->data + list->count; p++) *w++ = *p;   /* nothing follows the end */
        list->count = (int)(w - list->data) < list->capacity ? (int)(w - list->data) : list->capacity;
    }
    s = (snapshot *)calloc(1, sizeof *s);
    s->live_generation = g_live_generation;
    g_allocated++;
    if (list->count < list->capacity) list->data[list->count++] = s;
    list->cursor = list->data + list->count - 1;
}

static void make_history(int count, int cursor_at, int capacity)
{
    model_list *list = (model_list *)calloc(1, sizeof *list);
    int i;
    list->data = (void **)calloc((size_t)capacity, sizeof(void *));
    list->capacity = capacity;
    for (i = 0; i < count; i++) {
        snapshot *s = (snapshot *)calloc(1, sizeof *s);
        s->live_generation = i;
        list->data[i] = s;
    }
    list->count = count;
    list->cursor = list->data + cursor_at;
    *(model_list **)(g_editor + SH_EDITOR_UNDO_LIST_OFF) = list;
    g_destroyed = g_allocated = 0;
}

static void free_history(void)
{
    model_list *list = list_of();
    int i;
    if (!list) return;
    for (i = 0; i < list->count; i++) free(list->data[i]);
    free(list->data);
    free(list);
    *(model_list **)(g_editor + SH_EDITOR_UNDO_LIST_OFF) = NULL;
}

int main(void)
{
    int before, after;
    model_list *list;

    printf("-- after an uninstall's delete: every snapshot goes, the current state stays alone\n");
    /* Five snapshots, the cursor on the fourth (one redo entry after it); the
     * live map is the delete's result. */
    make_history(5, 3, 8);
    g_live_generation = 42;
    CHECK(sh_editor_undo_truncate(g_editor, model_push, &before, &after) == 1);
    list = list_of();
    CHECK(before == 5 && after == 1 && list->count == 1);
    CHECK(g_destroyed == 5 && g_allocated == 1);               /* the undo and the redo entries alike */
    CHECK(list->cursor == list->data && ((snapshot *)list->data[0])->live_generation == 42);
    CHECK(list->capacity == 8);
    /* Undo has nothing to step back to: the cursor is the first entry. */
    CHECK(list->cursor - list->data == 0);
    free_history();

    printf("-- a history of one, and an empty one\n");
    make_history(1, 0, 4);
    g_live_generation = 7;
    CHECK(sh_editor_undo_truncate(g_editor, model_push, &before, &after) == 1 && before == 1 && after == 1);
    CHECK(g_destroyed == 1 && ((snapshot *)list_of()->data[0])->live_generation == 7);
    free_history();
    make_history(0, -1, 4);
    CHECK(sh_editor_undo_truncate(g_editor, model_push, &before, &after) == 1 && before == 0 && after == 1);
    free_history();

    printf("-- a list this code does not know is left alone\n");
    make_history(3, 1, 4);
    list = list_of();
    list->cursor = list->data + 3;                             /* past the entries */
    CHECK(sh_editor_undo_truncate(g_editor, model_push, &before, &after) == 0 && g_destroyed == 0 && list->count == 3);
    list->cursor = (void **)((uint8_t *)list->data + 4);       /* not on an entry */
    CHECK(sh_editor_undo_truncate(g_editor, model_push, NULL, NULL) == 0 && g_destroyed == 0);
    list->cursor = list->data + 1;
    list->count = 5;                                           /* more entries than room */
    CHECK(sh_editor_undo_truncate(g_editor, model_push, NULL, NULL) == 0 && g_destroyed == 0);
    list->count = 3;
    CHECK(sh_editor_undo_truncate(g_editor, NULL, NULL, NULL) == 0);
    CHECK(sh_editor_undo_truncate(NULL, model_push, NULL, NULL) == 0);
    free_history();
    CHECK(sh_editor_undo_truncate(g_editor, model_push, &before, NULL) == 0 && before == -1);   /* no list */

    printf("-- a push that faults is reported, not survived silently\n");
    make_history(2, 1, 4);
    g_fault = 1;
    CHECK(sh_editor_undo_truncate(g_editor, model_push, NULL, NULL) == -1);
    g_fault = 0;
    list_of()->cursor = list_of()->data;
    free_history();

    printf("-- the push is decoded from EditorCommitEdit's body\n");
    {
        /* The 13954591 Vulkan bytes of EditorCommitEdit (RVA 0x1183D60), its
         * wrapper (RVA 0x1203B40) and the push (RVA 0x12039F0), laid out at
         * their relative distances in a model image. */
        static const uint8_t COMMIT[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xC8, 0x04, 0x02, 0x00,
            0x48, 0x83, 0xC1, 0x18, 0xE8, 0xA7, 0x67, 0x1B, 0xFF, 0x48, 0x8D, 0x4B, 0x10, 0x48, 0x8B, 0xD3,
            0xE8, 0xBB, 0xFD, 0x07, 0x00 };
        size_t size = 0x100000;
        uint8_t *image = (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        size_t commit = 0x1000, wrapper = commit + (0x1203B40 - 0x1183D60), push = commit + (0x12039F0 - 0x1183D60);
        int32_t rel;
        CHECK(image != NULL);
        if (image) {
            memcpy(image + commit, COMMIT, sizeof COMMIT);
            image[wrapper] = 0x48; image[wrapper + 1] = 0x8B; image[wrapper + 2] = 0x09; image[wrapper + 3] = 0xE9;
            rel = (int32_t)(push - (wrapper + 8));
            memcpy(image + wrapper + 4, &rel, 4);
            CHECK(sh_editor_undo_decode_push(image + commit, image, size) == (uintptr_t)(image + push));
            /* Another wrapper shape, another call, or a target outside the image: nothing. */
            image[wrapper + 2] = 0x01;
            CHECK(sh_editor_undo_decode_push(image + commit, image, size) == 0);
            image[wrapper + 2] = 0x09;
            image[commit + SH_EDITOR_UNDO_COMMIT_CALL] = 0xE9;
            CHECK(sh_editor_undo_decode_push(image + commit, image, size) == 0);
            image[commit + SH_EDITOR_UNDO_COMMIT_CALL] = 0xE8;
            CHECK(sh_editor_undo_decode_push(image + commit, image, wrapper) == 0);
            CHECK(sh_editor_undo_decode_push(NULL, image, size) == 0);
            VirtualFree(image, 0, MEM_RELEASE);
        }
    }

    printf("editor_undo_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
