#include <cstdio>
#include <vector>

#include "serialization_buffer.h"

static int failures;

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main()
{
    const std::size_t one_mb = 1u * 1024u * 1024u;
    const std::size_t max_cap = 32u * 1024u * 1024u;

    {
        std::vector<char> buffer;
        int calls = 0, observations = 0;
        int result = sh_serialize_growing_buffer(buffer, one_mb, max_cap,
            [&](char *, int cap) {
                calls++;
                return cap < static_cast<int>(2u * one_mb) ? 0 : 1700000;
            },
            [&](std::size_t cap, int value, bool terminal) {
                observations++;
                check(cap == one_mb && value == 0 && !terminal, "first retry observation was wrong");
            });
        check(result == 1700000, "large timeline did not serialize after growth");
        check(calls == 2 && observations == 1, "large timeline used the wrong attempt count");
        check(buffer.size() == 2u * one_mb, "timeline buffer did not grow to 2 MB");

        calls = 0;
        result = sh_serialize_growing_buffer(buffer, one_mb, max_cap,
            [&](char *, int) { calls++; return 1700000; },
            [&](std::size_t, int, bool) { check(false, "retained buffer unexpectedly retried"); });
        check(result == 1700000 && calls == 1, "grown capacity was not retained for the next open");
        check(buffer.size() == 2u * one_mb, "retained buffer capacity changed");
    }

    for (int special : {SH_SERIALIZE_UNAVAILABLE, SH_SERIALIZE_THREW}) {
        std::vector<char> buffer;
        int calls = 0, observations = 0;
        int result = sh_serialize_growing_buffer(buffer, one_mb, max_cap,
            [&](char *, int) { calls++; return special; },
            [&](std::size_t cap, int value, bool terminal) {
                observations++;
                check(cap == one_mb && value == special && terminal,
                      "non-size failure observation was wrong");
            });
        check(result == special, "non-size failure identity was lost");
        check(calls == 1 && observations == 1, "non-size failure was retried");
        check(buffer.size() == one_mb, "non-size failure grew the buffer");
    }

    {
        std::vector<char> buffer;
        int calls = 0;
        int result = sh_serialize_growing_buffer(buffer, one_mb, max_cap,
            [&](char *, int cap) {
                calls++;
                return static_cast<int>(one_mb - 1u < static_cast<std::size_t>(cap - 1)
                    ? one_mb - 1u : static_cast<std::size_t>(cap - 1));
            },
            [&](std::size_t, int, bool) {});
        check(result == static_cast<int>(one_mb - 1u), "exact-boundary result did not resolve");
        check(calls == 2 && buffer.size() == 2u * one_mb,
              "exact-boundary result was not disambiguated with one retry");
    }

    {
        std::vector<char> buffer;
        int calls = 0, terminal_observations = 0;
        int result = sh_serialize_growing_buffer(buffer, one_mb, max_cap,
            [&](char *, int) { calls++; return 0; },
            [&](std::size_t cap, int value, bool terminal) {
                check(value == 0, "zero ladder changed result value");
                if (terminal) {
                    terminal_observations++;
                    check(cap == max_cap, "zero ladder terminated below the safety cap");
                }
            });
        check(result == 0, "safety-cap refusal was reported as success");
        check(calls == 6, "1 MB to 32 MB ladder used the wrong number of attempts");
        check(buffer.size() == max_cap, "safety-cap refusal did not stop at 32 MB");
        check(terminal_observations == 1, "safety-cap refusal was not observed exactly once");
    }

    {
        std::vector<char> buffer;
        int calls = 0;
        int result = sh_serialize_growing_buffer(buffer, one_mb, max_cap,
            [&](char *, int cap) { calls++; return cap - 1; },
            [&](std::size_t, int, bool) {});
        check(result == 0 && calls == 6 && buffer.size() == max_cap,
              "cap-filling writer did not refuse at the 32 MB boundary");
    }

    {
        std::vector<char> buffer;
        int result = sh_serialize_growing_buffer(buffer, 1, max_cap,
            [&](char *, int) { return 1; }, [&](std::size_t, int, bool) {});
        check(result == 0 && buffer.empty(), "invalid capacities changed the buffer");
    }

    if (failures) return 1;
    std::puts("serialization buffer tests passed");
    return 0;
}
