/* player_text.h -- the description text a player sees, from the generated
 * entity and event tables (src/ui/sh_entity_desc.h, sh_event_docs.h).
 *
 * The tables were written from the game's declarations and from study of its
 * code, so some sentences talk about that study rather than about the game:
 * function names, addresses and offsets, handlers, vtables and decompiles, the
 * runtime class and the raw field list. Those sentences, and parentheticals
 * that carry them, are left out; an entry left with nothing but its own name
 * ("'aas/obstacle' entity.") has no description, and the window then shows
 * none. Kept free of window state so it is testable
 * (tests/player_text_test.cpp). */
#ifndef SNAPMAP_PLUS_PLAYER_TEXT_H
#define SNAPMAP_PLUS_PLAYER_TEXT_H

#include <cctype>
#include <string>
#include <vector>

namespace sh_player_text {

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool is_hex(char c) { c = lower(c); return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

inline bool contains_ci(const std::string &s, const char *needle)
{
    size_t n = 0;
    while (needle[n]) n++;
    if (!n || s.size() < n) return false;
    for (size_t i = 0; i + n <= s.size(); i++) {
        size_t j = 0;
        while (j < n && lower(s[i + j]) == lower(needle[j])) j++;
        if (j == n) return true;
    }
    return false;
}

/* True when the text speaks of the study of the game's code. */
inline bool is_study_text(const std::string &s)
{
    static const char *const words[] = {
        "FUN_", "DAT_", "PTR_", "LAB_", "decompil", "disassembl", "vtable", "handler", "dispatcher",
        "Runtime class", "Runtime C++ class", "Editable properties", "this+", "offset +"
    };
    for (const char *w : words) if (contains_ci(s, w)) return true;
    for (size_t i = 0; i + 2 < s.size(); i++) {
        /* an address or offset: 0x followed by a hex digit */
        if (s[i] == '0' && lower(s[i + 1]) == 'x' && is_hex(s[i + 2])) return true;
    }
    /* an event number: "event 1c3", "event id 63e" */
    for (size_t at = 0; (at = s.find("event ", at)) != std::string::npos; at += 6) {
        size_t i = at + 6;
        if (s.compare(i, 3, "id ") == 0) i += 3;
        else if (s.compare(i, 4, "num ") == 0) i += 4;
        size_t b = i;
        bool digit = false;
        while (i < s.size() && is_hex(s[i])) { if (s[i] >= '0' && s[i] <= '9') digit = true; i++; }
        if (digit && i - b >= 2 && i - b <= 4 && (i == s.size() || !isalnum((unsigned char)s[i]))) return true;
    }
    return false;
}

/* The text without any parenthetical that speaks of the study. */
inline std::string drop_study_parentheticals(const std::string &s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '(') {
            size_t close = s.find(')', i);
            size_t nested = s.find('(', i + 1);
            if (close != std::string::npos && (nested == std::string::npos || nested > close) &&
                is_study_text(s.substr(i, close - i + 1))) {
                while (!out.empty() && out.back() == ' ') out.pop_back();
                i = close + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

/* Split after ". ", "! " or "? " when the next sentence starts with a capital, a digit, a quote or '('. */
inline std::vector<std::string> sentences(const std::string &s)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i + 2 < s.size(); i++) {
        if ((s[i] == '.' || s[i] == '!' || s[i] == '?') && s[i + 1] == ' ') {
            char n = s[i + 2];
            if ((n >= 'A' && n <= 'Z') || (n >= '0' && n <= '9') || n == '\'' || n == '"' || n == '(') {
                out.push_back(s.substr(start, i + 1 - start));
                start = i + 2;
            }
        }
    }
    if (start < s.size()) out.push_back(s.substr(start));
    return out;
}

/* "'aas/obstacle' entity." or "'x' entity (gui category)." -- the entry's name and nothing else. */
inline bool is_name_only(const std::string &s)
{
    if (s.size() < 10 || s[0] != '\'') return false;
    size_t q = s.find('\'', 1);
    if (q == std::string::npos) return false;
    std::string rest = s.substr(q + 1);
    if (rest == " entity.") return true;
    return rest.compare(0, 9, " entity (") == 0 && rest.size() > 11 && rest.compare(rest.size() - 2, 2, ").") == 0 &&
           rest.find(')') == rest.size() - 2;
}

/* The description a player sees, or "" when the entry has none worth showing. */
inline std::string summary(const char *text)
{
    if (!text || !text[0]) return std::string();
    std::string kept;
    for (const std::string &raw : sentences(drop_study_parentheticals(text))) {
        if (is_study_text(raw) || is_name_only(raw)) continue;
        if (!kept.empty()) kept += ' ';
        kept += raw;
    }
    return kept;
}

}  // namespace sh_player_text

#endif
