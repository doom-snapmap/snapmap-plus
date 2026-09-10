# Runtime source

## Purpose

Builds the backend and frontend DLLs installed into DOOM 2016.

## Contents

- [backend/](backend/) owns engine hooks, entity edits, packages, media previews and navigation.
- [common/](common/) owns the shared DLL interface and logging helper.
- [fault_shield/](fault_shield/) handles selected engine faults and crash evidence.
- [ui/](ui/) builds the WebView2 companion window.

## Working here

Run the repository-root `build.ps1` to build both DLLs together. Keep engine addresses and calls behind the backend interface. Source and ownership rules are in [architecture](../docs/architecture.md); build and test requirements are in [contributing](../docs/contributing.md).
