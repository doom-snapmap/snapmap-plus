/* JSON crash-record formatting, shared by the capture path and off-game tests. */
#ifndef SHIELD_CRASH_RECORD_FORMAT_H
#define SHIELD_CRASH_RECORD_FORMAT_H

#include <stddef.h>

typedef struct crash_record {
    /* "fatal" | "classB" | "engine_fatalerror" | "offthread". The frontend treats
     * classB/offthread as nonterminal notices; a record alone does not prove survival.
     * Update crash_record_is_terminal when adding a kind. */
    const char *kind;
    unsigned long code;       /* exception code (0 if not a hardware fault) */
    unsigned long long rip_rva;    /* faulting rip - module base (0 if unknown) */
    unsigned long long fault_addr; /* faulting data address (0 if none) */
    const char *module;       /* faulting module basename, e.g. "DOOMx64vk.exe" */
    const char *stack;        /* multi-line "MOD+0xRVA" walk, or "" */
    const char *engine_text;  /* the engine's own formatted error text, or "" */
    const char *dump;         /* crash-dump path if one was written, or "" */
    const char *version;      /* installed version string AT FAULT TIME (read at arm), or "" */
    /* Renderer at fault time: "vulkan", "opengl", or "" if unresolved. Preserve this
     * snapshot when reporting after a restart into the other renderer. */
    const char *renderer;
    const char *time;         /* preformatted local "YYYY-MM-DD HH:MM:SS" */
} crash_record;

/* Escape `src` as a JSON string body (no surrounding quotes) into dst. Handles \\ \" and control
 * chars (\n \r \t and \u00XX for the rest). Always NUL-terminates; truncates at cap. Returns the
 * number of chars written (excluding the NUL). Pure + deterministic. */
int crash_json_escape(char *dst, size_t cap, const char *src);

/* Return the complete JSON byte count, or zero with an empty buffer on failure.
 * Reentrant, allocation-free, and never truncates a field. Invalid UTF-8 becomes
 * a replacement character. All scratch storage belongs to the caller. */
int crash_record_json(char *buf, size_t cap, const crash_record *r);

#endif /* SHIELD_CRASH_RECORD_FORMAT_H */
