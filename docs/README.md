# Product and contributor documentation

## Purpose

Explains how to develop, package and operate Snapmap+, and how its major components work together.

## Contents

| Guide | Purpose |
|---|---|
| [contributing.md](contributing.md) | Setup, builds, tests, review and releases. |
| [architecture.md](architecture.md) | Component boundaries, threading, ownership and the DLL interface. |
| [webview-ui.md](webview-ui.md) | Frontend modules, messages, embedding and preview. |
| [packaging.md](packaging.md) | Shipped files, runtime dependencies and player-data ownership. |
| [capabilities.md](capabilities.md) | Supported commands, settings and editor workflows. |
| [fidelity.md](fidelity.md) | Compatibility choices and unsupported original features. |
| [feedback.md](feedback.md) | Reporting across the frontend, relay and issue tracker. |
| [services.md](services.md) | External services and credential ownership. |

## Working here

Retain contributor instructions and cross-component contracts here. Explain narrow backend algorithms in source and concise comments; keep their detailed investigations in snaphak-re's findings system. Navigation and weapon HUD implementation references were moved there with their source revisions preserved.

Player instructions live in the [website guide](../site/snapmap-plus-guide.md); release history lives in [CHANGELOG.md](../CHANGELOG.md). External contributors can supply investigation evidence in their pull request without needing a research checkout.
