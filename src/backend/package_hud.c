/* Typed package HUD settings shared by compilation and native publication. */
#include "package_hud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int sh_package_hud_name_valid(const char *name)
{
    if (!name || !*name) return 0;
    const char *part = name;
    for (const char *p = name;; p++) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
            if (!c) return 1;
            part = p + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.')) return 0;
    }
}

int sh_package_hud_mode(const char *settings, int *weapon_mode)
{
    sh_json_object row = {0};
    char mode[16]; size_t length = 0; int ok = 0;
    if (!settings || !weapon_mode || !sh_json_parse_object(settings, strlen(settings), 2, &row)) goto done;
    const char *raw = sh_json_object_get(&row, "ammo_display");
    if (row.count != 1 || !raw || !sh_json_decode_string(raw, strlen(raw), mode, sizeof(mode), &length) ||
        length != strlen(mode) || (strcmp(mode, "weapon") && strcmp(mode, "engine"))) goto done;
    *weapon_mode = !strcmp(mode, "weapon"); ok = 1;
done:
    sh_json_object_free(&row); return ok;
}

static int ph_rows(sh_json_object *out, const sh_json_object *source,
    char *error, size_t capacity, int *allocation)
{
    sh_json_object rows = {0}; int ok = 0;
    if (!source->count) return 1;
    const char *raw = sh_json_object_get(source, "weapons");
    if (source->count != 1 || !raw || !sh_json_parse_object(raw, strlen(raw), 3, &rows)) goto done;
    for (size_t i = 0; i < rows.count; i++) {
        const sh_json_member *row = rows.members + i;
        int mode = 0, previous_mode = 0;
        if (strlen(row->key) != row->key_length || !sh_package_hud_name_valid(row->key) ||
            !sh_package_hud_mode(row->value_json, &mode)) goto done;
        const char *previous = sh_json_object_get(out, row->key);
        if (previous && !sh_package_hud_mode(previous, &previous_mode)) goto done;
        if (!sh_json_object_set(out, row->key, mode || previous_mode ?
            "{\"ammo_display\":\"weapon\"}" : "{\"ammo_display\":\"engine\"}", 2)) {
            if (allocation) *allocation = 1;
            goto done;
        }
    }
    ok = 1;
done:
    sh_json_object_free(&rows);
    if (!ok && error && capacity) snprintf(error, capacity, "hud requires weapons.<weapon declaration>.ammo_display: weapon or engine");
    return ok;
}

int sh_package_hud_merge_provisional(sh_json_object *target, const sh_json_object *source,
    char *error, size_t capacity, int *allocation)
{
    sh_json_object rows = {0}, next = {0}; char *json = NULL; int ok = 0;
    if (allocation) *allocation = 0;
    if (!target || !source || !ph_rows(&rows, target, error, capacity, allocation) ||
        !ph_rows(&rows, source, error, capacity, allocation)) goto done;
    if (rows.count) {
        json = sh_json_serialize_object(&rows, 0, NULL);
        if (!json || !sh_json_object_set(&next, "weapons", json, 3)) {
            if (allocation) *allocation = 1;
            goto done;
        }
    }
    sh_json_object_free(target); *target = next; memset(&next, 0, sizeof(next)); ok = 1;
done:
    free(json); sh_json_object_free(&rows); sh_json_object_free(&next); return ok;
}

int sh_package_hud_bind(sh_package_policy *policy, const char *name, int weapon_mode,
    const sh_package_owners *owners)
{
    if (!policy || !sh_package_hud_name_valid(name) || (weapon_mode != 0 && weapon_mode != 1)) return 0;
    for (size_t i = 0; i < policy->hud_binding_count; i++) {
        sh_package_hud_binding *row = policy->hud_bindings + i;
        if (row->weapon_mode == weapon_mode && !strcmp(row->name, name))
            return !owners || sh_package_owners_union(&row->owners, owners);
    }
    sh_package_hud_binding row = {0}; row.name = _strdup(name); row.weapon_mode = weapon_mode;
    if (!row.name || (owners && !sh_package_owners_union(&row.owners, owners)) ||
        policy->hud_binding_count >= SIZE_MAX / sizeof(*policy->hud_bindings)) goto failed;
    sh_package_hud_binding *grown = realloc(policy->hud_bindings,
        (policy->hud_binding_count + 1) * sizeof(*grown));
    if (!grown) goto failed;
    policy->hud_bindings = grown; grown[policy->hud_binding_count++] = row; return 1;
failed:
    free(row.name); sh_package_owners_free(&row.owners); return 0;
}

int sh_package_hud_copy(sh_package_policy *out, const sh_package_policy *from,
    const sh_package_owners *selected, size_t owner_count, size_t offset)
{
    if (from->hud_bound) out->hud_bound = 1;
    for (size_t i = 0; i < from->hud_binding_count; i++) {
        const sh_package_hud_binding *row = from->hud_bindings + i;
        sh_package_owners owners = {0};
        if (!sh_package_owners_within(&row->owners, owner_count)) return 0;
        int ok = 1;
        for (size_t owner = 0; ok && owner < owner_count; owner++)
            if (sh_package_owners_contains(&row->owners, owner) &&
                (!selected || sh_package_owners_contains(selected, owner)))
                ok = owner <= SIZE_MAX - offset && sh_package_owners_add(&owners, owner + offset);
        if (ok && (!selected || sh_package_owners_count(&owners)))
            ok = sh_package_hud_bind(out, row->name, row->weapon_mode, &owners);
        sh_package_owners_free(&owners); if (!ok) return 0;
    }
    return 1;
}

int sh_package_hud_publish(sh_package_policy *policy, size_t map_owner_begin, int map_overlay)
{
    sh_json_object rows = {0}, next = {0}; char *json = NULL; int ok = 0;
    if (!policy->hud_bound) return 1;
    for (size_t i = 0; i < policy->hud_binding_count; i++) {
        const sh_package_hud_binding *row = policy->hud_bindings + i;
        int has_map = 0, mode = 0;
        if (sh_json_object_get(&rows, row->name)) continue;
        for (size_t j = i; j < policy->hud_binding_count; j++) {
            const sh_package_hud_binding *peer = policy->hud_bindings + j;
            if (strcmp(row->name, peer->name)) continue;
            int peer_map = map_overlay && !sh_package_owners_within(&peer->owners, map_owner_begin);
            if (peer_map && !has_map) { has_map = 1; mode = peer->weapon_mode; }
            else if (peer_map == has_map) mode |= peer->weapon_mode;
        }
        if (!sh_json_object_set(&rows, row->name, mode ?
            "{\"ammo_display\":\"weapon\"}" : "{\"ammo_display\":\"engine\"}", 2)) goto done;
    }
    if (rows.count) {
        json = sh_json_serialize_object(&rows, 0, NULL);
        if (!json || !sh_json_object_set(&next, "weapons", json, 3)) goto done;
    }
    sh_json_object_free(&policy->hud); policy->hud = next; memset(&next, 0, sizeof(next)); ok = 1;
done:
    free(json); sh_json_object_free(&rows); sh_json_object_free(&next); return ok;
}
