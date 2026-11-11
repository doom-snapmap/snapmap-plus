/* store_recycle.c -- see store_recycle.h. */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

#include "store_recycle.h"

/* The plain spelling of a long-form path: \\?\C:\x -> C:\x, \\?\UNC\s\x -> \\s\x. */
static wchar_t *rc_plain(const wchar_t *path)
{
    size_t n = wcslen(path);
    wchar_t *out = (wchar_t *)malloc(sizeof(wchar_t) * (n + 3));
    if (!out) return NULL;
    if (!wcsncmp(path, L"\\\\?\\UNC\\", 8)) {
        out[0] = L'\\';
        out[1] = L'\\';
        wcscpy_s(out + 2, n + 1, path + 8);
    } else if (!wcsncmp(path, L"\\\\?\\", 4)) {
        wcscpy_s(out, n + 3, path + 4);
    } else {
        wcscpy_s(out, n + 3, path);
    }
    return out;
}

static wchar_t *rc_join(const wchar_t *dir, const wchar_t *leaf)
{
    size_t a = wcslen(dir), b = wcslen(leaf);
    wchar_t *p = (wchar_t *)malloc(sizeof(wchar_t) * (a + b + 2));
    if (!p) return NULL;
    memcpy(p, dir, sizeof(wchar_t) * a);
    p[a] = L'\\';
    memcpy(p + a + 1, leaf, sizeof(wchar_t) * (b + 1));
    return p;
}

/* Remove a link itself (a reparse point), never what it points at. */
static int rc_unlink(const wchar_t *path, DWORD attributes)
{
    SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(path) != 0;
    return DeleteFileW(path) != 0;
}

/* Remove every link inside a folder tree (links are not walked into). */
static int rc_unlink_inside(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = rc_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    int ok = 1;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_NO_MORE_FILES;
    do {
        wchar_t *child;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        child = rc_join(dir, fd.cFileName);
        if (!child) { ok = 0; continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (!rc_unlink(child, fd.dwFileAttributes)) ok = 0; }
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!rc_unlink_inside(child)) ok = 0; }
        free(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* ---------------------------------------------- the delete's progress sink ---- */

/* The shell tells the sink, before each item, whether it can recycle it. An item the Recycle Bin
 * cannot take (the bin is turned off for the drive or the item is larger than it) would be deleted
 * for good, so the sink stops the operation there and the item stays where it is. */
typedef struct rc_sink {
    IFileOperationProgressSink iface;
    LONG refs;
    int refused;       /* an item would have been deleted for good */
    int recycled;      /* the shell reported an item moved to the Recycle Bin */
} rc_sink;

static HRESULT STDMETHODCALLTYPE rc_qi(IFileOperationProgressSink *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IFileOperationProgressSink)) {
        *out = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE rc_addref(IFileOperationProgressSink *This)
{
    return (ULONG)InterlockedIncrement(&((rc_sink *)This)->refs);
}
static ULONG STDMETHODCALLTYPE rc_release(IFileOperationProgressSink *This)
{
    return (ULONG)InterlockedDecrement(&((rc_sink *)This)->refs);  /* the sink lives on the stack */
}
static HRESULT STDMETHODCALLTYPE rc_start(IFileOperationProgressSink *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_finish(IFileOperationProgressSink *This, HRESULT hr) { (void)This; (void)hr; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_prerename(IFileOperationProgressSink *This, DWORD f, IShellItem *i, LPCWSTR n)
{ (void)This; (void)f; (void)i; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_postrename(IFileOperationProgressSink *This, DWORD f, IShellItem *i, LPCWSTR n, HRESULT hr, IShellItem *c)
{ (void)This; (void)f; (void)i; (void)n; (void)hr; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_premove(IFileOperationProgressSink *This, DWORD f, IShellItem *i, IShellItem *d, LPCWSTR n)
{ (void)This; (void)f; (void)i; (void)d; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_postmove(IFileOperationProgressSink *This, DWORD f, IShellItem *i, IShellItem *d, LPCWSTR n, HRESULT hr, IShellItem *c)
{ (void)This; (void)f; (void)i; (void)d; (void)n; (void)hr; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_predelete(IFileOperationProgressSink *This, DWORD flags, IShellItem *item)
{
    (void)item;
    if (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) return S_OK;
    ((rc_sink *)This)->refused = 1;
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
}
static HRESULT STDMETHODCALLTYPE rc_postdelete(IFileOperationProgressSink *This, DWORD flags, IShellItem *item, HRESULT hr, IShellItem *created)
{
    (void)flags; (void)item;
    if (SUCCEEDED(hr) && created) ((rc_sink *)This)->recycled = 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE rc_prenew(IFileOperationProgressSink *This, DWORD f, IShellItem *d, LPCWSTR n)
{ (void)This; (void)f; (void)d; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_postnew(IFileOperationProgressSink *This, DWORD f, IShellItem *d, LPCWSTR n, LPCWSTR t, DWORD a, HRESULT hr, IShellItem *c)
{ (void)This; (void)f; (void)d; (void)n; (void)t; (void)a; (void)hr; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_progress(IFileOperationProgressSink *This, UINT total, UINT sofar)
{ (void)This; (void)total; (void)sofar; return S_OK; }
static HRESULT STDMETHODCALLTYPE rc_timer(IFileOperationProgressSink *This) { (void)This; return S_OK; }

static IFileOperationProgressSinkVtbl rc_sink_vtbl = {
    rc_qi, rc_addref, rc_release, rc_start, rc_finish, rc_prerename, rc_postrename, rc_premove, rc_postmove,
    rc_premove, rc_postmove, rc_predelete, rc_postdelete, rc_prenew, rc_postnew, rc_progress, rc_timer, rc_timer, rc_timer,
};

/* Move plain to the Recycle Bin, never deleting it for good. Returns 1 when the shell recycled it. */
static int rc_shell_recycle(const wchar_t *plain)
{
    rc_sink sink;
    IFileOperation *op = NULL;
    IShellItem *item = NULL;
    DWORD cookie = 0;
    HRESULT hr, init;
    BOOL aborted = FALSE;
    int ok = 0;
    memset(&sink, 0, sizeof sink);
    sink.iface.lpVtbl = &rc_sink_vtbl;
    sink.refs = 1;
    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_ALL, &IID_IFileOperation, (void **)&op);
    if (SUCCEEDED(hr))
        hr = IFileOperation_SetOperationFlags(op, FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION |
                                                  FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(plain, NULL, &IID_IShellItem, (void **)&item);
    if (SUCCEEDED(hr)) hr = IFileOperation_Advise(op, &sink.iface, &cookie);
    if (SUCCEEDED(hr)) hr = IFileOperation_DeleteItem(op, item, NULL);
    if (SUCCEEDED(hr)) hr = IFileOperation_PerformOperations(op);
    if (SUCCEEDED(hr)) hr = IFileOperation_GetAnyOperationsAborted(op, &aborted);
    if (SUCCEEDED(hr) && !aborted && !sink.refused && sink.recycled) ok = 1;
    if (cookie) IFileOperation_Unadvise(op, cookie);
    if (item) IShellItem_Release(item);
    if (op) IFileOperation_Release(op);
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) SetLastError(sink.refused ? ERROR_NOT_SUPPORTED : (FAILED(hr) ? (DWORD)HRESULT_CODE(hr) : ERROR_CANCELLED));
    return ok;
}

int sh_store_recycle(void *context, const wchar_t *path)
{
    sh_store_recycle_stats *st = (sh_store_recycle_stats *)context;
    wchar_t *plain, drive[4];
    DWORD a;
    size_t n;
    int ok = 0;
    if (!path || !path[0]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return rc_unlink(path, a);
    plain = rc_plain(path);
    if (!plain) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    n = wcslen(plain);
    /* The Recycle Bin lives on fixed local disks; elsewhere the shell would
     * delete for good, so nothing is deleted. */
    if (n < 3 || plain[1] != L':' || plain[2] != L'\\') {
        free(plain);
        if (st) st->refused++;
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    drive[0] = plain[0]; drive[1] = L':'; drive[2] = L'\\'; drive[3] = 0;
    if (GetDriveTypeW(drive) != DRIVE_FIXED) {
        free(plain);
        if (st) st->refused++;
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (n >= MAX_PATH) {
        free(plain);
        if (st) st->refused++;
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !rc_unlink_inside(path)) {
        free(plain);
        if (st) st->refused++;
        return 0;
    }
    if (rc_shell_recycle(plain) && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) ok = 1;
    if (st) { if (ok) st->recycled++; else st->refused++; }
    free(plain);
    return ok;
}
