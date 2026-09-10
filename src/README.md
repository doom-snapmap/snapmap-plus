# Runtime source

## Purpose

Builds the backend and frontend DLLs installed into DOOM 2016.

## Contents

- [backend/](backend/) owns engine hooks, entity edits, packages, media previews and navigation.
- [common/](common/) owns the shared DLL interface and logging helper.
- [fault_shield/](fault_shield/) handles selected engine faults and crash evidence.
- [ui/](ui/) builds the WebView2 companion window.
- [workers/](workers/) shares request handling between the website services.

## Working here

Run the repository-root `build.ps1` to build both DLLs together. Keep engine addresses and calls behind the backend interface. Read [architecture](../docs/architecture.md) for component boundaries and the shared header for the DLL contract. Build and test requirements are in [contributing](../docs/contributing.md).
