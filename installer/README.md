# snaphak.exe — the end-user installer

A small, single static Windows CLI that installs the SnapHak clone overlay into a DOOM 2016 install, with
backup and a clean uninstall. **Stdlib only — no external dependencies.**

## Build

```
go build -o snaphak.exe .            # needs Go 1.21+
go build -ldflags "-X main.version=v1.2.3" -o snaphak.exe .   # stamp a release version (CI does this)
```

## Use

```
snaphak install   [--doom <path>] [--local <dist-dir>] [--release <tag>]
snaphak update    [--doom <path>] [--release <tag>]
snaphak uninstall [--doom <path>]
snaphak status
```

- **`--doom <path>`** — the DOOM install dir (the folder with `DOOMx64vk.exe`). If omitted, the installer
  auto-detects it from your Steam libraries (reads `SteamPath` from the registry, scans
  `libraryfolders.vdf` for the library holding appid `379720`, and verifies `DOOMx64vk.exe` is there).
- **`--local <dist-dir>`** — install from a local `dist/` tree (built by the repo's `package.ps1`) instead of
  downloading. This is the contributor / local-test path.
- **`--release <tag>`** — install a specific release tag instead of the latest.

With no `--local`, `install`/`update` download the latest release bundle from GitHub.

## What it does

- Verifies the bundle against its `MANIFEST.sha256` (every file present + hash-correct) **before** touching DOOM.
- **Backs up** any pre-existing file it would overwrite (e.g. a genuine `XINPUT1_3.dll`) to `<file>.snaphak-bak`.
- Records the install (files placed + backups taken) in `%LOCALAPPDATA%\open-snaphak\install.json`.
- **`uninstall`** reverses *exactly* that record: removes the files it placed, restores the backups, and cleans
  the dirs it created **only if they're empty** — a pre-existing `plugins/` or other content is left intact. Your
  `%USERPROFILE%\snaphak` data (overrides / prefabs / rawmaps) is **never** touched.

## Not yet configured

The GitHub-release download path needs the origin repo set (`repoSlug` in `bundle.go`, `$repo` in `install.ps1`,
both currently the `OWNER/open-snaphak` placeholder). Until then, use `--local <dist>`.
