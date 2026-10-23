/* Offline data-global resolution and layout checks on an RVA-mapped PE.
 * Run both supported renderers: pinned mode checks Vulkan RVAs; portable mode
 * checks resolution and relative layout without fixed addresses.
 *
 *   globals_test <unpacked.exe>
 *   globals_test <unpacked.exe> portable */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_class_registry.h"
#include "engine_globals.h"
#include "decl_native_schema.h"
#include "map_source.h"
#include "map_native.h"
#include "map_published.h"
#include "map_transition.h"
#include "editor_transition.h"
#include "resource_resident.h"
#include "swf_text_font.h"
#include "image_exclusion.h"
#include "virtual_texture_provider.h"
#include "audio_bank_commands.h"
#include "audio_limits_native.h"
#include "audio_environment_native.h"
#include "audio_state_native.h"
#include "audio_parameter_native.h"
#include "audio_switch_native.h"
#include "audio_bus_native.h"
#include "audio_bank_gate.h"
#include "resource_file_visibility.h"

/* Binding checks decode resource-file visibility but never serve through it. */
int sh_overrides_claim_open(const char *name, void **stream)
{
    (void)name; (void)stream;
    fputs("unexpected shadow claim during binding checks\n", stderr);
    abort();
}
void sh_overrides_pending_open(const char *name, void *stream) { (void)name; (void)stream; abort(); }
void *sh_overrides_pending_take(void) { abort(); }
void sh_overrides_stream_close(void *stream) { (void)stream; abort(); }

/* Binding checks must never invoke live reflection on the mapped PE. */
int sh_typeinfo_class_derives(const char *name, const char *base)
{
    (void)name; (void)base;
    fputs("unexpected live reflection during binding checks\n", stderr);
    abort();
}

typedef struct metadata_image { const uint8_t *base; size_t size; uintptr_t last; } metadata_image;
static int metadata_read(void *context, uintptr_t address, void *out, size_t length)
{
    metadata_image *image = (metadata_image *)context;
    uintptr_t base = (uintptr_t)image->base;
    image->last = address;
    if (address < base || address - base > image->size || length > image->size - (address - base)) return 0;
    memcpy(out, (const void *)address, length); return 1;
}

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    uint32_t hdr_size   = nt->OptionalHeader.SizeOfHeaders;

    uint8_t *img = (uint8_t *)calloc(1, image_size);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, hdr_size);

    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t rs = sec[i].SizeOfRawData;
        uint32_t po = sec[i].PointerToRawData;
        if (rs && va + rs <= image_size && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    free(file);
    *image_sz = image_size;
    return img;
}

static uint32_t rva_of(const uint8_t *base, const char *name, int *bad)
{
    glb_status st = GLB_UNKNOWN_NAME;
    uintptr_t a = glb_resolve(base, name, &st);
    if (!a) {
        printf("BAD %-24s UNRESOLVED status=%d\n", name, (int)st);
        (*bad)++;
        return 0;
    }
    return (uint32_t)(a - (uintptr_t)base);
}

static uintptr_t image_export(const uint8_t *base, size_t size, const char *name)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const IMAGE_EXPORT_DIRECTORY *exports;
    const DWORD *names, *functions;
    const WORD *ordinals;
    DWORD i;
    if (!directory.VirtualAddress || directory.VirtualAddress > size ||
        sizeof(*exports) > size - directory.VirtualAddress) return 0;
    exports = (const IMAGE_EXPORT_DIRECTORY *)(base + directory.VirtualAddress);
    if (exports->AddressOfNames > size || exports->NumberOfNames > (size - exports->AddressOfNames) / sizeof(DWORD) ||
        exports->AddressOfNameOrdinals > size || exports->NumberOfNames > (size - exports->AddressOfNameOrdinals) / sizeof(WORD) ||
        exports->AddressOfFunctions > size || exports->NumberOfFunctions > (size - exports->AddressOfFunctions) / sizeof(DWORD)) return 0;
    names = (const DWORD *)(base + exports->AddressOfNames);
    ordinals = (const WORD *)(base + exports->AddressOfNameOrdinals);
    functions = (const DWORD *)(base + exports->AddressOfFunctions);
    for (i = 0; i < exports->NumberOfNames; i++) {
        size_t length = strlen(name);
        if (names[i] >= size || length >= size - names[i] || memcmp(base + names[i], name, length + 1)) continue;
        if (ordinals[i] >= exports->NumberOfFunctions || functions[ordinals[i]] >= size) return 0;
        return (uintptr_t)base + functions[ordinals[i]];
    }
    return 0;
}

static void check_adjacent(const uint8_t *base, const char *lo, const char *hi,
                           uint32_t gap, int *bad)
{
    int local = 0;
    uint32_t a = rva_of(base, lo, &local);
    uint32_t b = rva_of(base, hi, &local);
    if (local) { *bad += local; return; }
    if (b - a != gap) {
        printf("BAD invariant %s + 0x%x != %s (0x%x + 0x%x != 0x%x)\n",
               lo, gap, hi, a, gap, b);
        (*bad)++;
    } else {
        printf("OK  invariant %s + 0x%-4x == %s\n", lo, gap, hi);
    }
}

int main(int argc, char **argv)
{
    int pinned = 1;
    if (argc < 2) {
        fprintf(stderr, "usage: globals_test <DOOM_unpacked.exe> [portable]\n");
        return 2;
    }
    if (argc >= 3 && strcmp(argv[2], "portable") == 0) pinned = 0;

    size_t image_sz = 0;
    uint8_t *base = map_pe_by_rva(argv[1], &image_sz);
    if (!base) return 2;

    size_t total = glb_db_count();
    size_t ok = 0;
    int bad = 0;

    {
        sig_result results[3] = {0};
        sh_audio_bank_commands commands = {0};
        sh_audio_registry registry = {0};
        const char *names[] = {"AudioBankLoad", "AudioEventPost", "AudioBankPrepare"};
        int resolved = 1;
        for (size_t n = 0; n < 3; n++) {
            const sig_entry *entry = NULL;
            for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name; ++i)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, names[n])) entry = BACKEND_ENGINE_SIGNATURES+i;
            if (!entry || sig_resolve_one(base, entry, results+n) != SIG_OK) resolved = 0;
        }
        if (!resolved || !sh_audio_bank_commands_bind(&commands, base, results, 3)) {
            puts("BAD numeric audio bank command binding"); ++bad;
        } else puts("OK  numeric audio bank command binding");
        uintptr_t index = pinned ? 0x70d4020u : 0x59df770u;
        uintptr_t lock = pinned ? 0x70d4818u : 0x59dff68u;
        if (!resolved || !sh_audio_registry_bind(&registry, base, results, 3) ||
            (uintptr_t)registry.index_slot != (uintptr_t)base+index ||
            (uintptr_t)registry.bank_lock != (uintptr_t)base+lock || registry.manager_slot != commands.manager_slot) {
            puts("BAD read-only audio registry binding"); ++bad;
        } else puts("OK  read-only audio registry binding");
    }

    {
        const char *names[]={"AudioThresholdSet","AudioVoiceLimitSet","AudioRenderTick","AudioBankLoad"};
        sig_result results[4]={0};sh_audio_limits_native limits={0};int resolved=1;
        for(size_t n=0;n<4;n++) {
            const sig_entry *entry=NULL;
            for(size_t i=0;BACKEND_ENGINE_SIGNATURES[i].name;i++)
                if(!strcmp(BACKEND_ENGINE_SIGNATURES[i].name,names[n]))entry=BACKEND_ENGINE_SIGNATURES+i;
            if(!entry || sig_resolve_one(base,entry,results+n)!=SIG_OK)resolved=0;
        }
        uintptr_t threshold=pinned ? 0x55bd9d4u : 0x3ec0044u;
        uintptr_t priority=pinned ? 0x55bd8a0u : 0x3ebff10u;
        uintptr_t voices=pinned ? 0x70d4200u : 0x59df950u;
        uintptr_t lock=pinned ? 0x70d40a0u : 0x59df7f0u;
        uintptr_t manager=pinned ? 0x70d4030u : 0x59df780u;
        if(!resolved || !sh_audio_limits_native_bind(&limits,base,results,4) ||
            (uintptr_t)limits.threshold!=(uintptr_t)base+threshold ||
            (uintptr_t)limits.amplitude!=(uintptr_t)base+threshold-4 ||
            (uintptr_t)limits.threshold_priority!=(uintptr_t)base+priority ||
            (uintptr_t)limits.voices_priority!=(uintptr_t)base+priority+4 ||
            (uintptr_t)limits.voices!=(uintptr_t)base+voices ||
            (uintptr_t)limits.render_lock!=(uintptr_t)base+lock ||
            (uintptr_t)limits.manager_slot!=(uintptr_t)base+manager) {
            puts("BAD native playback settings binding");bad++;
        } else puts("OK  native playback settings binding");
        const char *environment_names[]={"AudioEnvironmentRead","AudioEnvironmentSet"};
        sig_result environment_results[2]={0};sh_audio_environment_native environment={0};
        for(size_t n=0;n<2;n++) {
            const sig_entry *entry=NULL;
            for(size_t i=0;BACKEND_ENGINE_SIGNATURES[i].name;i++)
                if(!strcmp(BACKEND_ENGINE_SIGNATURES[i].name,environment_names[n]))entry=BACKEND_ENGINE_SIGNATURES+i;
            if(!entry || sig_resolve_one(base,entry,environment_results+n)!=SIG_OK)resolved=0;
        }
        uintptr_t environment_slot=pinned ? 0x70d3fc0u : 0x59df710u;
        uintptr_t curve_set=pinned ? 0x1cedfb0u : 0x1d07590u;
        if(!resolved || !sh_audio_environment_native_bind(&environment,base,environment_results,2,&limits) ||
            (uintptr_t)environment.environment_slot!=(uintptr_t)base+environment_slot ||
            (uintptr_t)environment.set_curve!=(uintptr_t)base+curve_set ||
            environment.render_lock!=limits.render_lock || environment.manager_slot!=limits.manager_slot) {
            puts("BAD native environment settings binding");bad++;
        } else puts("OK  native environment settings binding");
        const char *state_names[]={"AudioStateSettingsRead","AudioStateGroupEnsure"};
        sig_result state_results[2]={0};sh_audio_state_native state={0};
        for(size_t n=0;n<2;n++) {
            const sig_entry *entry=NULL;
            for(size_t i=0;BACKEND_ENGINE_SIGNATURES[i].name;i++)
                if(!strcmp(BACKEND_ENGINE_SIGNATURES[i].name,state_names[n]))entry=BACKEND_ENGINE_SIGNATURES+i;
            if(!entry || sig_resolve_one(base,entry,state_results+n)!=SIG_OK)resolved=0;
        }
        if(!resolved || !sh_audio_state_native_bind(&state,base,state_results,2,&limits) ||
            (uintptr_t)state.state_slot!=(uintptr_t)base+(pinned ? 0x70d3fa0u : 0x59df6f0u) ||
            (uintptr_t)state.ensure!=(uintptr_t)base+(pinned ? 0x1d28900u : 0x1d41da0u) ||
            (uintptr_t)state.destroy!=(uintptr_t)base+(pinned ? 0x1d297d0u : 0x1d42c70u) ||
            (uintptr_t)state.allocate!=(uintptr_t)base+(pinned ? 0x1ceb5b0u : 0x1d04b90u) ||
            (uintptr_t)state.release!=(uintptr_t)base+(pinned ? 0x1ceb2e0u : 0x1d048c0u) ||
            (uintptr_t)state.pool!=(uintptr_t)base+(pinned ? 0x55bd8a8u : 0x3ebff18u) ||
            state.render_lock!=limits.render_lock || state.manager_slot!=limits.manager_slot) {
            puts("BAD native state settings binding");bad++;
        } else puts("OK  native state settings binding");
        const char *parameter_names[]={"AudioStateSettingsRead","AudioParameterEnsure","AudioParameterDestroy"};
        sig_result parameter_results[3]={0};sh_audio_parameter_native parameter={0};
        for(size_t n=0;n<3;n++) {
            const sig_entry *entry=NULL;
            for(size_t i=0;BACKEND_ENGINE_SIGNATURES[i].name;i++)
                if(!strcmp(BACKEND_ENGINE_SIGNATURES[i].name,parameter_names[n]))entry=BACKEND_ENGINE_SIGNATURES+i;
            if(!entry || sig_resolve_one(base,entry,parameter_results+n)!=SIG_OK)resolved=0;
        }
        if(!resolved || !sh_audio_parameter_native_bind(&parameter,base,parameter_results,3,&limits) ||
            (uintptr_t)parameter.parameter_slot!=(uintptr_t)base+(pinned ? 0x70d3fe0u : 0x59df730u) ||
            (uintptr_t)parameter.ensure!=(uintptr_t)base+(pinned ? 0x1d1a340u : 0x1d33720u) ||
            (uintptr_t)parameter.destroy!=(uintptr_t)base+(pinned ? 0x1d18b10u : 0x1d31fa0u) ||
            parameter.allocate!=state.allocate || parameter.release!=state.release || parameter.pool!=state.pool ||
            parameter.render_lock!=limits.render_lock || parameter.manager_slot!=limits.manager_slot) {
            puts("BAD native parameter settings binding");bad++;
        } else puts("OK  native parameter settings binding");
        /* Switch-group construction and the common subscription core are
         * reached from the STMG reader through identical call sites. */
        sh_audio_subscription_native subscription={0};sh_audio_switch_native switches={0};
        if(!resolved || !sh_audio_subscription_native_bind(&subscription,base,parameter_results,3,&parameter,&environment) ||
            (uintptr_t)subscription.switch_slot!=(uintptr_t)base+(pinned ? 0x70d3fd8u : 0x59df728u) ||
            (uintptr_t)subscription.modulator_slot!=(uintptr_t)base+(pinned ? 0x70d3fd0u : 0x59df720u) ||
            (uintptr_t)subscription.switch_ensure!=(uintptr_t)base+(pinned ? 0x1d3e580u : 0x1d57a40u) ||
            (uintptr_t)subscription.reverse_remove!=(uintptr_t)base+(pinned ? 0x1d1c580u : 0x1d35960u) ||
            (uintptr_t)subscription.modulator_remove!=(uintptr_t)base+(pinned ? 0x1d45d00u : 0x1d5f0a0u) ||
            (uintptr_t)subscription.modulator_add!=(uintptr_t)base+(pinned ? 0x1d456a0u : 0x1d5ea40u) ||
            (uintptr_t)subscription.reverse_add!=(uintptr_t)base+(pinned ? 0x1d19260u : 0x1d32640u) ||
            (uintptr_t)subscription.refresh!=(uintptr_t)base+(pinned ? 0x1d1ea60u : 0x1d37e40u) ||
            subscription.parameters!=&parameter || subscription.set_curve!=environment.set_curve ||
            !sh_audio_switch_native_bind(&switches,&subscription,&limits) || switches.render_lock!=limits.render_lock ||
            switches.manager_slot!=limits.manager_slot) {
            puts("BAD native audio subscription binding");bad++;
        } else puts("OK  native audio subscription binding");
        /* Bus construction, cooked-reader setters, state helpers and globals
         * derive from the auxiliary constructor and the bus vtables. */
        const char *bus_names[]={"AudioAuxBusCreate","AudioBankLoad","AudioEventPost","AudioBankPrepare"};
        sig_result bus_results[4]={0};sh_audio_registry bus_registry={0};sh_audio_bus_native buses={0};
        for(size_t n=0;n<4;n++) {
            const sig_entry *entry=NULL;
            for(size_t i=0;BACKEND_ENGINE_SIGNATURES[i].name;i++)
                if(!strcmp(BACKEND_ENGINE_SIGNATURES[i].name,bus_names[n]))entry=BACKEND_ENGINE_SIGNATURES+i;
            if(!entry || sig_resolve_one(base,entry,bus_results+n)!=SIG_OK)resolved=0;
        }
        const uintptr_t b=(uintptr_t)base;
        if(!resolved || !sh_audio_registry_bind(&bus_registry,base,bus_results+1,3) ||
            !sh_audio_bus_native_bind(&buses,base,bus_results,1,&subscription,&limits,&bus_registry,&state) ||
            (uintptr_t)buses.create_aux!=b+(pinned ? 0x1d6b8e0u : 0x1d84e10u) ||
            (uintptr_t)buses.construct!=b+(pinned ? 0x1d40390u : 0x1d59760u) ||
            (uintptr_t)buses.initialize!=b+(pinned ? 0x1d42760u : 0x1d5b9e0u) ||
            (uintptr_t)buses.normal_vtable!=b+(pinned ? 0x281c518u : 0x2848568u) ||
            (uintptr_t)buses.aux_vtable!=b+(pinned ? 0x281dd30u : 0x2849de0u) ||
            (uintptr_t)buses.lookup!=b+(pinned ? 0x1d2c540u : 0x1d415d0u) ||
            (uintptr_t)buses.roots[0]!=b+(pinned ? 0x70d4258u : 0x59df9a8u) ||
            (uintptr_t)buses.roots[1]!=b+(pinned ? 0x70d4260u : 0x59df9b0u) ||
            (uintptr_t)buses.sample_rate!=b+(pinned ? 0x55bd8b8u : 0x3ebff28u) ||
            (uintptr_t)buses.duck_floor!=b+(pinned ? 0x55bd8c4u : 0x3ebff34u) ||
            (uintptr_t)buses.property_parameters!=b+(pinned ? 0x281ba80u : 0x2847ac0u) ||
            (uintptr_t)buses.set_effect!=b+(pinned ? 0x1d355c0u : 0x1d4eaa0u) ||
            (uintptr_t)buses.set_bypass!=b+(pinned ? 0x1d34510u : 0x1d4d9e0u) ||
            (uintptr_t)buses.set_limit_kill!=b+(pinned ? 0x1d35840u : 0x1d4ed20u) ||
            (uintptr_t)buses.set_limit_virtual!=b+(pinned ? 0x1d35b50u : 0x1d4f030u) ||
            (uintptr_t)buses.set_channels!=b+(pinned ? 0x1d40dc0u : 0x1d5a0e0u) ||
            (uintptr_t)buses.add_background!=b+(pinned ? 0x1d439b0u : 0x1d5cc30u) ||
            (uintptr_t)buses.remove_background!=b+(pinned ? 0x1d44420u : 0x1d5d6a0u) ||
            (uintptr_t)buses.set_duck!=b+(pinned ? 0x1d40ad0u : 0x1d59ea0u) ||
            (uintptr_t)buses.rtpc_ready!=b+(pinned ? 0x1d3c0f0u : 0x1d55610u) ||
            (uintptr_t)buses.rtpc_mark!=b+(pinned ? 0x1d3c280u : 0x1d557a0u) ||
            (uintptr_t)buses.state_group!=b+(pinned ? 0x1d32150u : 0x1d4b620u) ||
            (uintptr_t)buses.state_bind!=b+(pinned ? 0x1d31fb0u : 0x1d4b480u) ||
            (uintptr_t)buses.state_unbind!=b+(pinned ? 0x1d35050u : 0x1d4e530u) ||
            (uintptr_t)buses.state_unregister!=b+(pinned ? 0x1d29230u : 0x1d426d0u) ||
            (uintptr_t)buses.transition_unregister!=b+(pinned ? 0x1d280f0u : 0x1d413b0u) ||
            (uintptr_t)buses.transition_slot!=b+(pinned ? 0x70d4028u : 0x59df778u) ||
            buses.render_lock!=limits.render_lock || (void *)buses.state_slot!=(void *)state.state_slot) {
            puts("BAD native audio bus binding");bad++;
        } else puts("OK  native audio bus binding");
        const sig_entry *dispatch=NULL;sig_result dispatch_result={0};sh_audio_bank_gate gate={0};
        for(size_t i=0;BACKEND_ENGINE_SIGNATURES[i].name;i++)
            if(!strcmp(BACKEND_ENGINE_SIGNATURES[i].name,"AudioBankDispatch"))dispatch=BACKEND_ENGINE_SIGNATURES+i;
        uintptr_t dispatch_rva=pinned ? 0x1d2c030u : 0x1d454d0u;
        if(!dispatch || sig_resolve_one(base,dispatch,&dispatch_result)!=SIG_OK ||
            !sh_audio_bank_gate_bind(&gate,base,&dispatch_result,1,&limits) ||
            (uintptr_t)gate.entry!=(uintptr_t)base+dispatch_rva || gate.render_lock!=limits.render_lock ||
            gate.manager_slot!=limits.manager_slot || gate.initialized) {
            puts("BAD native bank publication binding");bad++;
        } else puts("OK  native bank publication binding");
    }

    /* The native profile reader derives its filesystem/allocator slots from
     * instructions adjacent to the signed-length guard on each renderer. */
    {
        const char *names[] = {"SnapMapReadSizeLimit", "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap",
            "PublishedMapComplete", "PublishedMapSelectionConstruct", "PublishedMapMetadataDestroy", "DeserializeFromJson",
            "PublishedMapOfflineLaunch", "PublishedMapLobbyLaunch", "PublishedMapDirectLaunch", "PublishedMapCacheReady",
            "PublishedMapRead", "PublishedMapCacheSet", "SerializeToJson"};
        sig_result source[15] = {0};
        int bound = 1;
        for (size_t i = 0; i < 15; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) {
                printf("BAD published source signature %s\n", names[i]); bound = 0;
            }
        }
        if (!bound || !sh_map_source_install(source, 15, base) || !sh_map_native_bind(source, 15, base) ||
            !sh_map_published_bind(source, 15, base)) {
            printf("BAD native profile source binding\n"); bad++;
        } else printf("OK  native profile source binding\n");
    }

    /* One read-only engine value bounds stored map text (map_capacity.h). The
     * cached saved/published reader compares it at the JLE map_payload.c lifts;
     * the publish and copy uploads compare it with MOVSXD RCX,[RIP+disp32];
     * CMP RAX,RCX; JLE; the save builder checks length - 1 against an
     * immediate one below it. All accept exactly up to the value. */
    {
        const sig_entry *read_entry = NULL, *save_entry = NULL;
        sig_result read_site = {0}, save_site = {0};
        uintptr_t jle = 0, bound = 0, upload[4] = {0};
        int32_t value = 0;
        size_t uploads = 0;
        int resolved, read_ok = 0, save_ok = 0, pinned_ok;
        for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++) {
            if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "SnapMapReadSizeLimit")) read_entry = &BACKEND_ENGINE_SIGNATURES[i];
            if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "SnapMapSaveSizeLimit")) save_entry = &BACKEND_ENGINE_SIGNATURES[i];
        }
        resolved = read_entry && save_entry && sig_resolve_one(base, read_entry, &read_site) == SIG_OK &&
                   sig_resolve_one(base, save_entry, &save_site) == SIG_OK;
        if (resolved) {
            const uint8_t *site = (const uint8_t *)read_site.addr, *save = (const uint8_t *)save_site.addr;
            jle = read_site.addr + 21;
            bound = jle + (intptr_t)*(const int32_t *)(jle - 4);
            if (bound >= (uintptr_t)base && bound + 4 <= (uintptr_t)base + image_sz) value = *(const int32_t *)bound;
            read_ok = site[15] == 0x3B && site[16] == 0x05 && site[21] == 0x7E;
            save_ok = save[0] == 0x3D && *(const int32_t *)(save + 1) == value - 1 && save[5] == 0x77;
            for (const uint8_t *p = base; p + 12 <= base + image_sz; p++) {
                if (p[0] == 0x48 && p[1] == 0x63 && p[2] == 0x0D && p[7] == 0x48 && p[8] == 0x3B &&
                    p[9] == 0xC1 && p[10] == 0x0F && p[11] == 0x8E &&
                    (uintptr_t)(p + 7) + (intptr_t)*(const int32_t *)(p + 3) == bound) {
                    if (uploads < 4) upload[uploads] = (uintptr_t)(p - base);
                    uploads++;
                }
            }
        }
        pinned_ok = !pinned || (bound - (uintptr_t)base == 0x20516d4u && upload[0] == 0x58b945u && upload[1] == 0x58cd36u);
        if (!resolved || value != 10485760 || !read_ok || !save_ok || uploads != 2 || !pinned_ok) {
            printf("BAD shared native map text limit (value %d, uploads %zu)\n", (int)value, uploads); bad++;
        } else printf("OK  shared native map text limit %d: cached reader, publish and copy uploads, save builder\n", (int)value);
    }

    {
        const char *names[] = {"AllocateGameResources", "UnloadGameResources", "FinalizeMapChange", "CancelMapChange"};
        sig_result source[4] = {0};
        int bound = 1;
        for (size_t i = 0; i < 4; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        if (!bound || !sh_map_transition_bind(source, 4, base)) {
            printf("BAD native map transition binding\n"); bad++;
        } else printf("OK  native map transition binding\n");
    }
    {
        const char *names[] = {"ResourceReloadRenderScope", "PublishedMapComplete", "DeclSourceModeCall",
            "ResourceReconstruct", "ResourceGenericLoad", "ResourceLookup", "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap",
            "MaterialVirtualTextureRebind", "SnapWorldTextFonts"};
        sig_result source[sizeof(names) / sizeof(names[0])] = {0};
        int bound = 1;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        if (!bound || !sh_resource_resident_bind(source, sizeof(source) / sizeof(source[0]), base)) {
            printf("BAD native resident refresh binding\n"); bad++;
        } else printf("OK  native resident refresh binding\n");
    }
    {
        /* Running-state file IO (R-309): the resource manager's +0x68 slot and
         * method, and the memory-file helpers decoded from idFileResource Load. */
        const char *names[] = {"ResourceManagerConstruct", "ResourceManagerOpenFile", "FileResourceLoad"};
        sig_result source[3] = {0};
        sh_resource_file_visibility_native native;
        uintptr_t slot = pinned ? 0x2710ba8u : 0x2726628u, open = pinned ? 0x18071f0u : 0x17f9b20u;
        uintptr_t made = pinned ? 0x1b16d58u : 0x1b303e8u, memory = pinned ? 0x1a19d40u : 0x1a0bf90u;
        uintptr_t fill = pinned ? 0x17ffc70u : 0x17f25c0u, release = pinned ? 0x1e1d89cu : 0x1e377dcu;
        int bound = 1;
        for (size_t i = 0; i < 3; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        if (!bound || !sh_resource_file_visibility_decode(source, 3, base, &native) ||
            (uintptr_t)native.slot != (uintptr_t)base + slot || (uintptr_t)native.open != (uintptr_t)base + open ||
            (uintptr_t)native.operator_new != (uintptr_t)base + made ||
            (uintptr_t)native.memory_file != (uintptr_t)base + memory || (uintptr_t)native.fill != (uintptr_t)base + fill ||
            (uintptr_t)native.operator_delete != (uintptr_t)base + release) {
            printf("BAD running-state resource-file binding\n"); bad++;
        } else printf("OK  running-state resource-file binding\n");
    }
    {
        /* SWF font-FX registration (R-309): its material manager, decoded only
         * when its DeclFind call targets the DeclFind signature. */
        const char *names[] = {"SwfRegisterFontFX", "DeclFind"};
        sig_result source[2] = {0};
        uintptr_t registration = pinned ? 0x289b80u : 0x2890a0u, find = pinned ? 0x17b36f0u : 0x17a61b0u;
        uintptr_t manager = pinned ? 0x59bd9d0u : 0x42bbce0u;
        int bound = 1;
        for (size_t i = 0; i < 2; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        if (!bound || source[0].addr != (uintptr_t)base + registration || source[1].addr != (uintptr_t)base + find ||
            (uintptr_t)sh_swf_fontfx_manager_decode((const unsigned char *)source[0].addr, (const void *)source[1].addr) !=
                (uintptr_t)base + manager) {
            printf("BAD SWF font-FX registration binding\n"); bad++;
        } else printf("OK  SWF font-FX registration binding\n");
    }
    {
        /* Image exclusion state (R-309): the purge driver names the native resource-list lock
         * getter, its blocking acquire and release, and the list-of-lists head every native add,
         * remove and purge uses; that head is the globals table's decl_resource_head. */
        const sig_entry *entry = NULL;
        sig_result source = {0};
        sh_image_exclusion_native native;
        uintptr_t purge = pinned ? 0x1800e10u : 0x17f3760u, lock = pinned ? 0x18018a0u : 0x17f41e0u;
        uintptr_t acquire = pinned ? 0x1a40cb0u : 0x1a33260u, release = pinned ? 0x1a40d00u : 0x1a332b0u;
        uintptr_t head = pinned ? 0x6217f90u : 0x4b162a0u;
        for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
            if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, "ResourcePurgeLevel")) entry = &BACKEND_ENGINE_SIGNATURES[j];
        void *const *table = (void *const *)glb_resolve(base, "decl_resource_head", NULL);
        if (!entry || sig_resolve_one(base, entry, &source) != SIG_OK || source.addr != (uintptr_t)base + purge ||
            !sh_image_exclusion_decode((const unsigned char *)source.addr, &native) ||
            (uintptr_t)native.lock != (uintptr_t)base + lock || (uintptr_t)native.acquire != (uintptr_t)base + acquire ||
            (uintptr_t)native.release != (uintptr_t)base + release || (uintptr_t)native.head != (uintptr_t)base + head ||
            (void *const *)native.head != table) {
            printf("BAD image exclusion resource-list lock binding\n"); bad++;
        } else printf("OK  image exclusion resource-list lock binding\n");
    }
    {
        /* Private page providers: the VT system and provider stride, SetSource,
         * its quiesce and release calls, and OpenGL's sparse-mode globals. */
        const char *names[] = {"MaterialVirtualTextureRebind", "VirtualTextureSystemInit"};
        sig_result source[2] = {0};
        sh_vt_native vt;
        int bound = 1;
        for (size_t i = 0; i < 2; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        void *const *resources = (void *const *)glb_resolve(base, "decl_resource_head", NULL);
        uintptr_t system = pinned ? 0x5861d10u : 0x415f210u, set = pinned ? 0xe11c50u : 0xe03770u;
        uintptr_t release = pinned ? 0xe10460u : 0xe01f00u, quiesce = pinned ? 0xe1c290u : 0xe0e160u;
        uintptr_t init = pinned ? 0xe18440u : 0xe0a340u;
        size_t stride = pinned ? 0x458u : 0x520u;
        if (!bound || !resources || !sh_vt_provider_decode(source, 2, resources, &vt) ||
            (uintptr_t)vt.system != (uintptr_t)base + system || vt.stride != stride ||
            (uintptr_t)vt.set_source != (uintptr_t)base + set || (uintptr_t)vt.release != (uintptr_t)base + release ||
            (uintptr_t)vt.quiesce != (uintptr_t)base + quiesce || source[1].addr != (uintptr_t)base + init ||
            (pinned ? vt.sparse_function || vt.sparse_enabled :
                (uintptr_t)vt.sparse_function != (uintptr_t)base + 0x59d2d48u ||
                (uintptr_t)vt.sparse_enabled != (uintptr_t)base + 0x415f1b0u)) {
            printf("BAD private virtual texture provider binding\n"); bad++;
        } else printf("OK  private virtual texture provider binding\n");
    }
    {
        const char *names[] = {"EditorLoadMap", "EditorSetActive", "ResourceReloadRenderScope",
            "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap"};
        sig_result source[sizeof(names) / sizeof(names[0])] = {0};
        int bound = 1;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        if (!bound || !sh_editor_transition_bind(source, sizeof(source) / sizeof(source[0]), base)) {
            printf("BAD native editor transition binding\n"); bad++;
        } else printf("OK  native editor transition binding\n");
    }
    for (size_t i = 0; i < total; i++) {
        const global_entry *e = &BACKEND_ENGINE_GLOBALS[i];
        glb_status st = GLB_UNKNOWN_NAME;
        uintptr_t a = glb_resolve(base, e->name, &st);
        if (!a) {
            printf("BAD %-24s UNRESOLVED status=%d\n", e->name, (int)st);
            bad++;
            continue;
        }
        ok++;
        uint32_t rva = (uint32_t)(a - (uintptr_t)base);
        if (pinned && rva != e->pinned_rva) {
            printf("BAD %-24s resolved=0x%-9x pinned=0x%x\n", e->name, rva, e->pinned_rva);
            bad++;
        } else if (pinned) {
            printf("OK  %-24s resolved=0x%-9x\n", e->name, rva);
        } else {
            printf("OK  %-24s resolved=0x%-9x (pinned 0x%x)\n", e->name, rva, e->pinned_rva);
        }
    }

    /* Class-B visibility recovery resumes at leaf+0x24. Verify the expected
     * xor al,al; ret tail in the supplied image before trusting that offset. */
    {
        glb_status st = GLB_UNKNOWN_NAME;
        uintptr_t lo = glb_resolve(base, "vis_leaf_lo", &st);
        if (!lo) {
            printf("BAD vis_leaf_lo UNRESOLVED status=%d\n", (int)st);
            bad++;
        } else {
            const uint8_t *tail = (const uint8_t *)(lo + 0x24);
            if (tail[0] == 0x32 && tail[1] == 0xC0 && tail[2] == 0xC3) {
                printf("OK  invariant vis_leaf_lo + 0x24 == xor al,al; ret\n");
            } else {
                printf("BAD vis_leaf_lo + 0x24 is %02x %02x %02x, not xor al,al; ret -- "
                       "the shield must not redirect here\n", tail[0], tail[1], tail[2]);
                bad++;
            }
        }
    }

    /* The runtime metadata binder must locate reflection without executing the
     * lazy singleton accessor. Static PE state has not initialized its readers. */
    {
        metadata_image image = {base, image_sz, 0};
        sh_decl_native_source source = {&image, metadata_read, 0};
        uintptr_t accessor = glb_resolve(base, "declmgr_accessor", NULL);
        uintptr_t container = glb_resolve(base, "type_container", NULL);
        uintptr_t rva = container - (uintptr_t)base;
        uintptr_t expected = rva == 0x3082b10u ? (uintptr_t)base + 0x6012120u :
            rva == 0x30c78e0u ? (uintptr_t)base + 0x4910430u : 0;
        int state = sh_decl_native_source_bind(&source, accessor, container);
        if (state != 0 || source.reflection || image.last != expected) {
            printf("BAD read-only metadata binder state=%d read=0x%llx expected=0x%llx\n",
                state, (unsigned long long)(image.last - (uintptr_t)base),
                (unsigned long long)(expected - (uintptr_t)base));
            bad++;
        } else printf("OK  read-only metadata binder locates uninitialized reflection at 0x%llx\n",
            (unsigned long long)(expected - (uintptr_t)base));

        /* Use the actual export directory, as runtime GetProcAddress does.
         * The getter's relative target and native list layout must agree on
         * both images; static reader allocation alone is not readiness. */
        {
            uintptr_t getter = image_export(base, image_sz, "GetGameSystemInterface");
            uintptr_t system = rva == 0x3082b10u ? (uintptr_t)base + 0x2dfa1b0u :
                rva == 0x30c78e0u ? (uintptr_t)base + 0x2e3f050u : 0;
            source.reflection = 1;
            state = sh_decl_native_source_ready(&source, accessor, container, getter);
            if (!getter || state != 0 || source.reflection || image.last != system + 8) {
                printf("BAD native reader readiness state=%d getter=0x%llx read=0x%llx\n", state,
                    (unsigned long long)(getter - (uintptr_t)base),
                    (unsigned long long)(image.last - (uintptr_t)base));
                bad++;
            } else printf("OK  native reader readiness waits at system 0x%llx through exported getter 0x%llx\n",
                (unsigned long long)(system - (uintptr_t)base),
                (unsigned long long)(getter - (uintptr_t)base));
        }
    }

    /* Layout invariants -- true on any build, so they run in both modes. */
    check_adjacent(base, "cmd_system_slot",      "cvar_system_slot",     0x10, &bad);
    check_adjacent(base, "main_thread_id",       "load_state",           0x08, &bad);
    check_adjacent(base, "main_thread_id",       "error_state",          0x0C, &bad);
    check_adjacent(base, "resource_manager_ctx", "material_manager_ctx", 0xE0, &bad);
    check_adjacent(base, "class_sorted_types",   "class_sorted_count",   0x08, &bad);
    check_adjacent(base, "managed_class_first",  "managed_class_last",   0x04, &bad);
    /* Static image state precedes idClass::Init: the registry is unready
     * (never a conclusive absence). */
    {
        metadata_image image = {base, image_sz, 0};
        sh_native_class_source classes = {&image, metadata_read,
            glb_resolve(base, "class_registry_ready", NULL), glb_resolve(base, "class_sorted_types", NULL),
            glb_resolve(base, "class_sorted_count", NULL), glb_resolve(base, "managed_class_first", NULL),
            glb_resolve(base, "managed_class_last", NULL)};
        int state = sh_native_class_lookup(&classes, "idManagedClass", NULL);
        if (state != -1 || !classes.ready) { printf("BAD idClass registry static state=%d\n", state); bad++; }
        else printf("OK  idClass registry reads unready before Init\n");
    }

    printf("======================================================================\n");
    printf("global resolver [%s]: %zu/%zu resolved; %d failures\n",
           pinned ? "pinned" : "portable", ok, total, bad);
    free(base);
    return (ok == total && bad == 0) ? 0 : 1;
}
