# Runtime source

## Purpose

Builds the backend and frontend DLLs installed into DOOM 2016.

## Contents

- [backend/](backend/) owns engine hooks, entity edits, media previews, navigation and installed mods.
- [common/](common/) owns the shared DLL interface and logging helper.
- [fault_shield/](fault_shield/) handles selected engine faults and crash evidence.
- [store/](store/) builds `snapmap-plus-store.exe`, the console tool the installer runs to migrate a mods folder of earlier releases, from the backend's store sources.
- [ui/](ui/) builds the WebView2 companion window.
- [workers/](workers/) shares request handling between the website services.

## Working here

Run the repository-root `build.ps1` to build both DLLs together. Keep engine addresses and calls behind the backend interface. Read [architecture](../docs/architecture.md) for component boundaries and the shared header for the DLL contract. Build and test requirements are in [contributing](../docs/contributing.md).
