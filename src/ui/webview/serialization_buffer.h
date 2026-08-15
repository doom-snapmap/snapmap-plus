#ifndef SNAPMAP_PLUS_SERIALIZATION_BUFFER_H
#define SNAPMAP_PLUS_SERIALIZATION_BUFFER_H

#include <climits>
#include <cstddef>
#include <vector>

enum {
    SH_SERIALIZE_UNAVAILABLE = -1,
    SH_SERIALIZE_THREW = -2
};

/* Retry an ambiguous entity serializer with a reusable doubling buffer.
 *
 * A clean zero and a result that fills cap-1 may both mean truncation, so they advance to the next
 * capacity. Negative results are terminal non-size failures. `observe` receives every unsuccessful
 * attempt and whether it is terminal, allowing the host to log the exact growth ladder. */
template <typename SerializeFn, typename ObserveFn>
static int sh_serialize_growing_buffer(std::vector<char> &buffer,
                                       std::size_t initial_cap,
                                       std::size_t max_cap,
                                       SerializeFn serialize,
                                       ObserveFn observe)
{
    if (initial_cap < 2 || max_cap < initial_cap || max_cap > static_cast<std::size_t>(INT_MAX))
        return 0;
    if (buffer.size() < initial_cap) buffer.resize(initial_cap);
    if (buffer.size() > max_cap) buffer.resize(max_cap);

    for (;;) {
        int n = serialize(buffer.data(), static_cast<int>(buffer.size()));
        if (n > 0 && static_cast<std::size_t>(n) < buffer.size() - 1u) return n;

        bool terminal = n < 0 || buffer.size() >= max_cap;
        observe(buffer.size(), n, terminal);
        if (terminal) return n < 0 ? n : 0;

        std::size_t next = buffer.size() * 2u;
        if (next < buffer.size() || next > max_cap) next = max_cap;
        buffer.resize(next);
    }
}

#endif
