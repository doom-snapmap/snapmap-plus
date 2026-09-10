#pragma once
#include <climits>
#include <string>
#include <vector>

namespace sh_crash_pending {

/* The producer uses pending-YYYYMMDD-HHMMSS[-N].json, with no suffix for N=0. */
inline bool sequence(const std::string &name, unsigned &value)
{
    if (name.size() < 28 || name.compare(0, 8, "pending-") != 0 ||
        name[16] != '-' || name.compare(name.size() - 5, 5, ".json") != 0) return false;
    for (size_t i = 8; i < 23; ++i)
        if (i != 16 && (name[i] < '0' || name[i] > '9')) return false;
    value = 0;
    if (name.size() == 28) return true;
    if (name.size() < 30 || name[23] != '-') return false;
    for (size_t i = 24; i < name.size() - 5; ++i) {
        unsigned digit = (unsigned)(name[i] - '0');
        if (digit > 9 || value > (UINT_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

inline bool newest_first(const std::string &a, const std::string &b)
{
    unsigned as = 0, bs = 0;
    bool av = sequence(a, as), bv = sequence(b, bs);
    if (av != bv) return av;
    if (av) {
        int stamp = a.compare(0, 23, b, 0, 23);
        if (stamp) return stamp > 0;
        if (as != bs) return as > bs;
    }
    return a > b;
}

/* Reclassify whenever any retained filename changes, including older arrivals. */
class inventory {
    std::vector<std::string> seen;
public:
    bool changed(const std::vector<std::string> &names) {
        if (seen == names) return false;
        seen = names;
        return true;
    }
    void clear() { seen.clear(); }
};

} // namespace sh_crash_pending
