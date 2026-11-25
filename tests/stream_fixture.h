/* Call the published idFile ABI so a misplaced slot also fails the stream tests. */
#ifndef SNAPMAP_PLUS_STREAM_FIXTURE_H
#define SNAPMAP_PLUS_STREAM_FIXTURE_H

#include <stdint.h>
#include "overrides.h"

#define stream_close ((void (*)(void *))sh_overrides_test_stream_vtable_slot(0))
#define stream_true_flag ((long long (*)(void *))sh_overrides_test_stream_vtable_slot(2))
#define stream_read ((long long (*)(void *, void *, uint64_t))sh_overrides_test_stream_vtable_slot(6))
#define stream_write ((long long (*)(void *, const void *, uint64_t))sh_overrides_test_stream_vtable_slot(7))
#define stream_read_at ((long long (*)(void *, long long, void *, uint64_t))sh_overrides_test_stream_vtable_slot(8))
#define stream_write_at ((long long (*)(void *, long long, const void *, uint64_t))sh_overrides_test_stream_vtable_slot(9))
#define stream_length ((long long (*)(void *))sh_overrides_test_stream_vtable_slot(13))
#define stream_set_length ((int (*)(void *, long long))sh_overrides_test_stream_vtable_slot(14))
#define stream_seek ((int (*)(void *, long long, int))sh_overrides_test_stream_vtable_slot(16))

#endif
