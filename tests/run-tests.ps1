# run-tests.ps1 -- compile + run Snapmap+'s C unit tests with MSVC (x64). Pure ASCII.
# Needs Build Tools for Visual Studio 2022 (C++ workload) -- the same toolchain the build scripts use.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1 -Doom C:\path\to\unpacked-DOOMx64vk.exe
# Optional explicit installed-resource probe (not part of the default suite):
#   tests\obj\resource_bridge_test.exe <data-root> <doom-base>
# Both paths are required; the probe performs no autodiscovery or environment lookup.
#
# Default: the self-contained tests (no game, no built DLL needed):
#   shield_format_test  -- the fault record string formatter (pure logic)
#   hook_test           -- the inline-detour installer, on a hand-laid scratch stub
#   crash_record_test   -- the crash-record JSON formatter + escaping (pure logic)
#   report_scrub_test   -- the crash-report log anonymization scrub + tail (pure logic)
#   dumpmap_path_test   -- sh_dumpmap's output-path resolution (pure logic)
#   json_pretty_test    -- sh_pretty_on's rawmap JSON re-layout: document preserved, refusals (pure logic)
#   config_json_test    -- bounded JSON grammar, duplicate keys, preservation + serialization
#   iface_config_test   -- append-only config slot layout + dedicated binder isolation
#   config_test         -- config lifecycle, validation, recovery, atomic faults + concurrency
#   user_overrides_test -- immutable launch snapshot, persistence reporting + marker independence
#   user_overrides_contract_test -- startup, command, cvar + loader source-wiring contract
#   decl_server_test -- path-derived identities + bounded decl-text structural validation
#   overrides_internal_test -- exact per-decl table, 31-slot provider ABI, helper gating, and read-only streams
#   decl_server_contract_test -- startup ordering, signature pins, one-shot per-decl source wiring
#   resource_bridge_test -- exact manifest resolution, sparse decode, provider gate + collisions
#   packages_test -- per-package override discovery: markers, legacy tree, order, bounds
#   map_package_test -- map-embedded package shards: scan/extract vs the reference impl, unsafe-zip refusal, load gate
#   override_packages_test -- the file shadow resolves a decl out of any installed package
#   package_requirements_test -- allowlisted package cvars, strict parsing, RUNNING gate + one-shot apply
#   strids_packages_test -- a package ships its own #str_ strings; user > packages > baked
#   config_message_test -- bounded raw WebView config-message extraction
#   theme_bootstrap_test -- pre-navigation dark-class injection (pure C++ helper)
#   theme_contract_test -- native/preview theme bridge contract in the embedded HTML source
#   entity_settings_contract_test -- persisted Entities controls + exclusive selection-mode contract
#   growing_text_buffer_test -- large declaration reads, exact boundary, and explicit safety-cap signal
#   preview_test       -- generation-safe request/publish handoff and RGBA PNG payload
#   bcn_test           -- BC1/BC3/BC7 vectors, padded dimensions, and overflow guards
#   soundpreview_queue_test -- failed-kick rollback, FIFO preservation, overflow, and name bounds
#   imgpreview_index_test -- bounded parsing, compact-name ownership, catalog routing, and rollback
#   imgpreview_catalog_test -- lazy optional unions, compact Wwise strings, direct images, and paging
#   prefabpreview_test -- bounded BMODEL/MD6 geometry decode and binary transport blob
#   megapreview_io_test -- compact VMTR metadata and selected-entry Mega2 reads
#   serialization_buffer_test -- timeline growth, terminal failures, retained capacity, and 32 MB cap
# The JS checks run after the native suite:
#   decl_overlay_test -- syntax-paint/text alignment for the Entity State editor
#   decl_index_order_test -- numeric item[n] presentation, nesting, and 1000-boundary regression
#   decl_enum_values_test -- schema enum sets match the engine constants, not the localized inspector labels
#   feedback_channel_test -- the relay reads a release channel out of the v-prefixed tag the app reports
#   entity_list_test -- bounded DOM window, full logical filtering, selection, and event delegation
#   prefab_transform_test -- sparse idMat3 defaults, column-major axes, scale, and block anchoring
#   prefab_viewport_contract_test -- Prefab Details layout, resize, budgets, and shared-buffer transport
#   window_chrome_contract_test -- captionless DWM shadow/rounded-corner contract
# -Doom <unpacked DOOMx64vk.exe>: ALSO the signature-resolver tests, which scan a real
#   (Steamless-unpacked) DOOM image:
#   sig_test            -- every engine signature resolves to its known RVA
#   hooktol_test        -- the resolver's hook-tolerant fallback (prologue-clobbered fns)
#
# Exit 0 iff every selected test passes; non-zero (with the build log) on any failure.
# Objects + test exes land in tests\obj\ (gitignored). The runtime XInput-ordinal test
# (xinput_ordinal_test) is run by hand against a built build\XINPUT1_3.dll -- see docs\contributing.md.
param([string]$Doom = "")
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

# name | sources (relative to tests\) | runtime arg
$tests = @(
    @{ name = "shield_format_test"; src = 'shield_format_test.c ..\src\fault_shield\fault_record.c ..\src\common\log_rotate.c'; arg = "" }
    @{ name = "hook_test";          src = 'hook_test.c ..\src\backend\hook.c';                       arg = "" }
    @{ name = "crash_record_test";  src = 'crash_record_test.c ..\src\fault_shield\crash_record_format.c'; arg = "" }
    @{ name = "report_scrub_test";  src = 'report_scrub_test.c';                                     arg = "" }
    @{ name = "dumpmap_path_test";  src = 'dumpmap_path_test.c';                                     arg = "" }
    @{ name = "json_pretty_test";   src = 'json_pretty_test.c';                                      arg = "" }
    @{ name = "config_json_test";   src = 'config_json_test.c ..\src\backend\config_json.c';         arg = "" }
    @{ name = "iface_config_test";  src = 'iface_config_test.c ..\src\common\snapmap_plus_iface.c';   arg = "" }
    @{ name = "config_test";        src = 'config_test.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_test"; src = 'user_overrides_test.c ..\src\backend\user_overrides.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING /DSH_USER_OVERRIDES_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_contract_test"; src = 'user_overrides_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "decl_server_test"; src = 'decl_server_test.c ..\src\backend\decl_server.c ..\src\backend\packages.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c'; defs = '/DSH_DECL_SERVER_TESTING'; arg = "" }
    @{ name = "overrides_internal_test"; src = 'overrides_internal_test.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "decl_server_contract_test"; src = 'decl_server_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "palette_refresh_test"; src = 'palette_refresh_test.c ..\src\backend\palette_refresh.c'; defs = '/DSH_PALETTE_REFRESH_TESTING'; arg = "" }
    @{ name = "engine_dialog_test"; src = 'engine_dialog_test.c ..\src\backend\engine_dialog.c'; defs = '/DSH_ENGINE_DIALOG_TESTING'; arg = "" }
    @{ name = "package_conflicts_test"; src = 'package_conflicts_test.c ..\src\backend\package_conflicts.c ..\src\backend\packages.c'; arg = "" }
    @{ name = "palette_refresh_contract_test"; src = 'palette_refresh_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "resource_bridge_test"; src = 'resource_bridge_test.c ..\src\backend\resource_bridge.c ..\src\backend\packages.c ..\src\backend\raw_deflate.c ..\src\backend\decl_text.c'; defs = '/DSH_RESOURCE_BRIDGE_TESTING /DSH_RAW_DEFLATE_TESTING'; arg = "" }
    @{ name = "packages_test"; src = 'packages_test.c ..\src\backend\packages.c'; arg = "" }
    @{ name = "map_package_test"; src = 'map_package_test.c ..\src\backend\map_package.c ..\src\backend\packages.c ..\src\backend\raw_deflate.c'; defs = '/DSH_MAP_PACKAGE_TESTING'; arg = "" }
    @{ name = "override_packages_test"; src = 'override_packages_test.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "package_requirements_test"; src = 'package_requirements_test.c ..\src\backend\package_requirements.c ..\src\backend\packages.c'; defs = '/DSH_PACKAGE_REQUIREMENTS_TESTING'; arg = "" }
    @{ name = "strids_packages_test"; src = 'strids_packages_test.c ..\src\backend\strids.c ..\src\backend\packages.c ..\src\backend\overrides.c ..\src\backend\decl_text.c'; defs = '/DSH_STRIDS_TESTING /DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "config_message_test"; src = 'config_message_test.cpp ..\src\ui\webview\config_message.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_bootstrap_test"; src = 'theme_bootstrap_test.cpp ..\src\ui\webview\theme_bootstrap.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_contract_test"; src = 'theme_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "entity_settings_contract_test"; src = 'entity_settings_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "growing_text_buffer_test"; src = 'growing_text_buffer_test.cpp'; cxx = $true; arg = "" }
    @{ name = "preview_test";     src = 'preview_test.c ..\src\backend\preview.c';              arg = "" }
    @{ name = "bcn_test";         src = 'bcn_test.c ..\src\backend\bcn.c';                      arg = "" }
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
    $tests += @{ name = "sig_test";     src = 'sig_test.c ..\src\backend\signatures.c';     arg = $da }
    $tests += @{ name = "hooktol_test"; src = 'hooktol_test.c ..\src\backend\signatures.c'; arg = $da }
}

$fail = 0
foreach ($t in $tests) {
    $exe = Join-Path $obj ($t.name + ".exe")
    $defs = if ($t.defs) { " $($t.defs)" } else { "" }
    $cxx  = if ($t.cxx)  { " /EHsc /std:c++17" } else { "" }
    $libs = if ($t.libs) { " /link $($t.libs)" } else { "" }
    # Output paths are RELATIVE (cwd=tests via cd /d) -- a quoted absolute path with a trailing backslash
    # is the cmd `\"` footgun the build scripts document (cl D8036). obj\ exists (created above); names have no spaces.
    $cl  = "cl /nologo /O2 /MT /I..\src\backend /I..\src\common /I..\src\fault_shield /I..\src\ui\webview$cxx$defs $($t.src) /Fe:obj\$($t.name).exe /Foobj\$libs"
    $log = Join-Path $obj ($t.name + ".build.log")
    # vcvars64.bat prints a spurious 'vswhere not recognized' line to stderr; gate on cl's real exit only
    # (the same cmd /c pattern the build scripts use) instead of letting that stderr trip $ErrorActionPreference.
    cmd /c "cd /d `"$here`" && `"$vcvars`" && $cl > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content $log | Write-Host; Write-Host "[FAIL] compile $($t.name)"; $fail++; continue }
    if ($t.arg) { & $exe $t.arg } else { & $exe }
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name) (exit $LASTEXITCODE)"; $fail++ }
    else { Write-Host "[ok]   $($t.name)" }
}
if ($fail -gt 0) { Write-Host ""; Write-Host "$fail native test(s) FAILED"; exit 1 }
Write-Host ""; Write-Host "all native tests passed ($($tests.Count))"

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { Write-Host "[FAIL] node not found (required for decl editor tests)"; exit 1 }
$jsTests = @("decl_overlay_test.js", "decl_index_order_test.js", "decl_enum_values_test.js", "asset_browser_test.js", "entity_list_test.js", "prefab_transform_test.js", "prefab_viewport_contract_test.js", "window_chrome_contract_test.js", "feedback_channel_test.js")
foreach ($jsTest in $jsTests) {
    & $node.Source (Join-Path $here $jsTest)
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $jsTest (exit $LASTEXITCODE)"; exit 1 }
    Write-Host "[ok]   $jsTest"
}
Write-Host "all JavaScript tests passed ($($jsTests.Count))"
