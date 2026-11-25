/* mods_message.h -- how the window's host reads a Mods slot request: the message's route in "cmd" ("modsAction"),
 * the action by name and its argument, mapped to the slot's SH_MODS_ACT_* code. on_message and
 * tests/publishing_key_dispatch_test.cpp share this one definition. */
#pragma once
#include <windows.h>
#include <string>
#include "snapmap_plus_iface.h"
#include "webview_json.h"

namespace sh_mods_message {

inline const wchar_t *action_name(int action)
{
    switch (action) {
    case SH_MODS_ACT_INSTALL:            return L"install";
    case SH_MODS_ACT_RESCAN:             return L"rescan";
    case SH_MODS_ACT_PALETTE:            return L"palette";
    case SH_MODS_ACT_UNINSTALL:          return L"uninstall";
    case SH_MODS_ACT_UNINSTALL_PREVIEW:  return L"uninstallPreview";
    case SH_MODS_ACT_MAP_REMOVE:         return L"mapRemoveMod";
    case SH_MODS_ACT_MAP_REMOVE_PREVIEW: return L"mapRemoveModPreview";
    case SH_MODS_ACT_MAP_ADD:            return L"mapAddMod";
    case SH_MODS_ACT_CHOOSE:             return L"chooseRegistry";
    case SH_MODS_ACT_FILES:              return L"modFiles";
    case SH_MODS_ACT_FILE_TEXT:          return L"modFileText";
    case SH_MODS_ACT_NEW_MOD:            return L"newMod";
    case SH_MODS_ACT_ADD_MINE:           return L"addToMine";
    case SH_MODS_ACT_CONTENT:            return L"content";
    case SH_MODS_ACT_MAP_UPDATE:         return L"mapUpdate";
    case SH_MODS_ACT_KEY:                return L"publishingKey";
    default:                             return L"";
    }
}

/* The slot's code for an action's name, 0 for a name it does not know. */
inline int action_code(const std::wstring &name)
{
    static const int codes[] = { SH_MODS_ACT_INSTALL, SH_MODS_ACT_RESCAN, SH_MODS_ACT_PALETTE, SH_MODS_ACT_UNINSTALL,
                                 SH_MODS_ACT_UNINSTALL_PREVIEW, SH_MODS_ACT_MAP_REMOVE, SH_MODS_ACT_MAP_REMOVE_PREVIEW,
                                 SH_MODS_ACT_MAP_ADD, SH_MODS_ACT_CHOOSE, SH_MODS_ACT_FILES, SH_MODS_ACT_FILE_TEXT,
                                 SH_MODS_ACT_NEW_MOD, SH_MODS_ACT_ADD_MINE, SH_MODS_ACT_CONTENT, SH_MODS_ACT_MAP_UPDATE,
                                 SH_MODS_ACT_KEY };
    for (int code : codes)
        if (!name.empty() && name == action_name(code)) return code;
    return 0;
}

/* The page message's route ("cmd"), "" when it names none: the host reads every page request by it. */
inline std::wstring route(const std::wstring &json)
{
    std::wstring cmd;
    if (!sh_webview_json::get_string(json, L"cmd", cmd)) cmd.clear();
    return cmd;
}

/* A Mods slot request ({"cmd":"modsAction","action":<name>,"arg":<text>}): true with its action's name and argument,
 * false for any other message. */
inline bool parse_action(const std::wstring &json, std::wstring &action, std::wstring &arg)
{
    action.clear();
    arg.clear();
    if (route(json) != L"modsAction") return false;
    sh_webview_json::get_string(json, L"action", action);
    sh_webview_json::get_string(json, L"arg", arg);
    return true;
}

/* Turns a publishing key file request "backup|restore<TAB><passphrase>" into "<op><TAB><path><TAB><passphrase>", the
 * path coming from the native picker `pick(save, path)` so the page never names one. Returns 1 when rewritten, 0 for
 * any other request, -3 when the picker was closed (nothing runs). */
template <class Pick>
inline int key_file_request(std::string &arg, Pick pick)
{
    size_t tab = arg.find('\t');
    std::string op = tab == std::string::npos ? arg : arg.substr(0, tab);
    std::wstring picked;
    if ((op != "backup" && op != "restore") || tab == std::string::npos) return 0;
    if (!pick(op == "backup", picked)) return -3;
    arg = op + "\t" + sh_webview_json::to_utf8(picked) + arg.substr(tab);
    return 1;
}

/* A UTF-8 text as the page's message text (invalid UTF-8: "null"). */
inline std::wstring wide(const std::string &utf8)
{
    if (utf8.empty()) return L"null";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return L"null";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), (int)utf8.size(), &w[0], n);
    return w;
}

/* A refusal or a bare answer to the page: {"kind":"modsActionResult","action","ok","message","data"}. */
inline std::wstring result_message(const wchar_t *action, bool ok, const wchar_t *message, const wchar_t *data)
{
    std::wstring m = L"{\"kind\":\"modsActionResult\",\"action\":\"";
    m += action;
    m += L"\",\"ok\":";
    m += ok ? L"true" : L"false";
    m += L",\"message\":\"";
    m += message;
    m += L"\",\"data\":";
    m += data ? data : L"null";
    m += L"}";
    return m;
}

/* A finished job's answer to the page, from the slot's RUN code and the document FETCH gave after it. */
inline std::wstring finish_message(int action, int run_rc, bool apply_pending, const std::string &run_doc)
{
    const wchar_t *name = action_name(action);
    if (run_rc == -3) return result_message(name, false, L"", L"{\"cancelled\":true}");
    if (run_rc == 1)
        return std::wstring(L"{\"kind\":\"modsActionResult\",\"action\":\"") + name +
               (apply_pending ? L"\",\"applyPending\":true,\"data\":" : L"\",\"data\":") + wide(run_doc) + L"}";
    if (run_rc == -1)
        return result_message(name, false, L"The game did not take the request. Try again with the editor open.", nullptr);
    return result_message(name, false, L"Snapmap+ could not do that. Try again; if it keeps failing, use Send feedback.",
                          nullptr);
}

}  // namespace sh_mods_message
