/* Exercise the same record ordering and rescan gate used by the UI poll. */
#include <algorithm>
#include <cassert>
#include <cstdio>
#include "crash_pending.h"

int main()
{
    using namespace sh_crash_pending;
    const std::string first = "pending-20260910-120000.json";
    const std::string fatal = "pending-20260910-120000-1.json";
    const std::string later = "pending-20260910-120001.json";
    const std::string older = "pending-20260909-235959.json";
    inventory seen;
    std::vector<std::string> names = {first};
    assert(seen.changed(names));
    assert(!seen.changed(names));
    names.push_back(fatal);
    std::sort(names.begin(), names.end(), newest_first);
    assert(names.front() == fatal && seen.changed(names));
    assert(!seen.changed(names));

    names.push_back(later);
    names.push_back("pending-20260910-120000-10.json");
    names.push_back("pending-20260910-120000-9.json");
    std::sort(names.begin(), names.end(), newest_first);
    assert(names.front() == later && seen.changed(names));
    assert(names[1] == "pending-20260910-120000-10.json");
    assert(names[2] == "pending-20260910-120000-9.json");
    names.push_back(older);
    std::sort(names.begin(), names.end(), newest_first);
    assert(names.front() == later && seen.changed(names));
    names.pop_back();
    assert(seen.changed(names));
    seen.clear();
    assert(seen.changed(names));
    names.clear();
    assert(seen.changed(names) && !seen.changed(names));

    unsigned value = 0;
    assert(sequence(first, value) && value == 0);
    assert(sequence(fatal, value) && value == 1);
    assert(!sequence("pending-20260910-120000-.json", value));
    assert(!sequence("pending-20260910-120000-4294967296.json", value));
    assert(!sequence("pending-20260910-120000-nope.json", value));
    assert(!sequence("pending-20260910-120000.json.tmp", value));
    assert(newest_first(first, "pending-invalid.json"));
    assert(!newest_first(first, first));
    puts("crash_pending_test OK");
    return 0;
}
