/* modpub_sites.c -- the bind-time byte checks of the published-map gates (modpublished.c) and the publish hooks
 * (modpublish.c), read in Ghidra on both build 13954591 executables. Their own file, with only the C library, so the
 * image-backed tests/modpub_sites_test.c runs the very functions the two binds run against each shipped executable
 * (R-LB1: a bind check no test ran against the executables turned a feature off in every session). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "modpublished.h"
#include "modpublish.h"

/* A direct call's target (E8), else NULL. */
static void *eng_call(const uint8_t *p)
{
    int32_t displacement;
    if (p[0] != 0xe8) return NULL;
    memcpy(&displacement, p + 1, 4);
    return (void *)(p + 5 + displacement);
}

/* A direct call's or jump's target (E8 or E9), else NULL: the publish hooks accept either. */
static void *eng_call_or_jump(const uint8_t *p)
{
    int32_t displacement;
    if (p[0] != 0xe8 && p[0] != 0xe9) return NULL;
    memcpy(&displacement, p + 1, 4);
    return (void *)(p + 5 + displacement);
}

/* The target of a RIP-relative operand whose disp32 follows `opcode`. */
static void *eng_rip(const uint8_t *p, const char *opcode, size_t opcode_length)
{
    int32_t displacement;
    if (memcmp(p, opcode, opcode_length)) return NULL;
    memcpy(&displacement, p + opcode_length, 4);
    return (void *)(p + opcode_length + 4 + displacement);
}

/* The slot index of `call qword ptr [reg+disp]` (optional REX.B, FF /2). */
static size_t eng_vcall_slot(const uint8_t *p)
{
    int32_t displacement;
    uint8_t modrm;
    if (p[0] == 0x41) p++;
    if (p[0] != 0xff) return 0;
    modrm = p[1];
    if (((modrm >> 3) & 7) != 2 || (modrm & 7) == 4) return 0;
    if ((modrm >> 6) == 1) displacement = (int8_t)p[2];
    else if ((modrm >> 6) == 2) memcpy(&displacement, p + 2, 4);
    else return 0;
    if (displacement <= 0 || displacement >= 0x800 || displacement % 8) return 0;
    return (size_t)displacement / sizeof(void *);
}

/* ------------------------------------------------------------ the published-map gates (modpublished.c eng_bind) */

const char *sh_modpub_check_sites(const sh_modpub_sites *s, sh_modpub_bound *out)
{
    sh_modpub_bound b;
    const uint8_t *fail;
    memset(&b, 0, sizeof b);
    if (out) memset(out, 0, sizeof *out);
    if (!s || !s->on_snap || !s->browser || !s->ready || !s->lobby || !s->feed || !s->lobby_launch ||
        !s->offline_launch || !s->read || !s->limit || !s->selection || !s->parse || !s->str_ctor || !s->str_dtor ||
        !s->shell_slot) return "a site is missing";
    /* The two read sites call the parser; their return addresses classify. */
    if (eng_call(s->read + 0x125) != (void *)s->parse) return "map read +0x125: the parse call";
    if (eng_call(s->limit + 0x58) != (void *)s->parse) return "size-limited read +0x58: the parse call";
    /* The selection record allocates 0x590 and copy-constructs the metadata. */
    if (memcmp(s->selection + 0x45, "\xb9\x90\x05\x00\x00", 5)) return "selection +0x45: the record size";
    if (!(b.metadata_copy = eng_call(s->selection + 0x69))) return "selection +0x69: the metadata copy";
    /* OnSnapPlay: the cookie load relocated at +0xB, the dialog block's constant head, the shell slot, the source
     * label, ClearDialog at vtable +0x220 and the progress widget. */
    if (memcmp(s->on_snap + 0x0B, "\x48\x8b\x05", 3)) return "OnSnapPlay +0x0B: the cookie load";
    if (!(b.snap_params = eng_rip(s->on_snap + 0x1D, "\x66\x0f\x6f\x05", 4))) return "OnSnapPlay +0x1D: the dialog block";
    if (eng_rip(s->on_snap + 0x4E, "\x48\x8b\x0d", 3) != s->shell_slot) return "OnSnapPlay +0x4E: the shell global";
    if (!(b.snap_source = eng_rip(s->on_snap + 0x55, "\x48\x8d\x05", 3))) return "OnSnapPlay +0x55: the source label";
    if (memcmp(s->on_snap + 0x69, "\xc7\x84\x24\xc0\x00\x00\x00\x19\x19\x00\x00", 11)) return "OnSnapPlay +0x69: the dialog id";
    if (memcmp(s->on_snap + 0x8A, "\xff\x90\x20\x02\x00\x00", 6)) return "OnSnapPlay +0x8A: ClearDialog";
    if (memcmp(s->on_snap + 0x90, "\x48\x8b\x8b\x38\x0a\x00\x00", 7) || memcmp(s->on_snap + 0xA1, "\xff\x50\x30", 3))
        return "OnSnapPlay +0x90: the progress widget";
    /* Browser play: the manager flag, the browser state and widget, and the manager's progress hide (also used for
     * the other manager hooks). */
    if (!(b.browser_flag = eng_call(s->browser + 0x21))) return "browser play +0x21: the manager flag";
    if (memcmp(s->browser + 0x26, "\x48\x8b\x4b\x30\xc7\x43\x20\x00\x00\x00\x00\x48\x85\xc9\x74\x0b"
                                  "\x48\x8b\x01\xba\x0f\x00\x00\x00\xff\x50\x08\x48\x8b\x4b\x10", 31))
        return "browser play +0x26: the browser state";
    if (!(b.hide_progress = eng_call(s->browser + 0x45))) return "browser play +0x45: the progress hide";
    /* The launch boundary is what browser play and feed play call: the lobby launch at browser+0x9C and feed+0xE3,
     * the offline launch at browser+0xAD. */
    if (eng_call(s->browser + 0x9C) != (void *)s->lobby_launch || eng_call(s->feed + 0xE3) != (void *)s->lobby_launch)
        return "browser +0x9C / feed +0xE3: the lobby launch";
    if (eng_call(s->browser + 0xAD) != (void *)s->offline_launch) return "browser +0xAD: the offline launch";
    /* CacheReady: progress, then session -> lobby -> parameters. */
    if (memcmp(s->ready + 0x38, "\x48\x8b\x89\x38\x0a\x00\x00", 7) || memcmp(s->ready + 0x49, "\xff\x50\x30", 3))
        return "CacheReady +0x38: the progress widget";
    if (!(b.session_slot = eng_rip(s->ready + 0x4C, "\x48\x8b\x0d", 3))) return "CacheReady +0x4C: the session global";
    if (memcmp(s->ready + 0x53, "\x48\x8b\x01", 3) || !(b.slot_lobby = eng_vcall_slot(s->ready + 0x56)))
        return "CacheReady +0x56: the session's lobby";
    if (memcmp(s->ready + 0x5C, "\x48\x8b\x10\x48\x8b\xc8", 6) || !(b.slot_parameters = eng_vcall_slot(s->ready + 0x62)))
        return "CacheReady +0x62: the lobby's parameters";
    /* CacheMapForLobby: the warm query, then the cold branch it replays. */
    if (!(b.system = eng_call(s->lobby + 0x34))) return "lobby cache +0x34: the map system";
    if (memcmp(s->lobby + 0x39, "\x48\x8b\x10\x48\x8b\xc8\xff\x52\x38", 9) ||
        memcmp(s->lobby + 0x42, "\x4c\x8b\x08\x4c\x8b\xc5\x48\x8b\xd6\x48\x8b\xc8\x41\xff\x91\xb0\x01\x00\x00", 19))
        return "lobby cache +0x39: the warm query";
    if (!(b.wait_text = eng_rip(s->lobby + 0x60, "\x48\x8d\x15", 3)) || eng_call(s->lobby + 0x6C) != (void *)s->str_ctor ||
        !(b.show_wait = eng_call(s->lobby + 0x7A)) || eng_call(s->lobby + 0x85) != (void *)s->str_dtor)
        return "lobby cache +0x60: the wait text";
    if (memcmp(s->lobby + 0x8A, "\xb9\x18\x00\x00\x00", 5) || !(b.op_new = eng_call(s->lobby + 0x8F)) ||
        eng_call(s->lobby + 0xC3) != b.op_new)
        return "lobby cache +0x8A: the request allocations";
    if (!(b.vft_ok = eng_rip(s->lobby + 0xA1, "\x48\x8d\x05", 3)) ||
        eng_rip(s->lobby + 0xAF, "\x48\x8d\x05", 3) != (void *)s->ready ||
        !(b.vft_fail = eng_rip(s->lobby + 0xD2, "\x48\x8d\x0d", 3)) ||
        !(fail = (const uint8_t *)eng_rip(s->lobby + 0xE0, "\x48\x8d\x0d", 3)))
        return "lobby cache +0xA1: the request callbacks";
    if (eng_call(s->lobby + 0xFD) != (void *)s->read) return "lobby cache +0xFD: the map read";
    b.fail_fn = fail;
    /* The read-error handler (VK 0x140FA1E80, GL 0x140FA13B0; same bytes on both): on a 404 it raises dialog 0xA2,
     * then +0x90 loads the session global CacheReady reads, +0x97 its vtable and +0x9A calls slot 0x2F0, which
     * leaves the lobby. The leave is used alone (no dialog), and only when every byte of that sequence holds. */
    if (!memcmp(fail, "\x41\x81\xf8\x94\x01\x00\x00", 7) &&
        eng_rip(fail + 0x90, "\x48\x8b\x0d", 3) == b.session_slot &&
        !memcmp(fail + 0x97, "\x48\x8b\x01", 3) && !memcmp(fail + 0x9A, "\xff\x90", 2))
        b.slot_leave = eng_vcall_slot(fail + 0x9A);
    if (out) *out = b;
    return NULL;
}

/* ------------------------------------------------------------ the publish hooks (modpublish.c eng_bind) */

const char *sh_modpublish_check_sites(const sh_modpublish_sites *s, sh_modpublish_bound *out)
{
    sh_modpublish_bound b;
    const uint8_t *on_error;
    memset(&b, 0, sizeof b);
    if (out) memset(out, 0, sizeof *out);
    if (!s || !s->fail || !s->copy || !s->browser || !s->start || !s->after_save || !s->str_ctor || !s->str_dtor)
        return "a site is missing";
    /* PublishFail(editorLocal): the toast on editor+0x21088 with two idStr locals, then EditorBusyClear(editor). */
    if (memcmp(s->fail + 0x27, "\x48\x83\xb9\x88\x10\x02\x00\x00\x74\x50", 10)) return "PublishFail +0x27: the editor screen";
    if (eng_call_or_jump(s->fail + 0x3D) != (void *)s->str_ctor || eng_call_or_jump(s->fail + 0x4F) != (void *)s->str_ctor)
        return "PublishFail +0x3D: the two idStr locals";
    if (memcmp(s->fail + 0x54, "\x90\x4c\x8d\x44\x24\x58\x48\x8d\x54\x24\x28\x48\x8b\x8b\x88\x10\x02\x00", 18) ||
        !(b.toast = eng_call_or_jump(s->fail + 0x66)))
        return "PublishFail +0x54: the toast";
    if (eng_call_or_jump(s->fail + 0x71) != (void *)s->str_dtor || memcmp(s->fail + 0x81, "\x48\x8b\xcb\xe8", 4))
        return "PublishFail +0x71: the locals' destruction";
    /* MapInfo copy: shell (+0x18, else from +0x20), manager, the record by index +0x300 of kind 1, and the model at
     * record+0x18 -> +8. */
    if (memcmp(s->copy + 0x2D, "\x48\x8b\xd9\x48\x8b\x41\x18\x48\x85\xc0\x75\x09\x48\x8b\x49\x20", 16) ||
        !(b.screen_shell = eng_call_or_jump(s->copy + 0x3D)))
        return "MapInfo copy +0x2D: the screen's shell";
    if (memcmp(s->copy + 0x42, "\x48\x8b\xc8", 3) || !(b.shell_manager = eng_call_or_jump(s->copy + 0x45)))
        return "MapInfo copy +0x42: the shell's manager";
    if (memcmp(s->copy + 0x56, "\x8b\x93\x00\x03\x00\x00\x48\x8b\xc8", 9) || !(b.record = eng_call_or_jump(s->copy + 0x5F)))
        return "MapInfo copy +0x56: the history record";
    if (memcmp(s->copy + 0x70, "\x83\x78\x0c\x01", 4) || memcmp(s->copy + 0x12C, "\x48\x8b\x56\x18\x48\x83\xc2\x08", 8))
        return "MapInfo copy +0x70: the record's model";
    /* Browser copy: the same record lookup on (+0x10, +8) of kind 5, the entry lookup on (+0x38, +0), the model at
     * entry+0x28 -> +8, and the cancelled-error path of its OnBranchError (flag 0). */
    if (memcmp(s->browser + 0x10, "\x8b\x51\x08\x48\x8b\x49\x10", 7) || eng_call_or_jump(s->browser + 0x17) != b.record ||
        memcmp(s->browser + 0x25, "\x83\x78\x0c\x05", 4))
        return "browser copy +0x10: the history record";
    if (memcmp(s->browser + 0x2F, "\x8b\x17\x48\x8d\x4f\x38", 6) || !(b.entry = eng_call_or_jump(s->browser + 0x43)) ||
        memcmp(s->browser + 0x9B, "\x48\x8b\x56\x28", 4))
        return "browser copy +0x2F: the entry and its model";
    if (!(on_error = (const uint8_t *)eng_rip(s->browser + 0x7E, "\x48\x8d\x15", 3))) return "browser copy +0x7E: its error path";
    if (memcmp(on_error + 0x2B, "\x48\x8b\x4b\x30\x33\xf6\x89\x73\x20\x48\x85\xc9\x74\x08\x48\x8b\x01\x33\xd2"
                                "\xff\x50\x08\xba\x0f\x00\x00\x00\x48\x8b\xcb", 30) ||
        !(b.browser_state = eng_call_or_jump(on_error + 0x49)))
        return "browser copy error path +0x2B: the browser state";
    if (memcmp(on_error + 0x4E, "\x48\x8b\x4b\x10\x48\x83\xc4\x40\x5f\x5e\x5b", 11) ||
        !(b.hide_progress = eng_call_or_jump(on_error + 0x59)))
        return "browser copy error path +0x4E: the progress hide";
    /* PublishStart's map:dev append: IdStrCtor at +0x55, the tag list at editor->edit + 0x998, grow when num == size
     * (+0xB3), assign into list[num * 0x30] (+0xD5), num++; its Save callback is PublishAfterSave. */
    if (eng_call_or_jump(s->start + 0x55) != (void *)s->str_ctor) return "PublishStart +0x55: the tag's idStr";
    if (memcmp(s->start + 0x5B, "\x49\x8b\xb6\xc8\x04\x02\x00\x48\x81\xc6\x98\x09\x00\x00", 14))
        return "PublishStart +0x5B: the edit's tag list";
    if (memcmp(s->start + 0xA8, "\x8b\x46\x0c\x39\x46\x08\x75\x0c\x48\x8b\xce\xe8", 12) ||
        !(b.tags_grow = eng_call_or_jump(s->start + 0xB3)))
        return "PublishStart +0xA8: the list's grow";
    if (memcmp(s->start + 0xB8, "\x84\xc0\x74\x21\x48\x63\x46\x08\x3b\x46\x0c\x7d\x18\x48\x8d\x0c\x40\x48\xc1\xe1\x04"
                                "\x48\x03\x0e\x48\x8d\x54\x24\x28\xe8", 30) ||
        !(b.idstr_assign = eng_call_or_jump(s->start + 0xD5)) || memcmp(s->start + 0xDA, "\xff\x46\x08", 3))
        return "PublishStart +0xB8: the tag's append";
    if (eng_rip(s->start + 0x1F5, "\x4c\x8d\x05", 3) != (void *)s->after_save) return "PublishStart +0x1F5: its Save callback";
    if (eng_call_or_jump(s->start + 0x140) != (void *)s->str_dtor) return "PublishStart +0x140: the tag's destruction";
    if (out) *out = b;
    return NULL;
}
