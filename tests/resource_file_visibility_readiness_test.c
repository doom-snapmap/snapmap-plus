#define main rfv_existing_main
#include "resource_file_visibility_test.c"
#undef main

int main(void)
{
    sh_resource_file_visibility_native n={0};void *slot=(void *)rfv_open_hook;
    n.slot=&slot;n.open=(void *)native_open;n.operator_new=(void *)fake_new;
    n.operator_delete=(void *)fake_delete;n.memory_file=(void *)fake_memory;
    n.fill=(void *)fake_fill;
    g_slot=n.slot;g_open=(rfv_open_fn)n.open;g_new=(rfv_new_fn)n.operator_new;
    g_delete=(rfv_delete_fn)n.operator_delete;g_memory=(rfv_memory_fn)n.memory_file;
    g_fill=(rfv_fill_fn)n.fill;
    SetLastError(734);assert(sh_resource_file_visibility_dispatch_ready(&n));assert(GetLastError()==734);
    slot=n.open;assert(!sh_resource_file_visibility_dispatch_ready(&n));slot=(void *)rfv_open_hook;
    void *old=n.fill;n.fill=(void *)native_open;assert(!sh_resource_file_visibility_dispatch_ready(&n));n.fill=old;
    old=n.operator_delete;n.operator_delete=NULL;assert(!sh_resource_file_visibility_dispatch_ready(&n));n.operator_delete=old;
    g_open=NULL;assert(!sh_resource_file_visibility_dispatch_ready(&n));g_open=(rfv_open_fn)n.open;
    assert(!sh_resource_file_visibility_dispatch_ready(NULL));assert(sh_resource_file_visibility_dispatch_ready(&n));
    g_slot=NULL;g_open=NULL;g_new=NULL;g_delete=NULL;g_memory=NULL;g_fill=NULL;
    puts("resource file visibility current ownership controls passed");return 0;
}
