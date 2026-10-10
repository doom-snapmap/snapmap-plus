# Compile and run the offline C/C++ and JavaScript tests with MSVC x64 and Node.
# Requires Visual Studio C++ Build Tools. Outputs go to tests\obj (gitignored).
#
#   tests\run-tests.ps1
#   tests\run-tests.ps1 -Doom <unpacked Vulkan exe> -DoomAlt <unpacked OpenGL exe>
#   tests\run-tests.ps1 -Skip map_published_test
#
# The default suite needs no game or built DLL. -Doom adds pinned signature,
# hook-fallback and global checks; -DoomAlt checks the second image in portable
# mode, accepting address shifts while requiring unique matches and layout checks.
# Exit 0 means every selected test passed; failures include their build log.
#
# Optional manual probes:
#   tests\obj\package_compiler_test.exe <data-root> <doom-base>
# Both resource paths are required; there is no autodiscovery. The probe skips
# invalid local packages like the runtime and prints each skipped package.
# For xinput_ordinal_test against the built proxy, see docs\contributing.md.
param([string]$Doom = "", [string]$DoomAlt = "", [string[]]$Only = @(), [string[]]$Skip = @())
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$obj  = Join-Path $here "obj"
New-Item -ItemType Directory -Force $obj | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Import the MSVC environment ONCE. vcvars64.bat costs seconds, so running it per
# test spent minutes on setup alone. Names starting with "=" are cmd's per-drive
# working directories and are skipped by the pattern.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "cl not on PATH after $vcvars" }

# name | sources (relative to tests\) | runtime arg
$tests = @(
    @{ name = "package_legacy_test"; src = 'package_legacy_test.c ..\src\backend\resource_catalog.c ..\src\backend\package_legacy.c ..\src\backend\package_migration.c ..\src\backend\decl_server_path.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c ..\src\backend\raw_deflate.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_migration_test"; src = 'package_migration_test.c ..\src\backend\package_migration.c ..\src\backend\decl_server_path.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\package_sources.c ..\src\backend\packages.c'; arg = "" }
    @{ name = "package_owners_test"; src = 'package_owners_test.c'; arg = "" }
    @{ name = "package_link_graph_test"; src = 'package_link_graph_test.c ..\src\backend\package_link_graph.c'; arg = "" }
    @{ name = "package_consumers_test"; src = 'package_consumers_test.c ..\src\backend\package_consumers.c'; arg = "" }
    @{ name = "package_font_test"; src = 'package_font_test.c ..\src\backend\package_font.c ..\src\backend\font_binary.c ..\src\backend\package_consumers.c'; arg = "" }
    @{ name = "package_link_pipeline_test"; src = 'package_link_pipeline_test.c ..\src\backend\package_scope.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\decl_rebind.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_linker_test"; src = 'package_linker_test.c ..\src\backend\package_provenance.c ..\src\backend\map_provenance.c ..\src\backend\map_shards.c ..\src\backend\package_scope.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_linker.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\decl_rebind.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; defs = '/DSH_PACKAGE_LINKER_TESTING'; arg = "" }
    @{ name = "model_reference_test"; src = 'model_reference_test.c ..\src\backend\model_reference.c'; arg = "" }
    @{ name = "virtual_material_native_test"; src = 'virtual_material_native_test.c ..\src\backend\virtual_material_native.c ..\src\backend\virtual_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "virtual_material_runtime_test"; src = 'virtual_material_runtime_test.c ..\src\backend\virtual_material_native.c ..\src\backend\virtual_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "virtual_material_test"; src = 'virtual_material_test.c ..\src\backend\virtual_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "virtual_texture_provider_test"; src = 'virtual_texture_provider_test.c ..\src\backend\virtual_texture_provider.c'; defs = '/DSH_VT_PROVIDER_TESTING'; arg = "" }
    @{ name = "package_virtual_pages_test"; src = 'package_virtual_pages_test.c ..\src\backend\package_virtual_pages.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "decl_native_rebind_test"; src = 'decl_native_rebind_test.c ..\src\backend\decl_native_rebind.c ..\src\backend\resource_rebind.c ..\src\backend\model_binary.c ..\src\backend\md6_binary.c ..\src\backend\decl_material.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_md6_compose.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "package_link_identity_test"; src = 'package_link_identity_test.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "map_provenance_test"; src = 'map_provenance_test.c ..\src\backend\map_provenance.c ..\src\backend\map_shards.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c'; arg = "" }
    @{ name = "package_provenance_test"; src = 'package_provenance_test.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c'; arg = "" }
    @{ name = "package_scope_test"; src = 'package_scope_test.c ..\src\backend\package_scope.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "decl_tree_test"; src = 'decl_tree_test.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "decl_script_native_test"; src = 'decl_script_native_test.c ..\src\backend\decl_script_native.c ..\src\backend\decl_script_schema.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; defs = '/DSH_DECL_SCRIPT_NATIVE_TESTING'; arg = "" }
    @{ name = "decl_native_lex_test"; src = 'decl_native_lex_test.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "decl_material_test"; src = 'decl_material_test.c ..\src\backend\decl_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "decl_renderparm_test"; src = 'decl_renderparm_test.c ..\src\backend\decl_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "decl_material_compose_test"; src = 'decl_material_compose_test.c ..\src\backend\decl_material_compose.c ..\src\backend\decl_material.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_polymorphic_test"; src = 'decl_polymorphic_test.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_compose.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "decl_graph_compose_test"; src = 'decl_graph_compose_test.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_graph.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_md6_compose_test"; src = 'decl_md6_compose_test.c ..\src\backend\decl_md6_compose.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_block_compose_test"; src = 'decl_block_compose_test.c ..\src\backend\decl_block_compose.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "md6_binary_test"; src = 'md6_binary_test.c ..\src\backend\md6_binary.c ..\src\backend\model_binary.c ..\src\backend\resource_rebind.c'; arg = "" }
    @{ name = "model_binary_test"; src = 'model_binary_test.c ..\src\backend\model_binary.c ..\src\backend\md6_binary.c ..\src\backend\resource_rebind.c'; arg = "" }
    @{ name = "swf_binary_test"; src = 'swf_binary_test.c ..\src\backend\swf_binary.c ..\src\backend\model_binary.c ..\src\backend\md6_binary.c ..\src\backend\resource_rebind.c'; arg = "" }
    @{ name = "font_binary_test"; src = 'font_binary_test.c ..\src\backend\font_binary.c'; arg = "" }
    @{ name = "package_movie_test"; src = 'package_movie_test.c ..\src\backend\package_movie.c ..\src\backend\swf_binary.c ..\src\backend\package_consumers.c ..\src\backend\resource_rebind.c ..\src\backend\model_binary.c ..\src\backend\md6_binary.c'; arg = "" }
    @{ name = "decl_graph_test"; src = 'decl_graph_test.c ..\src\backend\decl_graph.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "decl_entity_class_test"; src = 'decl_entity_class_test.c ..\src\backend\decl_entity_class.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "decl_native_carrier_test"; src = 'decl_native_carrier_test.c ..\src\backend\decl_native_carrier.c ..\src\backend\decl_tree.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_native_inline_test"; src = 'package_native_inline_test.c ..\src\backend\package_native_inline.c ..\src\backend\decl_native_carrier.c ..\src\backend\decl_entity_class.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_tree.c ..\src\backend\config_json.c'; defs = '/DSH_NATIVE_INLINE_TESTING'; arg = "" }
    @{ name = "package_editor_rows_test"; src = 'package_editor_rows_test.c ..\src\backend\package_editor_rows.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "decl_dependencies_test"; src = 'decl_dependencies_test.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_rebind_test"; src = 'decl_rebind_test.c ..\src\backend\decl_rebind.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_native_schema_test"; src = 'decl_native_schema_test.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_script_schema.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_compose.c ..\src\backend\decl_graph.c ..\src\backend\decl_graph_dependencies.c'; arg = "" }
    @{ name = "decl_native_registry_test"; src = 'decl_native_registry_test.c ..\src\backend\decl_native_registry.c'; arg = "" }
    @{ name = "native_resource_id_test"; src = 'native_resource_id_test.c ..\src\backend\native_resource_id.c'; arg = "" }
    @{ name = "package_native_registry_test"; src = 'package_native_registry_test.c ..\src\backend\package_native_registry.c ..\src\backend\decl_native_registry.c'; arg = "" }
    @{ name = "decl_visibility_test"; src = 'decl_visibility_test.c'; arg = "" }
    @{ name = "resource_graph_prepared_test"; src = 'resource_graph_prepared_test.c ..\src\backend\resource_graph_prepared.c ..\src\backend\resource_graph.c ..\src\backend\decl_native_registry.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_script_schema.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "startup_bindings_test"; src = 'startup_bindings_test.c ..\src\backend\startup_bindings.c'; arg = "" }
    @{ name = "serialized_entities_guard_test"; src = 'serialized_entities_guard_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "resource_graph_test"; src = 'resource_graph_test.c ..\src\backend\resource_graph.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "resource_resident_test"; src = 'resource_resident_test.c ..\src\backend\resource_graph.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "resource_graph_native_test"; src = 'resource_graph_native_test.c ..\src\backend\resource_graph.c ..\src\backend\package_font.c ..\src\backend\font_binary.c ..\src\backend\package_consumers.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "package_archive_test"; src = 'package_archive_test.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c ..\src\backend\raw_deflate.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\map_package.c ..\src\backend\decl_server_path.c ..\src\backend\package_migration.c ..\src\backend\package_legacy.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "package_descriptor_test"; src = 'package_descriptor_test.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "decl_compose_test"; src = 'decl_compose_test.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_composition_classes_test"; src = 'decl_composition_classes_test.c ..\src\backend\decl_compose.c ..\src\backend\decl_entity_class.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "package_sources_test"; src = 'package_sources_test.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_cache_test"; src = 'package_cache_test.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\audio_bank_codec.c ..\src\backend\audio_hirc.c ..\src\backend\audio_banks.c'; arg = "" }
    @{ name = "package_strings_test"; src = 'package_strings_test.c ..\src\backend\package_strings.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_compiler_test"; src = 'package_compiler_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_environment_test"; src = 'package_environment_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_context_test"; src = 'package_context_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_runtime_context_test"; src = 'package_runtime_context_test.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_md6_compose.c ..\src\backend\decl_block_compose.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_graph.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_producers_test"; src = 'package_producers_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_source_view_test"; src = 'package_source_view_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_decl_families_test"; src = 'package_decl_families_test.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_md6_compose.c ..\src\backend\decl_block_compose.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_graph.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_usage_test"; src = 'package_usage_test.c ..\src\backend\resource_graph.c ..\src\backend\package_usage.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "package_source_graph_test"; src = 'package_source_graph_test.c ..\src\backend\resource_graph.c ..\src\backend\package_usage.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c ..\src\backend\package_source_graph.c ..\src\backend\decl_native_registry.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_script_schema.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\decl_graph.c ..\src\backend\decl_graph_dependencies.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_PACKAGE_LIBRARY_TESTING'; arg = "" }
    @{ name = "package_scale_test"; src = 'package_scale_test.c ..\src\backend\resource_graph.c ..\src\backend\package_usage.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c ..\src\backend\map_embed.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "map_render_test"; src = 'map_render_test.c ..\src\backend\map_render.c'; arg = "" }
    @{ name = "map_render_editor_test"; src = 'map_render_editor_test.c ..\src\backend\map_render.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "map_render_native_test"; src = 'map_render_native_test.c ..\src\backend\map_render.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_nav_test"; src = 'grid_room_nav_test.c ..\src\backend\grid_room_nav.c ..\src\backend\grid_room.c ..\src\backend\aas_edit.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "grid_room_asset_test"; src = 'grid_room_asset_test.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_test"; src = 'grid_room_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_editor_test"; src = 'grid_room_editor_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_native_test"; src = 'grid_room_native_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\grid_room_edit.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_snap_test"; src = 'grid_room_snap_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "spawn_keys_test"; src = 'spawn_keys_test.c ..\src\backend\spawn_keys.c ..\src\backend\package_keys.c ..\src\backend\decl_tree.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_SPAWN_KEYS_TESTING'; arg = "" }
    @{ name = "package_spawn_registry_test"; src = 'package_spawn_registry_test.c ..\src\backend\package_spawn_registry.c ..\src\backend\package_keys.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "snap_type_cache_test"; src = 'snap_type_cache_test.c ..\src\backend\snap_type_cache.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_SNAP_TYPE_CACHE_TESTING'; arg = "" }
    @{ name = "key_inspector_test"; src = 'key_inspector_test.c ..\src\backend\key_inspector.c ..\src\backend\package_keys.c ..\src\backend\decl_tree.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_KEY_INSPECTOR_TESTING'; arg = "" }
    @{ name = "nav_heap_test"; src = 'nav_heap_test.c'; arg = "" }
    @{ name = "nav_heap_hook_test"; src = 'nav_heap_hook_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "patch_test"; src = 'patch_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "cvar_unlock_test"; src = 'cvar_unlock_test.c ..\src\backend\cvar_unlock.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "map_payload_test"; src = 'map_payload_test.c ..\src\backend\map_payload.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "map_source_test"; src = 'map_source_test.c'; arg = "" }
    @{ name = "map_native_test"; src = 'map_native_test.c ..\src\backend\map_session_json.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "map_session_json_test"; src = 'map_session_json_test.c ..\src\backend\map_session_json.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "map_published_test"; src = 'map_published_test.c'; arg = "" }
    @{ name = "map_transition_test"; src = 'map_transition_test.c'; arg = "" }
    @{ name = "editor_transition_test"; src = 'editor_transition_test.c'; arg = "" }
    @{ name = "json_patch_test"; src = 'json_patch_test.c ..\src\backend\json_patch.c'; arg = "" }
    @{ name = "edit_pair_test"; src = 'edit_pair_test.c'; arg = "" }
    @{ name = "engine_cvar_read_test"; src = 'engine_cvar_read_test.c'; arg = "" }
    @{ name = "apply_dispatch_test"; src = 'apply_dispatch_test.c ..\src\backend\perf.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; defs = '/Gy'; arg = "" }
    @{ name = "apply_flat_serialize_test"; src = 'apply_flat_serialize_test.c ..\src\backend\perf.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; defs = '/Gy'; arg = "" }
    @{ name = "apply_prefab_stage_test"; src = 'apply_prefab_stage_test.c ..\src\backend\perf.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; defs = '/Gy'; arg = "" }
    @{ name = "apply_prefab_export_test"; src = 'apply_prefab_export_test.c ..\src\backend\perf.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; defs = '/Gy'; arg = "" }
    @{ name = "snapstack_pair_test"; src = 'snapstack_pair_test.c'; defs = '/Gy'; arg = "" }
    @{ name = "crash_report_test"; src = 'crash_report_test.c ..\src\fault_shield\crash_record_format.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "recovery_dialog_test"; src = 'recovery_dialog_test.c'; defs = '/Gy'; arg = "" }
    @{ name = "nav_play_test"; src = 'nav_play_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "weapon_hud_test"; src = 'weapon_hud_test.c ..\src\backend\weapon_hud.c ..\src\backend\packages.c ..\src\backend\config_json.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_WEAPON_HUD_TESTING /DSH_PACKAGES_TESTING /DSH_PATCH_TESTING'; arg = "" }
    @{ name = "shield_format_test"; src = 'shield_format_test.c ..\src\fault_shield\fault_record.c ..\src\common\log_rotate.c'; arg = "" }
    @{ name = "hook_test";          src = 'hook_test.c ..\src\backend\hook.c ..\src\backend\patch.c'; defs = '/DSH_PATCH_TESTING';                       arg = "" }
    @{ name = "fsm_web_test";       src = 'fsm_web_test.c ..\src\backend\fsm_web.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_FSM_WEB_TESTING /DSH_PATCH_TESTING'; arg = "" }
    @{ name = "package_fsm_test";   src = 'package_fsm_test.c ..\src\backend\package_fsm.c ..\src\backend\decl_graph.c ..\src\backend\decl_tree.c ..\src\backend\decl_entity_class.c'; arg = "" }
    @{ name = "crash_record_test";  src = 'crash_record_test.c ..\src\fault_shield\crash_record_format.c'; arg = "" }
    @{ name = "report_scrub_test";  src = 'report_scrub_test.c';                                     arg = "" }
    @{ name = "dumpmap_path_test";  src = 'dumpmap_path_test.c';                                     arg = "" }
    @{ name = "json_pretty_test";   src = 'json_pretty_test.c';                                      arg = "" }
    @{ name = "editor_frame_test"; src = 'editor_frame_test.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "prefab_import_test"; src = 'prefab_import_test.c'; arg = "" }
    @{ name = "rawmap_paths_test";  src = 'rawmap_paths_test.c ..\src\backend\package_document.c ..\src\backend\map_provenance.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\hook.c ..\src\backend\patch.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_RAWMAP_TESTING /DSH_CONFIG_TESTING /DSH_PATCH_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "config_json_test";   src = 'config_json_test.c ..\src\backend\config_json.c';         arg = "" }
    @{ name = "native_json_sites_test"; src = 'native_json_sites_test.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "map_binding_sites_test"; src = 'map_binding_sites_test.c ..\src\backend\map_binding_sites.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_bundle_test"; src = 'package_bundle_test.c ..\src\backend\package_bundle.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c ..\src\backend\raw_deflate.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_shader_test"; src = 'package_shader_test.c ..\src\backend\package_shader.c'; arg = "" }
    @{ name = "iface_config_test";  src = 'iface_config_test.c ..\src\common\snapmap_plus_iface.c';   arg = "" }
    @{ name = "config_test";        src = 'config_test.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_test"; src = 'user_overrides_test.c ..\src\backend\user_overrides.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING /DSH_USER_OVERRIDES_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_contract_test"; src = 'user_overrides_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "decl_server_test"; src = 'decl_server_test.c ..\src\backend\decl_server.c ..\src\backend\engine_dialog.c ..\src\backend\packages.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c'; defs = '/DSH_DECL_SERVER_TESTING'; arg = "" }
    @{ name = "decl_text_order_test"; src = 'decl_text_order_test.c ..\src\backend\decl_text.c'; arg = "" }
    @{ name = "overrides_internal_test"; src = 'overrides_internal_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "decl_server_contract_test"; src = 'decl_server_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "palette_refresh_test"; src = 'palette_refresh_test.c ..\src\backend\palette_refresh.c'; defs = '/DSH_PALETTE_REFRESH_TESTING'; arg = "" }
    @{ name = "process_heap_scope_test"; src = 'process_heap_scope_test.c'; arg = "" }
    @{ name = "render_scope_test"; src = 'render_scope_test.c'; arg = "" }
    @{ name = "engine_dialog_test"; src = 'engine_dialog_test.c ..\src\backend\engine_dialog.c'; defs = '/DSH_ENGINE_DIALOG_TESTING'; arg = "" }
    @{ name = "palette_refresh_contract_test"; src = 'palette_refresh_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "resource_catalog_test"; src = 'resource_catalog_test.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; defs = '/DSH_RAW_DEFLATE_TESTING'; arg = "" }
    @{ name = "packages_test"; src = 'packages_test.c ..\src\backend\packages.c'; defs = '/DSH_PACKAGES_TESTING'; arg = "" }
    @{ name = "map_package_test"; src = 'map_package_test.c ..\src\backend\map_package.c ..\src\backend\decl_server_path.c ..\src\backend\package_migration.c ..\src\backend\package_legacy.c ..\src\backend\map_shards.c ..\src\backend\packages.c ..\src\backend\raw_deflate.c ..\src\backend\raw_deflate_encode.c ..\src\backend\package_archive.c ..\src\backend\package_sources.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; defs = '/DSH_MAP_PACKAGE_TESTING'; arg = "" }
    @{ name = "navmesh_test"; src = 'navmesh_test.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING'; arg = "" }
    @{ name = "nav_regions_test"; src = 'nav_regions_test.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "aas_edit_test"; src = 'aas_edit_test.c ..\src\backend\aas_edit.c'; arg = "" }
    @{ name = "nav_geometry_test"; src = 'nav_geometry_test.c ..\src\backend\nav_geometry.c'; arg = "" }
    @{ name = "aas_augment_test"; src = 'aas_augment_test.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\aas_edit.c ..\src\backend\nav_traversal.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING /DSH_AUG_TESTING /DSH_TRAV_TESTING'; arg = "" }
    @{ name = "nav_bake_test"; src = 'nav_bake_test.c ..\src\backend\resource_graph.c ..\src\backend\perf.c ..\src\backend\nav_bake.c ..\src\backend\nav_regions.c ..\src\backend\aas_edit.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\nav_traversal.c ..\src\backend\map_shards.c'; defs = '/DSH_NAV_BAKE_TESTING /DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "nav_traversal_test"; src = 'nav_traversal_test.c ..\src\backend\nav_traversal.c'; defs = '/DSH_TRAV_TESTING'; arg = "" }
    @{ name = "override_packages_test"; src = 'override_packages_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_OVERRIDES_TESTING /DSH_RESOURCE_GRAPH_TESTING'; libs = 'shell32.lib'; arg = "" }
    # Link the real globals resolver to verify refusal when this non-game process
    # cannot provide the load-state address.
    @{ name = "package_requirements_test"; src = 'package_requirements_test.c ..\src\backend\package_requirements.c ..\src\backend\packages.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_PACKAGE_REQUIREMENTS_TESTING'; arg = "" }
    @{ name = "strids_packages_test"; src = 'strids_packages_test.c ..\src\backend\perf.c ..\src\backend\strids.c ..\src\backend\packages.c ..\src\backend\overrides.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_STRIDS_TESTING /DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "config_message_test"; src = 'config_message_test.cpp ..\src\ui\webview\config_message.cpp'; cxx = $true; arg = "" }
    @{ name = "webview_json_test"; src = 'webview_json_test.cpp ..\src\ui\webview\webview_json.cpp'; cxx = $true; arg = "" }
    @{ name = "prefab_scene_test"; src = 'prefab_scene_test.c ..\src\ui\webview\prefab_scene.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "crash_pending_test"; src = 'crash_pending_test.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_bootstrap_test"; src = 'theme_bootstrap_test.cpp ..\src\ui\webview\theme_bootstrap.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_contract_test"; src = 'theme_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "entity_settings_contract_test"; src = 'entity_settings_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "growing_text_buffer_test"; src = 'growing_text_buffer_test.cpp'; cxx = $true; arg = "" }
    @{ name = "backend_log_test"; src = 'backend_log_test.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = "" }
    @{ name = "preview_test";     src = 'preview_test.c ..\src\backend\preview.c';              arg = "" }
    @{ name = "bcn_test";         src = 'bcn_test.c ..\src\backend\bcn.c';                      arg = "" }
    @{ name = "audio_banks_test"; src = 'audio_banks_test.c ..\src\backend\audio_banks.c';             arg = "" }
    @{ name = "audio_hirc_test"; src = 'audio_hirc_test.c ..\src\backend\audio_hirc.c';               arg = "" }
    @{ name = "audio_shared_policy_test"; src = 'audio_shared_policy_test.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_hirc.c'; arg = "" }
    @{ name = "audio_effective_policy_test"; src = 'audio_effective_policy_test.c ..\src\backend\audio_effective_policy.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_hirc.c'; arg = "" }
    @{ name = "audio_bank_codec_test"; src = 'audio_bank_codec_test.c ..\src\backend\audio_bank_codec.c ..\src\backend\audio_hirc.c ..\src\backend\audio_banks.c'; arg = "" }
    @{ name = "audio_link_graph_test"; src = 'audio_link_graph_test.c ..\src\backend\audio_id_reservations.c ..\src\backend\audio_link_graph.c ..\src\backend\audio_bank_codec.c ..\src\backend\audio_hirc.c ..\src\backend\audio_banks.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "audio_packages_test"; src = 'audio_packages_test.c ..\src\backend\audio_packages.c'; arg = "" }
    @{ name = "audio_originals_test"; src = 'audio_originals_test.c'; arg = "" }
    @{ name = "audio_bank_commands_test"; src = 'audio_bank_commands_test.c'; arg = "" }
    @{ name = "audio_bank_gate_test"; src = 'audio_bank_gate_test.c ..\src\backend\hook.c ..\src\backend\patch.c'; arg = "" }
    @{ name = "audio_parameter_native_test"; src = 'audio_parameter_native_test.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_hirc.c'; arg = "" }
    @{ name = "audio_state_native_test"; src = 'audio_state_native_test.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_hirc.c'; arg = "" }
    @{ name = "audio_switch_native_test"; src = 'audio_switch_native_test.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_hirc.c'; arg = "" }
    @{ name = "audio_environment_native_test"; src = 'audio_environment_native_test.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_hirc.c'; arg = "" }
    @{ name = "audio_limits_native_test"; src = 'audio_limits_native_test.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_hirc.c'; arg = "" }
    @{ name = "audio_banks_native_test"; src = 'audio_banks_native_test.c ..\src\backend\audio_banks.c'; arg = "" }
    @{ name = "audio_files_test"; src = 'audio_files_test.c ..\src\backend\audio_files.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "soundpreview_queue_test"; src = 'soundpreview_queue_test.c';                       arg = "" }
    @{ name = "imgpreview_index_test"; src = 'imgpreview_index_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c';      arg = "" }
    @{ name = "imgpreview_catalog_test"; src = 'imgpreview_catalog_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c';  arg = "" }
    @{ name = "prefabpreview_test"; src = 'prefabpreview_test.c';                                  arg = "" }
    @{ name = "megapreview_io_test"; src = 'megapreview_io_test.c';                                 arg = "" }
    @{ name = "serialization_buffer_test"; src = 'serialization_buffer_test.cpp'; cxx = $true;     arg = "" }
)
if ($Doom) {
    if (-not (Test-Path $Doom)) { throw "-Doom path not found: $Doom" }
    $da = (Resolve-Path $Doom).Path
    $tests += @{ name = "sig_test";     src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c';     arg = $da }
    $tests += @{ name = "host_renderer_test"; src = 'host_renderer_test.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = @($da, "1") }
    $tests += @{ name = "cvar_unlock_image_test"; src = 'cvar_unlock_test.c ..\src\backend\cvar_unlock.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = $da }
    $tests += @{ name = "hooktol_test"; src = 'hooktol_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
    $tests += @{ name = "globals_test"; src = 'globals_test.c ..\src\backend\audio_bank_commands.c ..\src\backend\audio_limits_native.c ..\src\backend\audio_environment_native.c ..\src\backend\audio_state_native.c ..\src\backend\audio_parameter_native.c ..\src\backend\audio_subscription_native.c ..\src\backend\audio_switch_native.c ..\src\backend\audio_bus_native.c ..\src\backend\audio_bank_gate.c map_published_bindings_stubs.c ..\src\backend\virtual_texture_provider.c ..\src\backend\resource_resident.c ..\src\backend\resource_catalog.c ..\src\backend\resource_graph.c ..\src\backend\raw_deflate.c ..\src\backend\map_source.c ..\src\backend\map_native.c ..\src\backend\map_session_json.c ..\src\backend\config_json.c ..\src\backend\map_published.c ..\src\backend\map_transition.c ..\src\backend\editor_transition.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_script_schema.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = $da }
}
# Check the second executable without requiring the pinned image's addresses.
if ($DoomAlt) {
    if (-not (Test-Path $DoomAlt)) { throw "-DoomAlt path not found: $DoomAlt" }
    $alt = (Resolve-Path $DoomAlt).Path
    $tests += @{ name = "sig_test_alt"; src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; arg = @($alt, "portable") }
    $tests += @{ name = "host_renderer_test_alt"; src = 'host_renderer_test.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = @($alt, "0") }
    $tests += @{ name = "cvar_unlock_image_test_alt"; src = 'cvar_unlock_test.c ..\src\backend\cvar_unlock.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = $alt }
    $tests += @{ name = "globals_test_alt"; src = 'globals_test.c ..\src\backend\audio_bank_commands.c ..\src\backend\audio_limits_native.c ..\src\backend\audio_environment_native.c ..\src\backend\audio_state_native.c ..\src\backend\audio_parameter_native.c ..\src\backend\audio_subscription_native.c ..\src\backend\audio_switch_native.c ..\src\backend\audio_bus_native.c ..\src\backend\audio_bank_gate.c map_published_bindings_stubs.c ..\src\backend\virtual_texture_provider.c ..\src\backend\resource_resident.c ..\src\backend\resource_catalog.c ..\src\backend\resource_graph.c ..\src\backend\raw_deflate.c ..\src\backend\map_source.c ..\src\backend\map_native.c ..\src\backend\map_session_json.c ..\src\backend\config_json.c ..\src\backend\map_published.c ..\src\backend\map_transition.c ..\src\backend\editor_transition.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_script_schema.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = @($alt, "portable") }
}

$compiledConsumers = @("package_runtime_context_test", "weapon_hud_test", "package_requirements_test", "strids_packages_test", "override_packages_test", "overrides_internal_test", "decl_server_test")
$compiledConsumers += @("audio_originals_test", "package_native_preparation_runtime_test")
$tests += @{ name = "package_native_preparation_runtime_test"; src = 'package_native_preparation_runtime_test.c'; defs = '/DSH_PACKAGE_RUNTIME_NATIVE_TESTING'; arg = "" }
$compilerSources = @("grid_room_asset.c", "grid_room_nav.c", "aas_edit.c", "grid_room.c", "grid_room_resources.c", "grid_room_decl.c", "package_runtime.c", "resource_graph.c", "package_usage.c", "package_source_graph.c", "decl_native_registry.c", "decl_dependencies.c", "decl_json_dependencies.c", "decl_graph.c", "decl_graph_compose.c", "decl_md6_compose.c", "decl_block_compose.c", "decl_native_lex.c", "md6_binary.c", "model_binary.c", "decl_graph_dependencies.c", "package_compiler.c", "decl_entity_class.c", "decl_native_schema.c", "decl_script_schema.c", "package_sources.c", "packages.c", "package_descriptor.c", "config_json.c", "decl_compose.c", "decl_tree.c", "decl_polymorphic.c", "decl_server_path.c", "decl_text.c", "resource_catalog.c", "raw_deflate.c")
$compilerSources += @("decl_native_lex.c", "decl_material.c", "decl_material_compose.c", "package_material.c", "package_keys.c", "package_spawn_registry.c")
$compilerSources += @("audio_banks.c", "audio_packages.c", "audio_originals.c", "package_audio.c", "package_sound.c")
$compilerSources += @("virtual_material_runtime.c", "virtual_material_native.c", "virtual_material.c", "package_virtual_material.c")
$compilerSources += @("package_link_inputs.c", "package_library.c", "package_source_link.c", "package_linker.c", "package_link_graph.c", "package_link_identity.c", "package_provenance.c", "package_scope.c", "package_native_names.c")
$compilerSources += "package_native_assets.c"
$compilerSources += "package_hud.c"
$compilerSources += @("map_binding_sites.c", "package_bundle.c", "package_archive.c", "raw_deflate_encode.c", "package_shader.c", "package_strings.c")
$compilerSources += @("package_occurrence.c", "map_provenance.c", "map_shards.c")
$compilerSources += @("package_map_uses.c", "map_embed_uses.c")
$compilerSources += @("decl_native_carrier.c", "package_native_inline.c", "package_editor_rows.c")
$nativeAssetsSources = ($tests | Where-Object { $_.name -eq 'package_compiler_test' }).src -replace '^package_compiler_test.c ', 'package_native_assets_test.c '
$nativeAssetsSources += ' ..\src\backend\package_native_assets.c ..\src\backend\package_shader.c ..\src\backend\audio_banks.c'
$nativeAssetsSources += ' ..\src\backend\decl_native_registry.c ..\src\backend\decl_material.c ..\src\backend\decl_native_lex.c'
$tests += @{ name = "package_native_assets_test"; src = $nativeAssetsSources; arg = "" }
$pageSourceSources = ($tests | Where-Object { $_.name -eq 'package_compiler_test' }).src -replace '^package_compiler_test.c ', 'package_virtual_page_source_test.c '
$pageSourceSources += ' ..\src\backend\package_virtual_pages.c'
$tests += @{ name = "package_virtual_page_source_test"; src = $pageSourceSources; arg = "" }
$virtualRuntimeTest = $tests | Where-Object { $_.name -eq 'virtual_material_runtime_test' }
# This fixture includes virtual_material_runtime.c directly. Link its retained
# page-source implementation and the same compiler/source dependencies as the
# standalone page-source suite, without duplicating its own main or sources.
foreach ($source in @((($pageSourceSources -split ' ') | Select-Object -Skip 1)) +
    @('..\src\backend\package_virtual_page_source.c')) {
    if ($source -notin ($virtualRuntimeTest.src -split ' ')) { $virtualRuntimeTest.src += " $source" }
}
$tests += @{ name = "package_link_inputs_test"; src = 'package_link_inputs_test.c ..\src\backend\package_link_inputs.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
$tests += @{ name = "package_native_names_test"; src = 'package_native_names_test.c ..\src\backend\package_native_names.c ..\src\backend\model_reference.c ..\src\backend\package_link_inputs.c ..\src\backend\decl_server_path.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
$atlasSources = ($tests | Where-Object { $_.name -eq 'package_linker_test' }).src -replace '^package_linker_test.c ', 'package_virtual_material_test.c '
$atlasSources += ' ..\src\backend\package_virtual_material.c ..\src\backend\package_link_inputs.c ..\src\backend\package_native_names.c ..\src\backend\virtual_material.c ..\src\backend\virtual_material_native.c ..\src\backend\decl_native_lex.c'
$tests += @{ name = "package_virtual_material_test"; src = $atlasSources; arg = "" }
$materialSources = 'package_material_test.c ..\src\backend\package_material.c ..\src\backend\decl_material.c ..\src\backend\decl_material_compose.c ..\src\backend\decl_native_lex.c'
$materialSources += ' ' + (($tests | Where-Object { $_.name -eq 'package_source_view_test' }).src -replace '^package_source_view_test.c ', '')
$tests += @{ name = "package_material_test"; src = $materialSources; arg = "" }
$busPolicySources = (($tests | Where-Object { $_.name -eq 'audio_link_graph_test' }).src -replace '^audio_link_graph_test.c ', 'audio_bus_policy_test.c ')
$tests += @{ name = 'audio_bus_policy_test'; src = ($busPolicySources + ' ..\src\backend\audio_bus_policy.c'); arg = '' }
$effectiveBusSources = ($busPolicySources -replace '^audio_bus_policy_test.c ', 'audio_effective_bus_test.c ')
$tests += @{ name = 'audio_effective_bus_test'; src = ($effectiveBusSources + ' ..\src\backend\audio_bus_policy.c ..\src\backend\audio_effective_bus.c'); arg = '' }
$busNativeSources = ($effectiveBusSources -replace '^audio_effective_bus_test.c ', 'audio_bus_native_test.c ')
$tests += @{ name = 'audio_bus_native_test'; src = ($busNativeSources + ' ..\src\backend\audio_bus_policy.c ..\src\backend\audio_effective_bus.c'); arg = '' }
$audioSources = 'package_audio_test.c ..\src\backend\package_audio.c ..\src\backend\audio_banks.c'
$audioSources += ' ..\src\backend\audio_originals.c ..\src\backend\audio_packages.c'
$audioSources += ' ..\src\backend\package_audio_program.c ..\src\backend\audio_shared_policy.c ..\src\backend\audio_bus_policy.c ..\src\backend\package_sound.c ..\src\backend\package_audio_capture.c ..\src\backend\audio_link_graph.c ..\src\backend\audio_bank_codec.c ..\src\backend\audio_hirc.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c'
$audioSources += ' ' + (($tests | Where-Object { $_.name -eq 'package_source_view_test' }).src -replace '^package_source_view_test.c ', '')
$tests += @{ name = "package_audio_test"; src = $audioSources; arg = "" }
# The native adapter now prepares real retained audio programs/ID leases as
# well as the legacy catalog. Keep its link closure aligned with that program.
$nativeAudio = $tests | Where-Object { $_.name -eq 'audio_banks_native_test' }
$nativeAudio.src = ($audioSources -replace '^package_audio_test.c ', 'audio_banks_native_test.c ')
$nativeAudio.src += ' ..\src\backend\audio_id_reservations.c ..\src\backend\audio_effective_policy.c ..\src\backend\audio_effective_bus.c ..\src\backend\hook.c ..\src\backend\patch.c'
$soundSources = ($tests | Where-Object { $_.name -eq 'package_compiler_test' }).src -replace '^package_compiler_test.c ', 'package_sound_test.c '
$soundSources += ' ..\src\backend\package_sound.c ..\src\backend\audio_banks.c'
$tests += @{ name = "package_sound_test"; src = $soundSources; arg = "" }
$tests += @{ name = "package_sound_native_test"; src = 'package_sound_native_test.c'; arg = "" }
$opaqueSources = ($tests | Where-Object { $_.name -eq 'package_compiler_test' }).src -replace '^package_compiler_test.c ', 'package_opaque_test.c '
$tests += @{ name = "package_opaque_test"; src = $opaqueSources; arg = "" }
$shaderLinkSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'package_shader_link_test.c '
$tests += @{ name = "package_shader_link_test"; src = $shaderLinkSources; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
$tests += @{ name = "package_shader_receipt_test"; src = 'package_shader_receipt_test.c ..\src\backend\package_shader_receipt.c'; defs = '/DSH_PACKAGE_SHADER_RECEIPT_TESTING'; arg = "" }
$occurrenceSources = ($tests | Where-Object { $_.name -eq 'package_bundle_test' }).src -replace '^package_bundle_test.c ', 'package_occurrence_test.c ..\src\backend\package_occurrence.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c '
$tests += @{ name = "package_occurrence_test"; src = $occurrenceSources; arg = "" }
$tests += @{ name = "package_editor_test"; src = ($occurrenceSources -replace '^package_occurrence_test.c ', 'package_editor_test.c '); arg = "" }
$prefabSources = 'prefab_test.c ..\src\backend\prefab.c ..\src\backend\package_occurrence.c ..\src\backend\package_bundle.c ..\src\backend\map_provenance.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c '
$prefabSources += ($tests | Where-Object { $_.name -eq 'package_archive_test' }).src -replace '^package_archive_test.c ', ''
$tests += @{ name = "prefab_test"; src = $prefabSources; arg = "" }
foreach ($t in $tests) {
    if ($t.name -eq 'package_runtime_context_test') {
        $t.src += ' ..\src\backend\spawn_keys.c'
        $t.defs += ' /DSH_SPAWN_KEYS_TESTING'
        # Activation and recovery bridge the real state-machine web hook API.
        $t.src += ' ..\src\backend\fsm_web.c'
        $t.defs += ' /DSH_FSM_WEB_TESTING'
    }
    if ('..\src\backend\package_library.c' -in ($t.src -split ' ') -or $t.name -in $compiledConsumers -or
        $t.name -in @('package_source_graph_test', 'package_shader_link_test')) { $t.src += ' ..\src\backend\package_consumers.c' }
    if ('..\src\backend\package_source_graph.c' -in ($t.src -split ' ')) { $t.src += ' ..\src\backend\map_binding_sites.c ..\src\backend\package_shader.c ..\src\backend\package_keys.c ..\src\backend\package_spawn_registry.c' }
    if ($t.name -in @("map_package_test", "package_archive_test", "package_scale_test")) { $t.src += ' ..\src\backend\package_bundle.c' }
    if ($t.name -eq "package_scale_test") { $t.src += ' ..\src\backend\package_provenance.c ..\src\backend\package_scope.c ..\src\backend\package_link_inputs.c' }
    if ($t.name -in @("package_linker_test", "package_source_graph_test", "package_shader_link_test")) { $t.src += ' ..\src\backend\package_link_inputs.c' }
    if ($t.name -eq "package_linker_test") { $t.src += ' ..\src\backend\package_native_names.c' }
    if ($t.name -in @("package_source_graph_test", "package_shader_link_test")) { $t.src += ' ..\src\backend\package_source_link.c ..\src\backend\package_library.c ..\src\backend\package_linker.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c ..\src\backend\package_native_names.c' }
    if ($t.name -in @("package_source_graph_test", "package_shader_link_test")) { $t.src += ' ..\src\backend\package_virtual_material.c ..\src\backend\virtual_material.c ..\src\backend\virtual_material_native.c ..\src\backend\package_native_assets.c' }
    if ($t.name -in @("package_source_graph_test", "package_shader_link_test") -or $t.name -in $compiledConsumers) {
        $t.src += ' ..\src\backend\decl_native_reference.c ..\src\backend\model_reference.c ..\src\backend\decl_native_rebind.c ..\src\backend\resource_rebind.c ..\src\backend\decl_rebind.c'
    }
    if ($t.name -in @("package_source_graph_test", "package_shader_link_test")) { $t.src += ' ..\src\backend\decl_material.c ..\src\backend\decl_md6_compose.c ..\src\backend\md6_binary.c ..\src\backend\model_binary.c ..\src\backend\decl_native_lex.c ..\src\backend\audio_banks.c ..\src\backend\package_audio.c ..\src\backend\audio_originals.c ..\src\backend\audio_packages.c' }
    if ($t.name -in $compiledConsumers) {
        if ($t.name -notin @("package_runtime_context_test", "strids_packages_test", "decl_server_test") -and
            '..\src\backend\hook.c' -notin ($t.src -split ' ')) {
            $t.src += ' native_lookup_unavailable_stubs.c'
        }
        foreach ($source in $compilerSources) {
            $relative = '..\src\backend\' + $source
            if ($relative -notin ($t.src -split ' ')) { $t.src += ' ' + $relative }
        }
        $t.defs = "$($t.defs) /DSH_PACKAGE_RUNTIME_TESTING"
    }
    if ('..\src\backend\package_library.c' -in ($t.src -split ' ') -and
        '..\src\backend\package_strings.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_strings.c'
    }
    if ('..\src\backend\package_library.c' -in ($t.src -split ' ') -and
        '..\src\backend\decl_block_compose.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\decl_block_compose.c'
    }
    if ('..\src\backend\package_library.c' -in ($t.src -split ' ')) {
        foreach ($source in @('package_audio_program.c', 'package_audio_capture.c', 'audio_link_graph.c', 'audio_id_reservations.c',
            'audio_bank_codec.c', 'audio_hirc.c', 'audio_shared_policy.c', 'audio_bus_policy.c', 'package_editor_rows.c')) {
            $relative = '..\src\backend\' + $source
            if ($relative -notin ($t.src -split ' ')) { $t.src += ' ' + $relative }
        }
    }
    if ('..\src\backend\package_native_names.c' -in ($t.src -split ' ') -and
        '..\src\backend\swf_binary.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\swf_binary.c'
    }
    if ('..\src\backend\package_native_names.c' -in ($t.src -split ' ') -and
        '..\src\backend\font_binary.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\font_binary.c'
    }
    if ('..\src\backend\package_source_link.c' -in ($t.src -split ' ') -and
        '..\src\backend\package_font.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_font.c'
    }
    # Behavior state-machine discovery reads layer masks wherever the source link is compiled.
    if ('..\src\backend\package_source_link.c' -in ($t.src -split ' ') -and
        '..\src\backend\package_fsm.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_fsm.c'
        if ('..\src\backend\decl_graph.c' -notin ($t.src -split ' ')) { $t.src += ' ..\src\backend\decl_graph.c' }
    }
    # Movie identities are derived wherever native references, source graphs,
    # source links or the native adapter are compiled.
    if (('..\src\backend\decl_native_reference.c' -in ($t.src -split ' ') -or
         '..\src\backend\package_source_graph.c' -in ($t.src -split ' ') -or
         '..\src\backend\package_source_link.c' -in ($t.src -split ' ') -or
         '..\src\backend\package_native_assets.c' -in ($t.src -split ' ') -or
         $t.name -eq 'resource_graph_native_test') -and
        '..\src\backend\package_movie.c' -notin ($t.src -split ' ')) {
        foreach ($source in @('package_movie.c', 'swf_binary.c', 'package_consumers.c', 'font_binary.c', 'package_font.c')) {
            $relative = '..\src\backend\' + $source
            if ($relative -notin ($t.src -split ' ')) { $t.src += ' ' + $relative }
        }
    }
    if ('..\src\backend\package_library.c' -in ($t.src -split ' ') -or
        '..\src\backend\package_source_graph.c' -in ($t.src -split ' ')) {
        foreach ($source in @('package_sound.c', 'audio_banks.c')) {
            $relative = '..\src\backend\' + $source
            if ($relative -notin ($t.src -split ' ')) { $t.src += ' ' + $relative }
        }
    }
    if ('..\src\backend\package_library.c' -in ($t.src -split ' ') -and
        '..\src\backend\package_native_registry.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_native_registry.c'
    }
    if ('..\src\backend\decl_native_registry.c' -in ($t.src -split ' ') -and
        '..\src\backend\native_resource_id.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\native_resource_id.c'
    }
    if (($t.name -in @('resource_graph_native_test', 'resource_resident_test') -or
        '..\src\backend\overrides.c' -in ($t.src -split ' ') -or
        '..\src\backend\package_native_assets.c' -in ($t.src -split ' ')) -and
        '..\src\backend\package_shader_receipt.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_shader_receipt.c'
    }
    if (('..\src\backend\package_native_assets.c' -in ($t.src -split ' ') -or
        '..\src\backend\package_virtual_material.c' -in ($t.src -split ' ')) -and
        '..\src\backend\package_virtual_pages.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_virtual_pages.c'
    }
    if (('..\src\backend\package_virtual_material.c' -in ($t.src -split ' ') -or
        '..\src\backend\package_native_assets.c' -in ($t.src -split ' ')) -and
        '..\src\backend\package_virtual_page_source.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_virtual_page_source.c'
    }
    # The virtual material runtime builds private page providers.
    if (($t.name -in @('virtual_material_runtime_test', 'package_virtual_material_test', 'package_source_graph_test') -or
        '..\src\backend\virtual_material_runtime.c' -in ($t.src -split ' ')) -and
        '..\src\backend\virtual_texture_provider.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\virtual_texture_provider.c'
    }
}

$localizationSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'package_localization_test.c '
$localizationSources += ' ..\src\backend\strids.c'
$tests += @{ name = "package_localization_test"; src = $localizationSources; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_STRIDS_TESTING'; libs = 'shell32.lib'; arg = "" }

$labelSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'strids_labels_test.c '
$labelSources += ' ..\src\backend\strids.c'
$tests += @{ name = "strids_labels_test"; src = $labelSources; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_STRIDS_TESTING'; libs = 'shell32.lib'; arg = "" }

$reverseRootSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'package_reverse_roots_test.c '
$tests += @{ name = "package_reverse_roots_test"; src = $reverseRootSources; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_PACKAGE_SOURCE_LINK_TESTING'; arg = "" }
$fsmLinkSources = $reverseRootSources -replace '^package_reverse_roots_test.c ', 'package_fsm_link_test.c '
$tests += @{ name = "package_fsm_link_test"; src = $fsmLinkSources; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }

$mapUsesSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'package_map_uses_test.c '
$mapUsesSources += ' ..\src\backend\package_map_uses.c ..\src\backend\map_embed.c ..\src\backend\package_bundle.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c'
$tests += @{ name = "package_map_uses_test"; src = $mapUsesSources; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
foreach ($t in $tests) {
    if ('..\src\backend\map_embed.c' -in ($t.src -split ' ') -and
        '..\src\backend\map_embed_uses.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\map_embed_uses.c'
    }
}

if ($Skip.Count) {
    $unknown = @($Skip | Where-Object { $_ -notin $tests.name })
    if ($unknown.Count) { throw "Unknown skipped test(s): $($unknown -join ', ')." }
    if (@($Skip | Where-Object { $_ -in $Only }).Count) { throw "A test cannot be selected by both -Only and -Skip." }
    foreach ($name in $Skip) { Write-Host "[skip] $name (explicit -Skip selection)" }
    $tests = @($tests | Where-Object { $_.name -notin $Skip })
}
if ($Only.Count) {
    $unknown = @($Only | Where-Object { $_ -notin $tests.name })
    if ($unknown.Count) { throw "Unknown test selector(s): $($unknown -join ', '). Use complete names from this manifest, including _test." }
    $tests = @($tests | Where-Object { $_.name -in $Only })
}

$fail = 0
foreach ($t in $tests) {
    $exe = Join-Path $obj ($t.name + ".exe")
    $defs = if ($t.defs) { " $($t.defs)" } else { "" }
    $cxx  = if ($t.cxx)  { " /EHsc /std:c++17" } else { "" }
    $libs = if ($t.libs) { " /link $($t.libs)" } else { "" }
    # Relative output paths avoid cl's quoted trailing-backslash parsing issue.
    $cl  = "cl /nologo /O2 /MT /I..\src\backend /I..\src\common /I..\src\fault_shield /I..\src\ui\webview$cxx$defs $($t.src) /Fe:obj\$($t.name).exe /Foobj\$libs"
    $log = Join-Path $obj ($t.name + ".build.log")
    # Judge the compiler exit code; vcvars may emit unrelated stderr diagnostics.
    cmd /c "cd /d `"$here`" && $cl > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content $log | Write-Host; Write-Host "[FAIL] compile $($t.name)"; $fail++; continue }
    if ($t.arg) { & $exe @($t.arg) } else { & $exe }
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name) (exit $LASTEXITCODE)"; $fail++ }
    else { Write-Host "[ok]   $($t.name)" }
}
if ($fail -gt 0) { Write-Host ""; Write-Host "$fail native test(s) FAILED"; exit 1 }
Write-Host ""; Write-Host "all native tests passed ($($tests.Count))"

if ($Only.Count) { exit 0 }

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { Write-Host "[FAIL] node not found (required for decl editor tests)"; exit 1 }
$jsTests = @("decl_overlay_test.js", "decl_index_order_test.js", "decl_enum_values_test.js", "decl_language_test.js", "ui_assets_test.js", "asset_browser_test.js", "entity_list_test.js", "prefab_transform_test.js", "prefab_viewport_contract_test.js", "window_chrome_contract_test.js", "feedback_channel_test.js", "feedback_renderer_test.js", "worker_body_test.js", "worker_quota_test.js", "rawmap_menu_test.js")
foreach ($jsTest in $jsTests) {
    & $node.Source (Join-Path $here $jsTest)
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $jsTest (exit $LASTEXITCODE)"; exit 1 }
    Write-Host "[ok]   $jsTest"
}
Write-Host "all JavaScript tests passed ($($jsTests.Count))"
