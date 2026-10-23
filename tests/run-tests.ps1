# Compile and run the offline C/C++ and JavaScript tests with MSVC x64 and Node.
# Requires Visual Studio C++ Build Tools. Outputs go to tests\obj (gitignored).
#
#   tests\run-tests.ps1
#   tests\run-tests.ps1 -Doom <unpacked Vulkan exe> -DoomAlt <unpacked OpenGL exe>
#   tests\run-tests.ps1 -Skip map_published_test
#
# The complete suite uses both local images for digest-backed native fixtures.
# CI explicitly selects -SkipNativeImageTests and skips the retained composition.
# -Doom adds pinned signature, hook-fallback and global checks; -DoomAlt checks the second image in portable
# mode, accepting address shifts while requiring unique matches and layout checks.
# Exit 0 means every selected test passed; failures include their build log.
#
# occurrence_revival_retained runs the retained request-6 composition (R-274)
# over the installed game and a reflection replay. It is part of the default
# suite and fails until its inputs are configured once (tests\README.md,
# "Retained request-6 composition"):
#   tests\run-tests.ps1 -Only occurrence_revival_retained -DoomBase <DOOM>\base -ReflectionReplay <replay.tsv>
# This copies the replay to tests\local\ (untracked) and records both inputs in
# tests\local\retained-inputs.json. The test pins the game executable, the
# replay and every stock byte it reads (retained_stock_closure.h); after a
# deliberate input change, add -RecordRetainedClosure to rewrite that closure.
# CI has no game and skips it explicitly.
#
# Optional manual probes:
#   tests\obj\package_compiler_test.exe <data-root> <doom-base>
# Both resource paths are required; there is no autodiscovery. The probe skips
# invalid local packages like the runtime and prints each skipped package.
# For xinput_ordinal_test against the built proxy, see docs\contributing.md.
param([string]$Doom = "", [string]$DoomAlt = "", [string[]]$Only = @(), [string[]]$Skip = @(), [switch]$SkipNativeImageTests,
      [string]$DoomBase = "", [string]$ReflectionReplay = "", [switch]$RecordRetainedClosure)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$obj  = Join-Path $here "obj"
. (Join-Path $here '..\tools\output-lock.ps1')
$testOutputLock = Enter-OutputLock -Directory $obj -Label 'test runner'
$previousNativeVk = $env:SNAPMAPPLUS_NATIVE_TEST_VK
$previousNativeGl = $env:SNAPMAPPLUS_NATIVE_TEST_GL
try {

# Digest-backed native contracts hydrate fixture spans from the caller's local
# images. Test source never contains complete executable bodies.
$env:SNAPMAPPLUS_NATIVE_TEST_VK = $null
$env:SNAPMAPPLUS_NATIVE_TEST_GL = $null
if ($Doom) {
    if (-not (Test-Path -LiteralPath $Doom -PathType Leaf)) { throw "-Doom path not found: $Doom" }
    $env:SNAPMAPPLUS_NATIVE_TEST_VK = (Resolve-Path -LiteralPath $Doom).Path
}
if ($DoomAlt) {
    if (-not (Test-Path -LiteralPath $DoomAlt -PathType Leaf)) { throw "-DoomAlt path not found: $DoomAlt" }
    $env:SNAPMAPPLUS_NATIVE_TEST_GL = (Resolve-Path -LiteralPath $DoomAlt).Path
}

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
    @{ name = "nav_menu_test"; src = 'nav_menu_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "package_legacy_test"; src = 'package_legacy_test.c ..\src\backend\resource_catalog.c ..\src\backend\package_legacy.c ..\src\backend\package_migration.c ..\src\backend\decl_server_path.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c ..\src\backend\raw_deflate.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_migration_test"; src = 'package_migration_test.c ..\src\backend\package_migration.c ..\src\backend\decl_server_path.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\package_sources.c ..\src\backend\packages.c'; arg = "" }
    @{ name = "package_owners_test"; src = 'package_owners_test.c'; arg = "" }
    @{ name = "package_link_graph_test"; src = 'package_link_graph_test.c ..\src\backend\package_link_graph.c'; arg = ""; defs = '/DSH_PACKAGE_LINK_GRAPH_TESTING' }
    @{ name = "package_consumers_test"; src = 'package_consumers_test.c ..\src\backend\package_consumers.c'; arg = "" }
    @{ name = "package_font_test"; src = 'package_font_test.c ..\src\backend\package_font.c ..\src\backend\font_binary.c ..\src\backend\package_consumers.c'; arg = "" }
    @{ name = "package_link_pipeline_test"; src = 'package_link_pipeline_test.c ..\src\backend\package_scope.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\decl_rebind.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_linker_test"; src = 'package_linker_test.c ..\src\backend\package_provenance.c ..\src\backend\map_provenance.c ..\src\backend\map_shards.c ..\src\backend\package_scope.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_linker.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\decl_rebind.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; defs = '/DSH_PACKAGE_LINKER_TESTING'; arg = "" }
    @{ name = "model_reference_test"; src = 'model_reference_test.c ..\src\backend\model_reference.c'; arg = "" }
    @{ name = "virtual_material_native_test"; src = 'virtual_material_native_test.c ..\src\backend\virtual_material_native.c ..\src\backend\virtual_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "virtual_material_runtime_test"; src = 'virtual_material_runtime_test.c ..\src\backend\virtual_material_native.c ..\src\backend\virtual_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "virtual_material_test"; src = 'virtual_material_test.c ..\src\backend\virtual_material.c ..\src\backend\decl_native_lex.c'; arg = "" }
    @{ name = "virtual_texture_provider_test"; src = 'virtual_texture_provider_test.c ..\src\backend\virtual_texture_provider.c'; defs = '/DSH_VT_PROVIDER_TESTING'; arg = "" }
    @{ name = "virtual_texture_provenance_test"; src = 'virtual_texture_provenance_test.c ..\src\backend\resource_instances.c ..\src\backend\execution_context.c'; defs = '/DSH_VT_PROVIDER_TESTING'; arg = "" }
    @{ name = "package_virtual_pages_test"; src = 'package_virtual_pages_test.c ..\src\backend\package_virtual_pages.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "decl_native_rebind_test"; src = 'decl_native_rebind_test.c ..\src\backend\decl_native_rebind.c ..\src\backend\resource_rebind.c ..\src\backend\model_binary.c ..\src\backend\md6_binary.c ..\src\backend\decl_material.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_md6_compose.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "package_link_identity_test"; src = 'package_link_identity_test.c ..\src\backend\package_link_graph.c ..\src\backend\package_link_identity.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = ""; defs = '/DSH_PACKAGE_LINK_GRAPH_TESTING' }
    @{ name = "map_provenance_test"; src = 'map_provenance_test.c ..\src\backend\map_provenance.c ..\src\backend\map_shards.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c'; arg = "" }
    @{ name = "package_provenance_test"; src = 'package_provenance_test.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c'; defs = '/DSH_PACKAGE_PROVENANCE_TESTING'; arg = "" }
    @{ name = "package_scope_test"; src = 'package_scope_test.c ..\src\backend\package_scope.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "native_reflection_replay_test"; src = 'native_reflection_replay_test.c ..\src\backend\decl_native_registry.c ..\src\backend\native_resource_id.c'; arg = "" }
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
    @{ name = "package_native_inline_test"; src = 'package_native_inline_test.c ..\src\backend\package_native_inline.c ..\src\backend\decl_native_carrier.c ..\src\backend\decl_entity_class.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_tree.c ..\src\backend\config_json.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_native_registry.c ..\src\backend\native_resource_id.c ..\src\backend\decl_script_schema.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_compose.c ..\src\backend\decl_graph.c ..\src\backend\decl_graph_dependencies.c ..\src\backend\map_binding_sites.c ..\src\backend\decl_material.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_text.c ..\src\backend\decl_native_rebind.c ..\src\backend\resource_rebind.c ..\src\backend\model_binary.c ..\src\backend\md6_binary.c ..\src\backend\decl_md6_compose.c'; defs = '/DSH_NATIVE_INLINE_TESTING'; arg = "" }
    @{ name = "package_native_nameflow_test"; src = 'package_native_nameflow_test.c ..\src\backend\package_native_nameflow.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_editor_rows_test"; src = 'package_editor_rows_test.c ..\src\backend\package_editor_rows.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "decl_dependencies_test"; src = 'decl_dependencies_test.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_rebind_test"; src = 'decl_rebind_test.c ..\src\backend\decl_rebind.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_native_schema_test"; src = 'decl_native_schema_test.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_native_registry.c ..\src\backend\native_resource_id.c ..\src\backend\decl_script_schema.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_compose.c ..\src\backend\decl_graph.c ..\src\backend\decl_graph_dependencies.c'; arg = "" }
    @{ name = "decl_native_registry_test"; src = 'decl_native_registry_test.c ..\src\backend\decl_native_registry.c'; arg = "" }
    @{ name = "native_class_registry_test"; src = 'native_class_registry_test.c ..\src\backend\native_class_registry.c'; arg = "" }
    @{ name = "native_resource_id_test"; src = 'native_resource_id_test.c ..\src\backend\native_resource_id.c'; arg = "" }
    @{ name = "package_native_registry_test"; src = 'package_native_registry_test.c ..\src\backend\package_native_registry.c ..\src\backend\decl_native_registry.c'; arg = "" }
    @{ name = "decl_visibility_test"; src = 'decl_visibility_test.c'; arg = "" }
    @{ name = "resource_file_visibility_test"; src = 'resource_file_visibility_test.c'; defs = '/DSH_RESOURCE_FILE_VISIBILITY_TESTING'; arg = "" }
    @{ name = "resource_file_visibility_readiness_test"; src = 'resource_file_visibility_readiness_test.c'; defs = '/DSH_RESOURCE_FILE_VISIBILITY_TESTING'; arg = "" }
    @{ name = "resource_graph_prepared_test"; src = 'resource_graph_prepared_test.c ..\src\backend\resource_graph_prepared.c ..\src\backend\resource_graph.c ..\src\backend\execution_context.c ..\src\backend\decl_native_registry.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_script_schema.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\config_json.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "startup_bindings_test"; src = 'startup_bindings_test.c ..\src\backend\startup_bindings.c'; arg = "" }
    @{ name = "serialized_entities_guard_test"; src = 'serialized_entities_guard_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "resource_graph_test"; src = 'resource_graph_test.c ..\src\backend\resource_graph.c ..\src\backend\execution_context.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "resource_instances_test"; src = 'resource_instances_test.c ..\src\backend\resource_instances.c'; defs = '/DSH_RESOURCE_INSTANCES_TESTING'; arg = "" }
    @{ name = "resource_light_origins_test"; src = 'resource_light_origins_test.c ..\src\backend\resource_instances.c ..\src\backend\execution_context.c'; defs = ''; arg = "" }
    @{ name = "resource_joint_reads_test"; src = 'resource_joint_reads_test.c ..\src\backend\execution_context.c'; defs = '/DSH_RESOURCE_JOINT_READS_TESTING'; arg = "" }
    @{ name = "resource_presentation_test"; src = 'resource_presentation_test.c'; defs = ''; arg = "" }
    @{ name = "resource_metadata_reads_test"; src = 'resource_metadata_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_diagnostic_reads_test"; src = 'decl_diagnostic_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_particle_default_reads_test"; src = 'decl_particle_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_md6_release_reads_test"; src = 'decl_md6_release_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_md6_rig_reads_test"; src = 'decl_md6_rig_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_md6_engine_reads_test"; src = 'decl_md6_engine_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_md6_default_reads_test"; src = 'decl_md6_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_renderprog_cvar_reads_test"; src = 'decl_renderprog_cvar_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_renderprog_manager_reads_test"; src = 'decl_renderprog_manager_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_source_file_reads_test"; src = 'decl_source_file_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_source_file_composition_test"; src = 'decl_source_file_composition_test.c'; defs = ''; arg = "" }
    @{ name = "decl_source_preprocess_reads_test"; src = 'decl_source_preprocess_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_renderprog_backend_reads_test"; src = 'decl_renderprog_backend_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_renderprog_context_reads_test"; src = 'decl_renderprog_context_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_renderprog_default_reads_test"; src = 'decl_renderprog_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_renderprog_release_reads_test"; src = 'decl_renderprog_release_reads_test.c'; defs = ''; arg = "" }
    @{ name = "hook_rip_indirect_call_test"; src = 'hook_rip_indirect_call_test.c'; defs = ''; arg = "" }
    @{ name = "resource_renderprog_read_site_test"; src = 'resource_renderprog_read_site_test.c'; defs = ''; arg = "" }
    @{ name = "decl_material_default_reads_test"; src = 'decl_material_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "resource_model_reads_test"; src = 'resource_model_reads_test.c'; defs = '/DSH_RESOURCE_MODEL_READS_TESTING'; arg = "" }
    @{ name = "resource_late_model_reads_test"; src = 'resource_late_model_reads_test.c'; defs = ''; arg = "" }
    @{ name = "resource_material_reads_test"; src = 'resource_material_reads_test.c'; defs = '/DSH_RESOURCE_MATERIAL_READS_TESTING'; arg = "" }
    @{ name = "resource_renderprog_reads_test"; src = 'resource_renderprog_reads_test.c'; defs = '/DSH_RESOURCE_MATERIAL_READS_TESTING'; arg = "" }
    @{ name = "resource_md6_reads_test"; src = 'resource_md6_reads_test.c'; defs = ''; arg = "" }
    @{ name = "resource_parm_reads_test"; src = 'resource_parm_reads_test.c'; defs = ''; arg = "" }
    @{ name = "resource_parm_expression_reads_test"; src = 'resource_parm_expression_reads_test.c'; defs = ''; arg = "" }
    @{ name = "resource_default_reads_test"; src = 'resource_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "rig_default_test"; src = 'rig_default_test.c'; defs = ''; arg = "" }
    @{ name = "decl_default_reads_test"; src = 'decl_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_custom_default_reads_test"; src = 'decl_custom_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_string_default_reads_test"; src = 'decl_string_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_numeric_default_reads_test"; src = 'decl_numeric_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "decl_animweb_default_reads_test"; src = 'decl_animweb_default_reads_test.c'; defs = ''; arg = "" }
    @{ name = "resource_resident_test"; src = 'resource_resident_test.c ..\src\backend\resource_graph.c ..\src\backend\size_owners.c ..\src\backend\execution_context.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_SIZE_OWNERS_TESTING'; arg = "" }
    @{ name = "discrete_default_test"; src = 'discrete_default_test.c ..\src\backend\discrete_default.c ..\src\backend\execution_context.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_DISCRETE_DEFAULT_TESTING /DSH_EXECUTION_CONTEXT_TESTING /DSH_PATCH_TESTING'; arg = "" }
    @{ name = "size_owners_test"; src = 'size_owners_test.c ..\src\backend\size_owners.c ..\src\backend\execution_context.c ..\src\backend\resource_graph.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_SIZE_OWNERS_TESTING /DSH_EXECUTION_CONTEXT_TESTING'; arg = "" }
    @{ name = "image_exclusion_test"; src = 'image_exclusion_test.c ..\src\backend\image_exclusion.c'; defs = '/DSH_IMAGE_EXCLUSION_TESTING'; arg = "" }
    @{ name = "image_exclusion_layout_test"; src = 'image_exclusion_layout_test.c ..\src\backend\image_exclusion.c'; defs = '/DSH_IMAGE_EXCLUSION_TESTING'; arg = "" }
    @{ name = "resource_graph_native_test"; src = 'resource_graph_native_test.c ..\src\backend\resource_graph.c ..\src\backend\resource_instances.c ..\src\backend\resource_parm_reads.c ..\src\backend\resource_parm_expression_reads.c ..\src\backend\execution_context.c ..\src\backend\package_font.c ..\src\backend\font_binary.c ..\src\backend\package_consumers.c ..\src\backend\swf_text_font.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "package_archive_test"; src = 'package_archive_test.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c ..\src\backend\raw_deflate.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\map_package.c ..\src\backend\decl_server_path.c ..\src\backend\package_migration.c ..\src\backend\package_legacy.c ..\src\backend\map_shards.c ..\src\backend\map_package_share.c'; arg = "" }
    @{ name = "package_descriptor_test"; src = 'package_descriptor_test.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "decl_compose_test"; src = 'decl_compose_test.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "decl_composition_classes_test"; src = 'decl_composition_classes_test.c ..\src\backend\decl_compose.c ..\src\backend\decl_entity_class.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c'; arg = "" }
    @{ name = "package_sources_test"; src = 'package_sources_test.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_cache_sweep_test"; src = 'package_cache_sweep_test.c ..\src\backend\package_cache.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_cache_test"; src = 'package_cache_test.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\audio_bank_codec.c ..\src\backend\audio_hirc.c ..\src\backend\audio_banks.c'; arg = "" }
    @{ name = "package_strings_test"; src = 'package_strings_test.c ..\src\backend\package_strings.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_compiler_test"; src = 'package_compiler_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_environment_test"; src = 'package_environment_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_context_test"; src = 'package_context_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_runtime_context_test"; src = 'package_runtime_context_test.c ..\src\backend\resource_instances.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_md6_compose.c ..\src\backend\decl_block_compose.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_graph.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_producers_test"; src = 'package_producers_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_source_view_test"; src = 'package_source_view_test.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_decl_families_test"; src = 'package_decl_families_test.c ..\src\backend\decl_graph_compose.c ..\src\backend\decl_md6_compose.c ..\src\backend\decl_block_compose.c ..\src\backend\decl_native_lex.c ..\src\backend\decl_graph.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "package_usage_test"; src = 'package_usage_test.c ..\src\backend\resource_graph.c ..\src\backend\execution_context.c ..\src\backend\package_usage.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "package_source_graph_test"; src = 'package_source_graph_test.c ..\src\backend\resource_graph.c ..\src\backend\execution_context.c ..\src\backend\package_usage.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c ..\src\backend\package_source_graph.c ..\src\backend\package_variants.c ..\src\backend\decl_native_registry.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_script_schema.c ..\src\backend\decl_dependencies.c ..\src\backend\decl_json_dependencies.c ..\src\backend\decl_graph.c ..\src\backend\decl_graph_dependencies.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_PACKAGE_LIBRARY_TESTING'; arg = "" }
    @{ name = "package_scale_test"; src = 'package_scale_test.c ..\src\backend\resource_graph.c ..\src\backend\execution_context.c ..\src\backend\package_usage.c ..\src\backend\package_compiler.c ..\src\backend\package_hud.c ..\src\backend\decl_entity_class.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c ..\src\backend\decl_compose.c ..\src\backend\decl_tree.c ..\src\backend\decl_polymorphic.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c ..\src\backend\map_embed.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c'; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
    @{ name = "map_render_test"; src = 'map_render_test.c ..\src\backend\map_render.c'; arg = "" }
    @{ name = "map_render_editor_test"; src = 'map_render_editor_test.c ..\src\backend\map_render.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "map_render_native_test"; src = 'map_render_native_test.c ..\src\backend\map_render.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_nav_test"; src = 'grid_room_nav_test.c ..\src\backend\grid_room_nav.c ..\src\backend\grid_room.c ..\src\backend\aas_edit.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "grid_room_asset_test"; src = 'grid_room_asset_test.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_test"; src = 'grid_room_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_editor_test"; src = 'grid_room_editor_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_native_test"; src = 'grid_room_native_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\grid_room_edit.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "module_ribbon_native_test"; src = 'module_ribbon_native_test.c'; arg = "" }
    @{ name = "grid_room_snap_test"; src = 'grid_room_snap_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "spawn_keys_test"; src = 'spawn_keys_test.c ..\src\backend\spawn_keys.c ..\src\backend\package_keys.c ..\src\backend\decl_tree.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_SPAWN_KEYS_TESTING'; arg = "" }
    @{ name = "package_spawn_registry_test"; src = 'package_spawn_registry_test.c ..\src\backend\package_spawn_registry.c ..\src\backend\package_keys.c ..\src\backend\decl_tree.c'; arg = "" }
    @{ name = "snapshot_templates_test"; src = 'snapshot_templates_test.c ..\src\backend\snapshot_templates.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_SNAPSHOT_TEMPLATES_TESTING'; arg = "" }
    @{ name = "snap_type_cache_test"; src = 'snap_type_cache_test.c ..\src\backend\snap_type_cache.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_SNAP_TYPE_CACHE_TESTING'; arg = "" }
    @{ name = "native_decl_eval_test"; src = 'native_decl_eval_test.c ..\src\backend\native_decl_eval.c'; defs = '/DSH_NATIVE_DECL_EVAL_TESTING'; arg = "" }
    @{ name = "prepare_worker_test"; src = 'prepare_worker_test.c'; arg = "" }
    @{ name = "key_inspector_test"; src = 'key_inspector_test.c ..\src\backend\key_inspector.c ..\src\backend\package_keys.c ..\src\backend\decl_tree.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_KEY_INSPECTOR_TESTING'; arg = "" }
    @{ name = "nav_heap_test"; src = 'nav_heap_test.c'; arg = "" }
    @{ name = "nav_heap_hook_test"; src = 'nav_heap_hook_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "patch_test"; src = 'patch_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "cvar_unlock_test"; src = 'cvar_unlock_test.c ..\src\backend\cvar_unlock.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "map_capacity_test"; src = 'map_capacity_test.c ..\src\backend\map_capacity.c ..\src\backend\map_shards.c'; defs = ''; arg = "" }
    @{ name = "map_reference_audit_test"; src = 'map_reference_audit_test.c ..\src\backend\map_reference_audit.c ..\src\backend\config_json.c'; defs = ''; arg = "" }
    @{ name = "map_payload_test"; src = 'map_payload_test.c ..\src\backend\map_payload.c ..\src\backend\map_capacity.c ..\src\backend\map_shards.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
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
    @{ name = "rawmap_paths_test";  src = 'rawmap_paths_test.c ..\src\backend\package_document.c ..\src\backend\map_provenance.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\map_capacity.c ..\src\backend\map_reference_audit.c ..\src\backend\hook.c ..\src\backend\patch.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_RAWMAP_TESTING /DSH_CONFIG_TESTING /DSH_PATCH_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "config_json_test";   src = 'config_json_test.c ..\src\backend\config_json.c';         arg = "" }
    @{ name = "native_json_sites_test"; src = 'native_json_sites_test.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "map_binding_sites_test"; src = 'map_binding_sites_test.c ..\src\backend\map_binding_sites.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_bundle_test"; src = 'package_bundle_test.c ..\src\backend\package_bundle.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c ..\src\backend\raw_deflate.c ..\src\backend\package_sources.c ..\src\backend\packages.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "package_shader_test"; src = 'package_shader_test.c ..\src\backend\package_shader.c'; arg = "" }
    @{ name = "iface_config_test";  src = 'iface_config_test.c ..\src\common\snapmap_plus_iface.c';   arg = "" }
    @{ name = "editor_delete_test"; src = 'editor_delete_test.c ..\src\backend\editor_delete.c'; defs = '/DSH_EDITOR_DELETE_TESTING'; arg = "" }
    @{ name = "prefab_anchor_test"; src = 'prefab_anchor_test.c'; arg = "" }
    @{ name = "config_test";        src = 'config_test.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_test"; src = 'user_overrides_test.c ..\src\backend\user_overrides.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING /DSH_USER_OVERRIDES_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_contract_test"; src = 'user_overrides_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "decl_server_test"; src = 'decl_server_test.c ..\src\backend\decl_server.c ..\src\backend\engine_dialog.c ..\src\backend\boot_phases.c ..\src\backend\packages.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c'; defs = '/DSH_DECL_SERVER_TESTING'; arg = "" }
    @{ name = "decl_text_order_test"; src = 'decl_text_order_test.c ..\src\backend\decl_text.c'; arg = "" }
    @{ name = "overrides_internal_test"; src = 'overrides_internal_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "overrides_source_file_dispatch_test"; src = 'overrides_source_file_dispatch_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "decl_server_contract_test"; src = 'decl_server_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "package_role_test"; src = 'package_role_test.c ..\src\backend\package_role.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_PACKAGE_ROLE_TESTING'; arg = "" }
    @{ name = "palette_refresh_test"; src = 'palette_refresh_test.c ..\src\backend\palette_refresh.c'; defs = '/DSH_PALETTE_REFRESH_TESTING'; arg = "" }
    @{ name = "process_heap_scope_test"; src = 'process_heap_scope_test.c'; arg = "" }
    @{ name = "render_scope_test"; src = 'render_scope_test.c'; arg = "" }
    @{ name = "engine_dialog_test"; src = 'engine_dialog_test.c ..\src\backend\engine_dialog.c ..\src\backend\boot_phases.c'; defs = '/DSH_ENGINE_DIALOG_TESTING'; arg = "" }
    # R-400: the boot phases line and the first registration's placement.
    @{ name = "boot_phases_test"; src = 'boot_phases_test.c ..\src\backend\boot_phases.c'; defs = '/DSH_BOOT_PHASES_TESTING'; arg = "" }
    @{ name = "palette_refresh_contract_test"; src = 'palette_refresh_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "resource_catalog_test"; src = 'resource_catalog_test.c ..\src\backend\resource_catalog.c ..\src\backend\raw_deflate.c'; defs = '/DSH_RAW_DEFLATE_TESTING'; arg = "" }
    @{ name = "raw_deflate_test"; src = 'raw_deflate_test.c ..\src\backend\raw_deflate.c ..\src\backend\raw_deflate_encode.c ..\src\backend\resource_catalog.c'; defs = '/DSH_RAW_DEFLATE_TESTING'; arg = "" }
    @{ name = "packages_test"; src = 'packages_test.c ..\src\backend\packages.c'; defs = '/DSH_PACKAGES_TESTING'; arg = "" }
    @{ name = "map_package_test"; src = 'map_package_test.c ..\src\backend\resource_catalog.c ..\src\backend\map_package.c ..\src\backend\map_package_share.c ..\src\backend\decl_server_path.c ..\src\backend\package_migration.c ..\src\backend\package_legacy.c ..\src\backend\map_shards.c ..\src\backend\packages.c ..\src\backend\raw_deflate.c ..\src\backend\raw_deflate_encode.c ..\src\backend\package_archive.c ..\src\backend\package_sources.c ..\src\backend\package_descriptor.c ..\src\backend\config_json.c'; defs = '/DSH_MAP_PACKAGE_TESTING'; arg = "" }
    @{ name = "navmesh_test"; src = 'navmesh_test.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING'; arg = "" }
    @{ name = "nav_regions_test"; src = 'nav_regions_test.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "aas_edit_test"; src = 'aas_edit_test.c ..\src\backend\aas_edit.c'; arg = "" }
    @{ name = "nav_geometry_test"; src = 'nav_geometry_test.c ..\src\backend\nav_geometry.c'; arg = "" }
    @{ name = "aas_augment_test"; src = 'aas_augment_test.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\aas_edit.c ..\src\backend\nav_traversal.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING /DSH_AUG_TESTING /DSH_TRAV_TESTING'; arg = "" }
    @{ name = "nav_bake_test"; src = 'nav_bake_test.c ..\src\backend\resource_graph.c ..\src\backend\execution_context.c ..\src\backend\perf.c ..\src\backend\nav_bake.c ..\src\backend\nav_regions.c ..\src\backend\aas_edit.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\nav_traversal.c ..\src\backend\map_shards.c'; defs = '/DSH_NAV_BAKE_TESTING /DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
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
    @{ name = "audio_originals_test"; src = 'audio_originals_test.c'; defs = '/DSH_AUDIO_ORIGINALS_TESTING'; arg = "" }
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
    $tests += @{ name = "package_role_image_test"; src = 'package_role_test.c ..\src\backend\package_role.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_PACKAGE_ROLE_TESTING'; arg = @($da, "vk") }
    $tests += @{ name = "host_renderer_test"; src = 'host_renderer_test.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = @($da, "1") }
    $tests += @{ name = "cvar_unlock_image_test"; src = 'cvar_unlock_test.c ..\src\backend\cvar_unlock.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = $da }
    $tests += @{ name = "discrete_default_image_test"; src = 'discrete_default_test.c ..\src\backend\discrete_default.c ..\src\backend\execution_context.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_DISCRETE_DEFAULT_TESTING /DSH_EXECUTION_CONTEXT_TESTING /DSH_PATCH_TESTING'; arg = @($da, "1") }
    $tests += @{ name = "resource_joint_reads_image_test"; src = 'resource_joint_reads_test.c ..\src\backend\execution_context.c'; defs = '/DSH_RESOURCE_JOINT_READS_TESTING'; arg = @($da, "1") }
    $tests += @{ name = "resource_model_reads_image_test"; src = 'resource_model_reads_test.c'; defs = '/DSH_RESOURCE_MODEL_READS_TESTING'; arg = @($da, "1") }
    $tests += @{ name = "resource_late_model_reads_image_test"; src = 'resource_late_model_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "resource_material_reads_image_test"; src = 'resource_material_reads_test.c'; defs = '/DSH_RESOURCE_MATERIAL_READS_TESTING'; arg = @($da, "1") }
    $tests += @{ name = "resource_renderprog_reads_image_test"; src = 'resource_renderprog_reads_test.c'; defs = '/DSH_RESOURCE_MATERIAL_READS_TESTING'; arg = @($da, "1") }
    $tests += @{ name = "resource_md6_reads_image_test"; src = 'resource_md6_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "resource_metadata_reads_image_test"; src = 'resource_metadata_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_diagnostic_reads_image_test"; src = 'decl_diagnostic_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_particle_default_reads_image_test"; src = 'decl_particle_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_md6_release_reads_image_test"; src = 'decl_md6_release_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_md6_rig_reads_image_test"; src = 'decl_md6_rig_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_md6_engine_reads_image_test"; src = 'decl_md6_engine_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_md6_default_reads_image_test"; src = 'decl_md6_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_renderprog_cvar_reads_image_test"; src = 'decl_renderprog_cvar_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_renderprog_manager_reads_image_test"; src = 'decl_renderprog_manager_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "resource_parm_expression_reads_image_test"; src = 'resource_parm_expression_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_source_file_reads_image_test"; src = 'decl_source_file_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_source_file_composition_image_test"; src = 'decl_source_file_composition_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_source_preprocess_reads_image_test"; src = 'decl_source_preprocess_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_renderprog_backend_reads_image_test"; src = 'decl_renderprog_backend_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_renderprog_context_reads_image_test"; src = 'decl_renderprog_context_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_renderprog_default_reads_image_test"; src = 'decl_renderprog_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_renderprog_release_reads_image_test"; src = 'decl_renderprog_release_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_material_default_reads_image_test"; src = 'decl_material_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "rig_default_image_test"; src = 'rig_default_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_default_reads_image_test"; src = 'decl_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_custom_default_reads_image_test"; src = 'decl_custom_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_string_default_reads_image_test"; src = 'decl_string_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_numeric_default_reads_image_test"; src = 'decl_numeric_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "decl_animweb_default_reads_image_test"; src = 'decl_animweb_default_reads_test.c'; defs = ''; arg = @($da, "1") }
    $tests += @{ name = "hooktol_test"; src = 'hooktol_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
    $tests += @{ name = "globals_test"; src = 'globals_test.c ..\src\backend\native_class_registry.c ..\src\backend\audio_bank_commands.c ..\src\backend\audio_limits_native.c ..\src\backend\audio_environment_native.c ..\src\backend\audio_state_native.c ..\src\backend\audio_parameter_native.c ..\src\backend\audio_subscription_native.c ..\src\backend\audio_switch_native.c ..\src\backend\audio_bus_native.c ..\src\backend\audio_bank_gate.c map_published_bindings_stubs.c ..\src\backend\resource_file_visibility.c ..\src\backend\swf_text_font.c ..\src\backend\image_exclusion.c ..\src\backend\virtual_texture_provider.c ..\src\backend\resource_resident.c ..\src\backend\size_owners.c ..\src\backend\execution_context.c ..\src\backend\resource_catalog.c ..\src\backend\resource_graph.c ..\src\backend\raw_deflate.c ..\src\backend\map_source.c ..\src\backend\map_native.c ..\src\backend\map_session_json.c ..\src\backend\config_json.c ..\src\backend\map_published.c ..\src\backend\map_transition.c ..\src\backend\editor_transition.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_native_registry.c ..\src\backend\native_resource_id.c ..\src\backend\decl_script_schema.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = $da }
}
# Check the second executable without requiring the pinned image's addresses.
if ($DoomAlt) {
    if (-not (Test-Path $DoomAlt)) { throw "-DoomAlt path not found: $DoomAlt" }
    $alt = (Resolve-Path $DoomAlt).Path
    $tests += @{ name = "sig_test_alt"; src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; arg = @($alt, "portable") }
    $tests += @{ name = "package_role_image_test_alt"; src = 'package_role_test.c ..\src\backend\package_role.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_PACKAGE_ROLE_TESTING'; arg = @($alt, "gl") }
    $tests += @{ name = "host_renderer_test_alt"; src = 'host_renderer_test.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = @($alt, "0") }
    $tests += @{ name = "cvar_unlock_image_test_alt"; src = 'cvar_unlock_test.c ..\src\backend\cvar_unlock.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = $alt }
    $tests += @{ name = "discrete_default_image_test_alt"; src = 'discrete_default_test.c ..\src\backend\discrete_default.c ..\src\backend\execution_context.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_DISCRETE_DEFAULT_TESTING /DSH_EXECUTION_CONTEXT_TESTING /DSH_PATCH_TESTING'; arg = @($alt, "0") }
    $tests += @{ name = "resource_joint_reads_image_test_alt"; src = 'resource_joint_reads_test.c ..\src\backend\execution_context.c'; defs = '/DSH_RESOURCE_JOINT_READS_TESTING'; arg = @($alt, "0") }
    $tests += @{ name = "resource_model_reads_image_test_alt"; src = 'resource_model_reads_test.c'; defs = '/DSH_RESOURCE_MODEL_READS_TESTING'; arg = @($alt, "0") }
    $tests += @{ name = "resource_late_model_reads_image_test_alt"; src = 'resource_late_model_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "resource_material_reads_image_test_alt"; src = 'resource_material_reads_test.c'; defs = '/DSH_RESOURCE_MATERIAL_READS_TESTING'; arg = @($alt, "0") }
    $tests += @{ name = "resource_renderprog_reads_image_test_alt"; src = 'resource_renderprog_reads_test.c'; defs = '/DSH_RESOURCE_MATERIAL_READS_TESTING'; arg = @($alt, "0") }
    $tests += @{ name = "resource_md6_reads_image_test_alt"; src = 'resource_md6_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "resource_metadata_reads_image_test_alt"; src = 'resource_metadata_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_diagnostic_reads_image_test_alt"; src = 'decl_diagnostic_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_particle_default_reads_image_test_alt"; src = 'decl_particle_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_md6_release_reads_image_test_alt"; src = 'decl_md6_release_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_md6_rig_reads_image_test_alt"; src = 'decl_md6_rig_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_md6_engine_reads_image_test_alt"; src = 'decl_md6_engine_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_md6_default_reads_image_test_alt"; src = 'decl_md6_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_renderprog_cvar_reads_image_test_alt"; src = 'decl_renderprog_cvar_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_renderprog_manager_reads_image_test_alt"; src = 'decl_renderprog_manager_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "resource_parm_expression_reads_image_test_alt"; src = 'resource_parm_expression_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_source_file_reads_image_test_alt"; src = 'decl_source_file_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_source_file_composition_image_test_alt"; src = 'decl_source_file_composition_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_source_preprocess_reads_image_test_alt"; src = 'decl_source_preprocess_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_renderprog_backend_reads_image_test_alt"; src = 'decl_renderprog_backend_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_renderprog_context_reads_image_test_alt"; src = 'decl_renderprog_context_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_renderprog_default_reads_image_test_alt"; src = 'decl_renderprog_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_renderprog_release_reads_image_test_alt"; src = 'decl_renderprog_release_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_material_default_reads_image_test_alt"; src = 'decl_material_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "rig_default_image_test_alt"; src = 'rig_default_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_default_reads_image_test_alt"; src = 'decl_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_custom_default_reads_image_test_alt"; src = 'decl_custom_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_string_default_reads_image_test_alt"; src = 'decl_string_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_numeric_default_reads_image_test_alt"; src = 'decl_numeric_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "decl_animweb_default_reads_image_test_alt"; src = 'decl_animweb_default_reads_test.c'; defs = ''; arg = @($alt, "0") }
    $tests += @{ name = "globals_test_alt"; src = 'globals_test.c ..\src\backend\native_class_registry.c ..\src\backend\audio_bank_commands.c ..\src\backend\audio_limits_native.c ..\src\backend\audio_environment_native.c ..\src\backend\audio_state_native.c ..\src\backend\audio_parameter_native.c ..\src\backend\audio_subscription_native.c ..\src\backend\audio_switch_native.c ..\src\backend\audio_bus_native.c ..\src\backend\audio_bank_gate.c map_published_bindings_stubs.c ..\src\backend\resource_file_visibility.c ..\src\backend\swf_text_font.c ..\src\backend\image_exclusion.c ..\src\backend\virtual_texture_provider.c ..\src\backend\resource_resident.c ..\src\backend\size_owners.c ..\src\backend\execution_context.c ..\src\backend\resource_catalog.c ..\src\backend\resource_graph.c ..\src\backend\raw_deflate.c ..\src\backend\map_source.c ..\src\backend\map_native.c ..\src\backend\map_session_json.c ..\src\backend\config_json.c ..\src\backend\map_published.c ..\src\backend\map_transition.c ..\src\backend\editor_transition.c ..\src\backend\decl_native_schema.c ..\src\backend\decl_native_registry.c ..\src\backend\native_resource_id.c ..\src\backend\decl_script_schema.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = @($alt, "portable") }
}

# The real decl_server row lineage bound to the editor's steer.
$tests += @{ name = "package_editor_native_rows_test"; src = (($tests | Where-Object { $_.name -eq 'decl_server_test' }).src -replace '^decl_server_test.c ', 'package_editor_native_rows_test.c '); defs = '/DSH_DECL_SERVER_TESTING'; arg = "" }
# A prefab admission through decl_server with the real resident layer (M115).
$tests += @{ name = "occurrence_revival_test"; src = (($tests | Where-Object { $_.name -eq 'decl_server_test' }).src -replace '^decl_server_test.c ', 'occurrence_revival_test.c '); defs = '/DSH_DECL_SERVER_TESTING /DSH_PACKAGE_SHADER_RECEIPT_TESTING'; arg = "" }
$compiledConsumers = @("overrides_source_file_dispatch_test", "package_runtime_context_test", "weapon_hud_test", "package_requirements_test", "strids_packages_test", "override_packages_test", "overrides_internal_test", "decl_server_test", "package_editor_native_rows_test")
$compiledConsumers += "occurrence_revival_test"
$compiledConsumers += @("audio_originals_test", "package_native_preparation_runtime_test")
$tests += @{ name = "package_native_preparation_runtime_test"; src = 'package_native_preparation_runtime_test.c'; defs = '/DSH_PACKAGE_RUNTIME_NATIVE_TESTING'; arg = "" }
$compilerSources = @("grid_room_asset.c", "grid_room_nav.c", "aas_edit.c", "grid_room.c", "grid_room_resources.c", "grid_room_decl.c", "package_runtime.c", "package_cache.c", "resource_graph.c", "package_usage.c", "package_source_graph.c", "package_variants.c", "decl_native_registry.c", "decl_dependencies.c", "decl_json_dependencies.c", "decl_graph.c", "decl_graph_compose.c", "decl_md6_compose.c", "decl_block_compose.c", "decl_native_lex.c", "md6_binary.c", "model_binary.c", "decl_graph_dependencies.c", "package_compiler.c", "decl_entity_class.c", "decl_native_schema.c", "decl_script_schema.c", "package_sources.c", "packages.c", "package_descriptor.c", "config_json.c", "decl_compose.c", "decl_tree.c", "decl_polymorphic.c", "decl_server_path.c", "decl_text.c", "resource_catalog.c", "raw_deflate.c")
$compilerSources += @("decl_native_lex.c", "decl_material.c", "decl_material_compose.c", "package_material.c", "package_keys.c", "package_spawn_registry.c")
$compilerSources += @("audio_banks.c", "audio_packages.c", "audio_originals.c", "package_audio.c", "package_sound.c")
$compilerSources += @("virtual_material_runtime.c", "virtual_material_native.c", "virtual_material.c", "package_virtual_material.c")
$compilerSources += @("package_link_inputs.c", "package_library.c", "package_source_link.c", "package_linker.c", "package_link_graph.c", "package_link_identity.c", "package_provenance.c", "package_scope.c", "package_native_names.c")
$compilerSources += "package_native_assets.c"
$compilerSources += "package_warm.c"
$compilerSources += "package_hud.c"
$compilerSources += @("map_binding_sites.c", "package_bundle.c", "package_archive.c", "raw_deflate_encode.c", "package_shader.c", "package_strings.c")
$compilerSources += @("package_occurrence.c", "map_provenance.c", "map_shards.c")
$compilerSources += @("package_map_uses.c", "map_embed_uses.c")
$compilerSources += "native_class_registry.c"
$compilerSources += @("size_owners.c", "execution_context.c")
$compilerSources += @("decl_native_carrier.c", "package_native_inline.c", "package_editor_rows.c")
$compilerSources += "boot_phases.c"
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
$tests += @{ name = "package_shader_link_test"; src = $shaderLinkSources; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_PACKAGE_SOURCE_LINK_TESTING'; arg = "" }
$tests += @{ name = "package_shader_receipt_test"; src = 'package_shader_receipt_test.c ..\src\backend\package_shader_receipt.c'; defs = '/DSH_PACKAGE_SHADER_RECEIPT_TESTING'; arg = "" }
$occurrenceSources = ($tests | Where-Object { $_.name -eq 'package_bundle_test' }).src -replace '^package_bundle_test.c ', 'package_occurrence_test.c ..\src\backend\package_occurrence.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c '
$tests += @{ name = "package_occurrence_test"; src = $occurrenceSources; arg = "" }
$tests += @{ name = "package_editor_test"; src = ($occurrenceSources -replace '^package_occurrence_test.c ', 'package_editor_test.c '); arg = "" }
$prefabSources = 'prefab_test.c ..\src\backend\prefab.c ..\src\backend\package_occurrence.c ..\src\backend\package_bundle.c ..\src\backend\map_provenance.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c '
$prefabSources += ($tests | Where-Object { $_.name -eq 'package_archive_test' }).src -replace '^package_archive_test.c ', ''
$tests += @{ name = "prefab_test"; src = $prefabSources; arg = "" }
foreach ($t in $tests) {
    if ('..\src\backend\package_native_inline.c' -in ($t.src -split ' ') -and
        '..\src\backend\package_native_nameflow.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\package_native_nameflow.c'
    }
    if ($t.name -eq 'package_runtime_context_test') {
        $t.src += ' ..\src\backend\spawn_keys.c'
        $t.defs += ' /DSH_SPAWN_KEYS_TESTING'
        # Activation and recovery bridge the real state-machine web hook API.
        $t.src += ' ..\src\backend\fsm_web.c'
        $t.defs += ' /DSH_FSM_WEB_TESTING'
        # The label base fixture serves text through the real string injector (the runtime
        # tests otherwise replace its reservations with the test seam).
        $t.src += ' ..\src\backend\strids.c'
        $t.defs += ' /DSH_STRIDS_TESTING'
        $t.libs = 'shell32.lib'
    }
    if ('..\src\backend\package_library.c' -in ($t.src -split ' ') -or $t.name -in $compiledConsumers -or
        $t.name -in @('package_source_graph_test', 'package_shader_link_test')) { $t.src += ' ..\src\backend\package_consumers.c' }
    if ('..\src\backend\package_source_graph.c' -in ($t.src -split ' ')) { $t.src += ' ..\src\backend\package_variants.c ..\src\backend\map_binding_sites.c ..\src\backend\package_shader.c ..\src\backend\package_keys.c ..\src\backend\package_spawn_registry.c ..\src\backend\native_class_registry.c' }
    if ($t.name -in @("map_package_test", "package_archive_test", "package_scale_test")) { $t.src += ' ..\src\backend\package_bundle.c' }
    if ($t.name -in @("map_package_test", "package_archive_test", "prefab_test")) { $t.src += ' ..\src\backend\package_cache.c' }
    if ($t.name -in @("map_package_test", "package_archive_test")) { $t.src += ' ..\src\backend\map_provenance.c ..\src\backend\package_provenance.c ..\src\backend\package_scope.c' }
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
        if ($t.name -notin @("package_runtime_context_test", "strids_packages_test", "decl_server_test", "package_editor_native_rows_test", "occurrence_revival_test") -and
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
    # The source graph models a placed copy's JSON state as the map reader loads it.
    if ('..\src\backend\package_source_graph.c' -in ($t.src -split ' ') -and
        '..\src\backend\decl_native_carrier.c' -notin ($t.src -split ' ')) { $t.src += ' ..\src\backend\decl_native_carrier.c' }
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
    # Source receipts and cached provider payloads share the same origin store.
    if ('..\src\backend\overrides.c' -in ($t.src -split ' ')) {
        foreach ($source in @('resource_instances.c', 'execution_context.c')) {
            $relative = '..\src\backend\' + $source
            if ($relative -notin ($t.src -split ' ')) { $t.src += ' ' + $relative }
        }
    }
    # The virtual material runtime builds private page providers.
    if (($t.name -in @('virtual_material_runtime_test', 'package_virtual_material_test', 'package_source_graph_test') -or
        '..\src\backend\virtual_material_runtime.c' -in ($t.src -split ' ')) -and
        '..\src\backend\virtual_texture_provider.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\virtual_texture_provider.c'
    }
    if ('..\src\backend\virtual_texture_provider.c' -in ($t.src -split ' ')) {
        foreach ($source in @('resource_instances.c', 'execution_context.c')) {
            $relative = '..\src\backend\' + $source
            if ($relative -notin ($t.src -split ' ')) { $t.src += ' ' + $relative }
        }
    }
}

$localizationSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'package_localization_test.c '
$localizationSources += ' ..\src\backend\strids.c'
$tests += @{ name = "package_localization_test"; src = $localizationSources; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_STRIDS_TESTING'; libs = 'shell32.lib'; arg = "" }

$labelSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'strids_labels_test.c '
$labelSources += ' ..\src\backend\strids.c'
$tests += @{ name = "strids_labels_test"; src = $labelSources; defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_STRIDS_TESTING'; libs = 'shell32.lib'; arg = "" }

$reverseRootSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'package_reverse_roots_test.c '
$tests += @{ name = "package_reverse_roots_test"; src = ($reverseRootSources + ' ..\src\backend\image_exclusion.c'); defs = '/DSH_RESOURCE_GRAPH_TESTING /DSH_PACKAGE_SOURCE_LINK_TESTING /DSH_IMAGE_EXCLUSION_TESTING'; arg = "" }
$fsmLinkSources = $reverseRootSources -replace '^package_reverse_roots_test.c ', 'package_fsm_link_test.c '
$tests += @{ name = "package_fsm_link_test"; src = $fsmLinkSources; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }

$mapUsesSources = ($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src -replace '^package_source_graph_test.c ', 'package_map_uses_test.c '
$mapUsesSources += ' ..\src\backend\package_map_uses.c ..\src\backend\map_embed.c ..\src\backend\package_bundle.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c'
$tests += @{ name = "package_map_uses_test"; src = $mapUsesSources; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = "" }
# R-390: engine-written map forms through the product's local map open, with the
# retained reflection replay, the installed script module and catalog. The
# fixtures run with the retained inputs; the corpus run is manual (-Only) and
# reads tests\local\map-form-corpus.txt, one rawmap JSON or map.decl per line.
$sweepSources = ($mapUsesSources -replace '^package_map_uses_test.c ', 'map_form_sweep_test.c ') + ' ..\src\backend\decl_script_native.c'
$tests += @{ name = "map_form_sweep_test"; src = $sweepSources; defs = '/DSH_RESOURCE_GRAPH_TESTING'; arg = ""; sweep = 'fixtures' }
$tests += @{ name = "map_form_sweep_corpus"; sweep = 'corpus'; manual = $true }
$sweepEntry = $tests | Where-Object { $_.name -eq 'map_form_sweep_test' } | Select-Object -First 1
# R-249: the source graph test drives production map use (projection controls).
($tests | Where-Object { $_.name -eq 'package_source_graph_test' }).src += ' ..\src\backend\package_map_uses.c ..\src\backend\package_bundle.c ..\src\backend\package_archive.c ..\src\backend\raw_deflate_encode.c'
# package_source_graph.c reads a map's placed play-copy names (sg_walk_local)
# through package_native_inline.c, so every test linking it links that reader
# and the site locations its map-local rewrite uses.
$nativeInlineSources = ($tests | Where-Object { $_.name -eq 'package_native_inline_test' }).src -split ' ' | Select-Object -Skip 1
foreach ($t in $tests) {
    if ('..\src\backend\package_source_graph.c' -notin ($t.src -split ' ')) { continue }
    foreach ($f in $nativeInlineSources) { if ($f -notin ($t.src -split ' ')) { $t.src += " $f" } }
}
foreach ($t in $tests) {
    if ('..\src\backend\map_embed.c' -in ($t.src -split ' ') -and
        '..\src\backend\map_embed_uses.c' -notin ($t.src -split ' ')) {
        $t.src += ' ..\src\backend\map_embed_uses.c'
    }
}

# The production map-document chain over historical-writer carriers (M130 R-325): the runtime test's complete
# sources, defines and libraries plus the real map package reader, migration, document bind and save-route packing.
$documentEntry = $tests | Where-Object { $_.name -eq 'package_runtime_context_test' } | Select-Object -First 1
$documentSources = $documentEntry.src -replace '^package_runtime_context_test.c ', 'package_document_test.c '
foreach ($source in @('map_package.c', 'map_package_share.c', 'package_legacy.c', 'package_migration.c', 'package_document.c', 'package_installed_open.c', 'map_embed.c')) {
    $relative = '..\src\backend\' + $source
    if ($relative -notin ($documentSources -split ' ')) { $documentSources += ' ' + $relative }
}
$tests += @{ name = "package_document_test"; src = $documentSources; defs = $documentEntry.defs; libs = $documentEntry.libs; arg = "" }

# The published-map source contract (M130): the launch bridge, native selection and cached source, retained
# request, consent and installation over the same chain, with the lobby-session projection and the map's
# prepared resource graph. The test includes rawmap.c, map_native.c, map_source.c and map_published.c itself.
$publishedSources = $documentSources -replace '^package_document_test.c ', 'published_source_test.c '
foreach ($source in @('map_session_json.c', 'resource_graph_prepared.c', 'map_capacity.c', 'map_reference_audit.c', 'map_shards.c')) {
    $relative = '..\src\backend\' + $source
    if ($relative -notin ($publishedSources -split ' ')) { $publishedSources += ' ' + $relative }
}
$tests += @{ name = "published_source_test"; src = $publishedSources; defs = $documentEntry.defs; libs = $documentEntry.libs; arg = "" }

# The retained composition reuses occurrence_revival_test.exe with pinned game
# inputs; it is selected and skipped like any other test.
$revivalEntry = $tests | Where-Object { $_.name -eq 'occurrence_revival_test' } | Select-Object -First 1
$tests += @{ name = "occurrence_revival_retained"; retained = $true }

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

# These fixtures read local executable spans and check their SHA256 identities;
# complete game function bodies are never distributed as test data. List the
# fixture translation units so the paired image variants inherit the same rule.
$nativeFixtureUnits = @(
    'decl_default_reads_test.c', 'decl_custom_default_reads_test.c',
    'decl_diagnostic_reads_test.c', 'decl_string_default_reads_test.c',
    'decl_numeric_default_reads_test.c', 'decl_animweb_default_reads_test.c',
    'decl_material_default_reads_test.c', 'decl_particle_default_reads_test.c',
    'decl_md6_default_reads_test.c', 'decl_md6_release_reads_test.c',
    'decl_md6_rig_reads_test.c', 'decl_md6_engine_reads_test.c',
    'decl_renderprog_cvar_reads_test.c', 'decl_renderprog_context_reads_test.c',
    'decl_renderprog_manager_reads_test.c', 'decl_renderprog_backend_reads_test.c',
    'decl_renderprog_default_reads_test.c', 'decl_renderprog_release_reads_test.c',
    'decl_source_file_reads_test.c', 'decl_source_file_composition_test.c',
    'decl_source_preprocess_reads_test.c', 'resource_parm_expression_reads_test.c',
    'resource_late_model_reads_test.c', 'resource_material_reads_test.c',
    'resource_renderprog_reads_test.c', 'resource_metadata_reads_test.c',
    'resource_presentation_test.c', 'image_exclusion_layout_test.c'
)
$nativeFixtures = @($tests | Where-Object { ($_.src -split ' ')[0] -in $nativeFixtureUnits })
if ($SkipNativeImageTests) {
    if (@($nativeFixtures | Where-Object { $_.name -in $Only }).Count) {
        throw 'An explicitly selected native fixture cannot also be skipped by -SkipNativeImageTests.'
    }
    foreach ($t in $nativeFixtures) { Write-Host "[skip] $($t.name) (explicit -SkipNativeImageTests selection)" }
    $tests = @($tests | Where-Object { $_.name -notin $nativeFixtures.name })
} elseif (@($nativeFixtures | Where-Object { -not $_.arg }).Count -and (-not $Doom -or -not $DoomAlt)) {
    throw 'Native fixture tests require -Doom <unpacked Vulkan exe> and -DoomAlt <unpacked OpenGL exe>. CI must explicitly select -SkipNativeImageTests; this does not qualify the native contracts.'
}

if ($RecordRetainedClosure -and -not @($tests | Where-Object { $_.retained }).Count) {
    throw "-RecordRetainedClosure needs occurrence_revival_retained selected."
}
$local = Join-Path $here 'local'
$retainedRecord = Join-Path $local 'retained-inputs.json'
$retainedInputs = $null
if ($DoomBase -or $ReflectionReplay) {
    if (-not ($DoomBase -and $ReflectionReplay)) { throw "-DoomBase and -ReflectionReplay are required together." }
    $retainedInputs = [pscustomobject]@{ doom_base = (Resolve-Path $DoomBase).Path; replay = (Resolve-Path $ReflectionReplay).Path }
} elseif (Test-Path $retainedRecord) {
    $retainedInputs = Get-Content $retainedRecord -Raw | ConvertFrom-Json
}
$builtExes = @{}
function Build-Test($t) {
    $defs = if ($t.defs) { " $($t.defs)" } else { "" }
    $cxx  = if ($t.cxx)  { " /EHsc /std:c++17" } else { "" }
    $libs = if ($t.libs) { " /link $($t.libs)" } else { "" }
    # Relative output paths avoid cl's quoted trailing-backslash parsing issue.
    $cl  = "cl /nologo /O2 /MT /I..\src\backend /I..\src\common /I..\src\fault_shield /I..\src\ui\webview$cxx$defs $($t.src) /Fe:obj\$($t.name).exe /Foobj\$libs"
    $log = Join-Path $obj ($t.name + ".build.log")
    # Retain the actual resolved translation units and flags beside each log.
    Set-Content -LiteralPath (Join-Path $obj ($t.name + ".compile-command.txt")) -Value $cl -Encoding UTF8
    # Judge the compiler exit code; vcvars may emit unrelated stderr diagnostics.
    cmd /c "cd /d `"$here`" && $cl > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content $log | Write-Host; Write-Host "[FAIL] compile $($t.name)"; return $false }
    $builtExes[$t.name] = $true
    return $true
}

$fail = 0
foreach ($t in $tests) {
    if ($t.sweep) {
        if ($t.manual -and $t.name -notin $Only) { continue }
        if (-not $retainedInputs) {
            Write-Host "[FAIL] $($t.name): it reads the retained reflection replay and installation. Run once:"
            Write-Host "       tests\run-tests.ps1 -Only occurrence_revival_retained -DoomBase <DOOM>\base -ReflectionReplay <replay.tsv>"
            $fail++; continue
        }
        if (-not $builtExes['map_form_sweep_test'] -and -not (Build-Test $sweepEntry)) { $fail++; continue }
        $exe = Join-Path $obj 'map_form_sweep_test.exe'
        $doomRoot = Split-Path -Parent $retainedInputs.doom_base
        $replay = $retainedInputs.replay
        if (-not [System.IO.Path]::IsPathRooted($replay) -or -not (Test-Path $replay)) { $replay = Join-Path $local (Split-Path -Leaf $replay) }
        if ($t.sweep -eq 'fixtures') {
            & $exe $replay $doomRoot fixtures (Join-Path $here 'map_form_sweep_fixtures.jsonl')
        } else {
            $list = Join-Path $local 'map-form-corpus.txt'
            if (-not (Test-Path $list)) { Write-Host "[skip] $($t.name): no tests\local\map-form-corpus.txt"; continue }
            & $exe $replay $doomRoot corpus $list (Join-Path $obj 'map-form-corpus.tsv')
            if ($LASTEXITCODE -eq 0) {
                $refused = @(Get-Content (Join-Path $obj 'map-form-corpus.tsv') | Where-Object { ($_ -split "`t")[1] -eq 'result' -and ($_ -split "`t")[2] -ne 'opened' })
                if ($refused.Count) { $refused | Select-Object -First 20 | Write-Host; $LASTEXITCODE = 1 }
            }
        }
        if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name) (exit $LASTEXITCODE)"; $fail++ }
        else { Write-Host "[ok]   $($t.name)" }
        continue
    }
    if ($t.retained) {
        if (-not $retainedInputs) {
            Write-Host "[FAIL] $($t.name): its game inputs are not configured. Run once:"
            Write-Host "       tests\run-tests.ps1 -Only $($t.name) -DoomBase <DOOM>\base -ReflectionReplay <replay.tsv>"
            Write-Host "       (tests\README.md, `"Retained request-6 composition`"; CI skips it explicitly)"
            $fail++; continue
        }
        if (-not $builtExes['occurrence_revival_test'] -and -not (Build-Test $revivalEntry)) { $fail++; continue }
        $exe = Join-Path $obj 'occurrence_revival_test.exe'
        $replay = $retainedInputs.replay
        if ($DoomBase) {
            # Keep the configured replay independent of where it was produced.
            New-Item -ItemType Directory -Force $local | Out-Null
            $kept = Join-Path $local (Split-Path -Leaf $replay)
            if ($replay -ne $kept) { Copy-Item -Force $replay $kept }
            $replay = $kept
        }
        if ($RecordRetainedClosure) {
            $recorded = Join-Path $obj 'retained-stock-reads.tsv'
            if (Test-Path $recorded) { Remove-Item -Force $recorded }
            $recordOk = $true
            $env:RT_STOCK_RECORD = $recorded
            try {
                foreach ($mode in @('', 'untyped', 'uncorrected')) {
                    $modeArgs = @('retained', $retainedInputs.doom_base, $replay)
                    if ($mode) { $modeArgs += $mode }
                    & $exe @modeArgs | Out-Null
                    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name): recording the $mode stock reads failed (exit $LASTEXITCODE)"; $recordOk = $false }
                }
            } finally { Remove-Item Env:RT_STOCK_RECORD }
            if (-not $recordOk) { $fail++; continue }
            # Ordinal order keeps the generated closure identical on every machine.
            $all = [string[]]@(Get-Content $recorded)
            [Array]::Sort($all, [StringComparer]::Ordinal)
            $rows = [System.Collections.Generic.List[string]]::new()
            foreach ($row in $all) { if (-not $rows.Count -or $rows[$rows.Count - 1] -cne $row) { $rows.Add($row) } }
            $paths = @($rows | ForEach-Object { ($_ -split "`t")[0] } | Sort-Object -Unique -CaseSensitive)
            if (-not $rows.Count -or $paths.Count -ne $rows.Count) {
                Write-Host "[FAIL] $($t.name): the recorded stock reads are empty or read one path two ways ($recorded)"; $fail++; continue
            }
            $lines = @('/* Generated by tests\run-tests.ps1 -RecordRetainedClosure; do not edit. Each stock',
                ' * path the retained request-6 composition reads (R-274), with its reader, length,',
                ' * SHA-256 and catalog provider rows (count:archive:offset:stored size), or "absent". */',
                'typedef struct rt_stock_pin { const char *path, *source; size_t length; const char *sha256, *rows; } rt_stock_pin;',
                'static const rt_stock_pin rt_stock_closure[] = {')
            foreach ($row in $rows) {
                $f = @($row -split "`t" | ForEach-Object { $_.Replace('\', '\\').Replace('"', '\"') })
                $lines += "    {`"$($f[0])`", `"$($f[1])`", $($f[2]), `"$($f[3])`", `"$($f[4])`"},"
            }
            $lines += '};'
            [System.IO.File]::WriteAllText((Join-Path $here 'retained_stock_closure.h'), ($lines -join "`n") + "`n")
            $absent = @($rows | Where-Object { ($_ -split "`t")[1] -ceq 'absent' }).Count
            Write-Host "[ok]   $($t.name): recorded $($rows.Count) stock path(s), $absent absent, into retained_stock_closure.h"
            if (-not (Build-Test $revivalEntry)) { $fail++; continue }
        }
        $retainedOk = $true
        foreach ($mode in @('', 'untyped', 'uncorrected')) {
            $modeArgs = @('retained', $retainedInputs.doom_base, $replay)
            if ($mode) { $modeArgs += $mode }
            $label = if ($mode) { $mode } else { 'faithful' }
            & $exe @modeArgs
            if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name) $label (exit $LASTEXITCODE)"; $fail++; $retainedOk = $false }
            else { Write-Host "[ok]   $($t.name) $label" }
        }
        if ($retainedOk -and $DoomBase) {
            [pscustomobject]@{ doom_base = $retainedInputs.doom_base; replay = $replay } | ConvertTo-Json |
                Set-Content -Encoding utf8 $retainedRecord
        }
        continue
    }
    $exe = Join-Path $obj ($t.name + ".exe")
    if (-not (Build-Test $t)) { $fail++; continue }
    if ($t.arg) { & $exe @($t.arg) } else { & $exe }
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name) (exit $LASTEXITCODE)"; $fail++ }
    else { Write-Host "[ok]   $($t.name)" }
}
if ($fail -gt 0) { Write-Host ""; Write-Host "$fail native test(s) FAILED"; exit 1 }
Write-Host ""; Write-Host "all native tests passed ($($tests.Count))"

if ($Only.Count) { exit 0 }

powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'output_lock_test.ps1')
if ($LASTEXITCODE -ne 0) { throw 'test output lock controls failed' }

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { Write-Host "[FAIL] node not found (required for decl editor tests)"; exit 1 }
$jsTests = @("decl_overlay_test.js", "decl_index_order_test.js", "decl_enum_values_test.js", "decl_language_test.js", "ui_assets_test.js", "asset_browser_test.js", "entity_list_test.js", "prefab_transform_test.js", "prefab_viewport_contract_test.js", "window_chrome_contract_test.js", "feedback_channel_test.js", "feedback_renderer_test.js", "worker_body_test.js", "worker_quota_test.js", "rawmap_menu_test.js", "delete_result_test.js")
foreach ($jsTest in $jsTests) {
    & $node.Source (Join-Path $here $jsTest)
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $jsTest (exit $LASTEXITCODE)"; exit 1 }
    Write-Host "[ok]   $jsTest"
}
Write-Host "all JavaScript tests passed ($($jsTests.Count))"
} finally {
    $env:SNAPMAPPLUS_NATIVE_TEST_VK = $previousNativeVk
    $env:SNAPMAPPLUS_NATIVE_TEST_GL = $previousNativeGl
    $testOutputLock.Dispose()
}
