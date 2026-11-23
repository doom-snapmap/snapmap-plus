# Runtime source

## Purpose

Builds the backend and frontend DLLs installed into DOOM 2016.

## Contents

- [backend/](backend/) owns engine hooks, entity edits, media previews, navigation and installed mods.
- [common/](common/) owns the shared DLL interface and logging helper.
- [fault_shield/](fault_shield/) handles selected engine faults and crash evidence.
- [sdk/](sdk/) holds the Lua libraries (`sdk/lua/*.lua`) that `tools/gen_script_sdk.js` embeds in the backend for mod scripts.
- [store/](store/) builds `snapmap-plus-store.exe`, the console tool that converts a mods folder of earlier releases, from the backend's store sources; the installer does not run it in this release.
- [third_party/](third_party/) holds vendored Lua.
- [ui/](ui/) builds the WebView2 companion window.
- [workers/](workers/) holds the request-body reader (`request_body.js`) the feedback relay imports.

## Working here

Run the repository-root `build.ps1` to build both DLLs together. Keep engine addresses and calls behind the backend interface. Read [architecture](../docs/architecture.md) for component boundaries and the shared header for the DLL contract. Build and test requirements are in [contributing](../docs/contributing.md).
