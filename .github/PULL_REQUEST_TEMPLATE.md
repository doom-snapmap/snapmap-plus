<!-- Thanks for contributing to open-snaphak! Keep each PR focused on a single change. -->

## What this changes

<!-- A short description of the change and why it's needed. -->

## Checklist

- [ ] Clean-room: this is my own reverse-engineering / implementation -- no decompiled or copyrighted DOOM or
      original-SnapHak content is pasted into the repo.
- [ ] No binaries added (`.dll` / `.exe` / `.obj` / `.pdb` / `.zip` ...) -- the source is the only deliverable.
- [ ] Source stays pure ASCII (`.c` / `.h` / `.cpp` / `.ps1`).
- [ ] Built + tested locally: `build-qt.ps1` -> `package-qt.ps1` -> `snaphak install --local dist`, and I ran the Go
      (`gofmt` / `go vet` / `go test`) and C (`tests\run-tests.ps1`) suites.
- [ ] **If this PR touches `src/backend/`, `src/common/snaphak_iface.h`, or `src/ui/webview/`**: ALSO built +
      tested the experimental webview frontend locally (`build-webview.ps1` -> `package-webview.ps1` ->
      install --local dist) before pushing -- CI's `build-webview` job will catch a break here too, but a
      local round-trip is faster feedback and lets you confirm it actually works in DOOM (see
      `docs/architecture.md`'s note on the vtable's extension slots for why this matters). Not applicable
      otherwise.
- [ ] Docs updated in this PR for any behavior change (see `docs/contributing.md` section 9).

<!-- On every PR, CI runs a secretless security gate + a Windows build/test, and a maintainer reviews before merge. -->
