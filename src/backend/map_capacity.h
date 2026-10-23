/* The size of a map as DOOM stores it: the final serialized text after native
 * conversion, package embedding and navigation shards.
 *
 * One read-only engine value bounds that text in four places: the saved-map
 * builder (an immediate one below it), the cached saved/published map reader,
 * the publish upload and the upload that copies a published map into a
 * player's saved maps. The value is 10,485,760 on both supported images.
 * map_payload.c lifts only the builder and the reader, and only in this
 * process. A map above the bound therefore still saves and reloads here, but
 * DOOM refuses to publish it or to copy it from the service, and DOOM without
 * Snapmap+ refuses it as a cloud-saved or published map. The service's own
 * limits are not known.
 *
 * Measurement only: nothing here changes, truncates or refuses a map. */
#ifndef SNAPMAP_PLUS_MAP_CAPACITY_H
#define SNAPMAP_PLUS_MAP_CAPACITY_H

#include <stddef.h>

/* The engine value on both supported images, used until the running image's
 * own value is read (map_payload.c). */
#define SH_MAP_TEXT_LIMIT_DEFAULT 10485760u

typedef struct sh_map_capacity {
    size_t total;             /* serialized text bytes, exactly as stored */
    size_t limit;             /* the engine bound it was measured against */
    size_t package_bytes;     /* removed by stripping package delivery and binding shards */
    size_t reference_bytes;   /* then removed by stripping package reference shards (L2) */
    size_t navigation_bytes;  /* then removed by stripping navigation shards */
    unsigned package_shards;
    unsigned reference_shards;
    unsigned navigation_shards;
    int parts_known;          /* 0: the structure did not read; total stays exact */
} sh_map_capacity;

/* Record the bound read from the running image. Zero restores the default. */
void sh_map_capacity_set_limit(size_t limit);
size_t sh_map_capacity_limit(void);

/* Measure `text` (not NUL-terminated) against the current bound. Returns 0
 * only when `out` is NULL. */
int sh_map_capacity_measure(const char *text, size_t length, sh_map_capacity *out);

/* Within the bound: what DOOM publishes and copies, and what DOOM without
 * Snapmap+ reads as a cloud-saved or published map. */
int sh_map_capacity_within(const sh_map_capacity *capacity);

/* No package delivery remains, so nothing requires Snapmap+'s package serving.
 * Only a carrier-free map within the bound can reach DOOM without Snapmap+ as a
 * cloud-saved or published map; other product-only features can still apply. */
int sh_map_capacity_carrier_free(const sh_map_capacity *capacity);

/* The backend log line for a measurement. */
void sh_map_capacity_describe(const sh_map_capacity *capacity, char *line, size_t line_capacity);

#endif /* SNAPMAP_PLUS_MAP_CAPACITY_H */
