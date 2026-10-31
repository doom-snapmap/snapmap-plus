package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"time"
)

// The layouts earlier releases left, written with stand-in bytes. A stand-in for a Snapmap+ DLL
// carries the "snapmap-plus" marker every Snapmap+ DLL does (the snapmap-plus\ paths it loads and
// logs to), so the installer recognises it as ours exactly as it does a real one. A stand-in for the
// original SnapHak or for another tool does not.

const (
	pair2024UI = `snapmap-plus\snapmap-plus-ui-13954591.dll`
	pair2018UI = `snapmap-plus\snapmap-plus-ui.dll`
)

func ourStandIn(what string) []byte {
	return []byte("MZ simulated " + what + " of an earlier Snapmap+ release; loads .\\snapmap-plus\\ files")
}

func hexSHA(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

func write(root, rel string, data []byte) error {
	p := filepath.Join(root, rel)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	return os.WriteFile(p, data, 0o644)
}

// copyTree copies every file under src into dst.
func copyTree(src, dst string) error {
	return filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(src, path)
		if filepath.Base(rel) == ".gitattributes" {
			return nil
		}
		return copyFile(path, filepath.Join(dst, rel))
	})
}

type oldRecord struct {
	Version     string      `json:"version"`
	DoomPath    string      `json:"doom_path"`
	InstalledAt string      `json:"installed_at"`
	Files       []string    `json:"files"`
	Backups     []oldBackup `json:"backups"`
}

type oldBackup struct {
	Rel    string `json:"rel"`
	Backup string `json:"backup"`
}

// cmdSeed writes one earlier layout into the scratch DOOM, data and profile folders.
func cmdSeed(args []string) error {
	o, rest := parseOpts(args)
	if err := o.need("doom", "data", "profile"); err != nil {
		return err
	}
	if len(rest) != 1 {
		return fmt.Errorf("seed needs one layout: snaphak, beta19 or beta20")
	}
	doom, data, profile := o.one("doom"), o.one("data"), o.one("profile")
	now := time.Now().UTC().Format(time.RFC3339)
	steps := []struct {
		root, rel string
		data      []byte
	}{}
	add := func(root, rel string, b []byte) {
		steps = append(steps, struct {
			root, rel string
			data      []byte
		}{root, rel, b})
	}
	switch rest[0] {
	case "snaphak":
		// Chrispy's original SnapHak in DOOM, its player data under %USERPROFILE%\snaphak, and a
		// file another tool left beside it.
		for _, rel := range []string{"dinput8.dll", "XINPUT1_3.dll", "snaphak_algo.dll", "snaphak_ext.dll", "changelog.txt",
			"doomlegacymod.txt", `snaphak\snaphakui.dll`, `snaphak\Qt5Core.dll`, `snaphak\Qt5Gui.dll`, `snaphak\Qt5Widgets.dll`,
			`snaphak\Qt5Svg.dll`, `snaphak\lua51.dll`, `snaphak\ds_descriptions.json`, `plugins\platforms\qwindows.dll`,
			`platforms\qwindows.dll`} {
			add(doom, rel, []byte("simulated original SnapHak file "+rel))
		}
		add(doom, "dinput8.dll.doomforge.rollback", []byte("simulated file another tool left"))
		add(profile, `snaphak\prefabs\arena.snapmap.json`, []byte(`{"prefab":"arena"}`))
		add(profile, `snaphak\strings\en.json`, []byte(`{"hello":"world"}`))
		add(profile, `snaphak\rawmap.json`, []byte(`{"rawmap":1}`))
	case "beta19":
		// v0.2.1-beta.19's pair left in an April 2024 folder, its install record and installer copy,
		// and a data folder of that era (tests/fixtures/store-retired) with an overrides\ package.
		add(doom, "XINPUT1_3.dll", ourStandIn("v0.2.1-beta.19 XINPUT1_3.dll"))
		add(doom, pair2018UI, ourStandIn("v0.2.1-beta.19 frontend"))
		exe := []byte("simulated v0.2.1-beta.19 snapmap-plus.exe")
		add(data, "snapmap-plus.exe", exe)
		files, _ := json.Marshal(map[string]string{"snapmap-plus.exe": hexSHA(exe)})
		add(data, "installer-files.json", files)
		rec, _ := json.MarshalIndent(oldRecord{Version: "v0.2.1-beta.19", DoomPath: doom, InstalledAt: now,
			Files: []string{"XINPUT1_3.dll", pair2018UI}, Backups: []oldBackup{}}, "", "  ")
		add(data, "install.json", rec)
		add(data, "config.json", []byte(`{"schema_version":1,"settings":{"theme":"dark"}}`))
		if fx := o.one("fixtures"); fx != "" {
			if err := copyTree(filepath.Join(fx, "store-retired", "data"), data); err != nil {
				return fmt.Errorf("copying the store-retired fixture: %v", err)
			}
		}
	case "beta20":
		// v0.2.1-beta.20's blind update: both pairs, backups of our own DLLs in DOOM, a file another
		// tool left, and every bookkeeping file of that release in the data folder.
		add(doom, "XINPUT1_4.dll", ourStandIn("v0.2.1-beta.20 XINPUT1_4.dll"))
		add(doom, pair2024UI, ourStandIn("v0.2.1-beta.20 frontend"))
		add(doom, "XINPUT1_3.dll", ourStandIn("v0.2.1-beta.19 XINPUT1_3.dll"))
		add(doom, pair2018UI, ourStandIn("v0.2.1-beta.19 frontend"))
		add(doom, "XINPUT1_4.dll.snapmap-plus-bak", ourStandIn("development XINPUT1_4.dll"))
		add(doom, pair2024UI+".snapmap-plus-bak", ourStandIn("development frontend"))
		add(doom, "XINPUT1_3.dll.snapmap-plus-bak", ourStandIn("v0.2.1-beta.18 XINPUT1_3.dll"))
		add(doom, "dinput8.dll.doomforge.rollback", []byte("simulated file another tool left"))
		exe := []byte("simulated v0.2.1-beta.20 snapmap-plus.exe")
		old := []byte("simulated v0.2.1-beta.19 snapmap-plus.exe")
		add(data, "snapmap-plus.exe", exe)
		add(data, "snapmap-plus.exe.old", old)
		upd, _ := json.MarshalIndent(map[string]any{"current_sha256": hexSHA(exe),
			"backups": map[string]string{"snapmap-plus.exe.old": hexSHA(old)}}, "", "  ")
		add(data, "snapmap-plus.exe.updates.json", upd)
		files, _ := json.Marshal(map[string]string{"snapmap-plus.exe": hexSHA(exe)})
		add(data, "installer-files.json", files)
		vers, _ := json.Marshal(map[string]any{"snapmap-plus.exe": map[string]string{"sha256": hexSHA(exe), "version": "v0.2.1-beta.20"}})
		add(data, "installer-versions.json", vers)
		rec, _ := json.MarshalIndent(oldRecord{Version: "v0.2.1-beta.20", DoomPath: doom, InstalledAt: now,
			Files: []string{"XINPUT1_4.dll", pair2024UI, "XINPUT1_3.dll", pair2018UI},
			Backups: []oldBackup{
				{Rel: "XINPUT1_4.dll", Backup: filepath.Join(doom, "XINPUT1_4.dll.snapmap-plus-bak")},
				{Rel: pair2024UI, Backup: filepath.Join(doom, pair2024UI+".snapmap-plus-bak")},
			}}, "", "  ")
		add(data, "install.json", rec)
		add(data, "config.json", []byte(`{"schema_version":1,"settings":{"theme":"light"}}`))
		add(data, `prefabs\corridor.snapmap.json`, []byte(`{"prefab":"corridor"}`))
		add(data, `mods\imp\mod.json`, []byte(`{"id":"imp","version":"1.0.0"}`))
		if err := os.MkdirAll(filepath.Join(data, "override-backups"), 0o755); err != nil {
			return err
		}
	default:
		return fmt.Errorf("unknown layout %q", rest[0])
	}
	for _, s := range steps {
		if err := write(s.root, s.rel, s.data); err != nil {
			return err
		}
	}
	return nil
}
