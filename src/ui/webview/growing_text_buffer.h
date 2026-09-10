#ifndef SNAPMAP_PLUS_GROWING_TEXT_BUFFER_H
#define SNAPMAP_PLUS_GROWING_TEXT_BUFFER_H

#include <cstddef>
#include <vector>

/* Grow and reuse storage until a NUL-terminated engine string fits below cap-1.
 * The copy API does not report source length, so an exact fit needs an extra read.
 * At max_cap, set truncated and refuse partial editable text. */
template <typename CopyFn>
static bool sh_read_growing_text(std::vector<char> &buffer,
                                 std::size_t initial_cap,
                                 std::size_t max_cap,
                                 CopyFn copy,
                                 bool *truncated)
{
    if (truncated) *truncated = false;
    if (initial_cap < 2 || max_cap < initial_cap) return false;

    if (buffer.size() < initial_cap) buffer.resize(initial_cap);
    if (buffer.size() > max_cap) buffer.resize(max_cap);

    for (;;) {
        buffer[0] = '\0';
        buffer[buffer.size() - 1] = '\0';
        if (!copy(buffer.data(), static_cast<int>(buffer.size()))) {
            buffer[0] = '\0';
            return false;
        }

        std::size_t len = 0;
        while (len < buffer.size() && buffer[len] != '\0') ++len;
        if (len < buffer.size() - 1) return true;

        if (buffer.size() == max_cap) {
            if (truncated) *truncated = true;
            return true;
        }

        std::size_t next = buffer.size() * 2;
        if (next < buffer.size() || next > max_cap) next = max_cap;
        buffer.resize(next);
    }
}

#endif
