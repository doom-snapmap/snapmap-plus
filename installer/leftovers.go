package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
)

type leftoverKind int

const (
	ownedExe leftoverKind = iota
	bookkeeping
	emptyDir
	logTree
	replacedImage
)

type leftover struct {
	name string
	kind leftoverKind
	game bool
}

var earlierFiles = []leftover{
	{"snapmap-plus-store.exe", ownedExe, false},
	{selfExeAsset, replacedImage, false},
	{legacyExeName, replacedImage, false},
	{retiredExecutableRecord, bookkeeping, false},
	{retiredVersionsRecord, bookkeeping, false},
	{"override-backups", emptyDir, false},
	{"package-cache", emptyDir, false},
	{"package-staging", emptyDir, false},
	{"snaphak_logs", logTree, true},
	{filepath.Join("snaphak", "logs"), logTree, true},
	{"snaphak", emptyDir, true},
}

func earlierExeDigests(dir string) map[string]string {
	var digests map[string]string
	path := filepath.Join(dir, retiredExecutableRecord)
	if plainPath(path) {
		if b, err := os.ReadFile(path); err == nil {
			_ = json.Unmarshal(b, &digests)
		}
	}
	return digests
}

// Remove known leftovers. The old digest record supplies ownership proof only for a retired executable.
func removeLeftovers(doom, data, running string) {
	if data == "" || !plainPath(data) {
		return
	}
	rec, err := loadStateAt(data)
	if err != nil {
		return
	}
	oldDigests := earlierExeDigests(data)
	stable := filepath.Join(data, selfExeAsset)
	// Keep proof until the stable installer has been adopted or replaced on its normal install path.
	keepProof := !ownsExecutable(stable, rec.Executables[selfExeAsset].SHA256) && ownsExecutable(stable, oldDigests[selfExeAsset])
	changed := false
	scanned := map[string]bool{}
	images := func(exe string) {
		key := strings.ToLower(filepath.Clean(exe))
		if scanned[key] || !plainPath(exe) {
			return
		}
		scanned[key] = true
		if update := exe + retiredUpdatesSuffix; plainPath(update) && os.Remove(update) == nil {
			startupTidy = append(startupTidy, filepath.Base(update)+" (retired installer record)")
		}
		if st, err := os.Lstat(exe); err != nil || !st.Mode().IsRegular() {
			return
		}
		entries, _ := os.ReadDir(filepath.Dir(exe))
		for _, entry := range entries {
			if !updateBackupName(exe, entry.Name()) || !entry.Type().IsRegular() {
				continue
			}
			old := filepath.Join(filepath.Dir(exe), entry.Name())
			if plainPath(old) && os.Remove(old) == nil {
				startupTidy = append(startupTidy, entry.Name()+" (replaced installer)")
			}
		}
	}
	for _, rule := range earlierFiles {
		root := data
		if rule.game {
			root = doom
		}
		if root == "" {
			continue
		}
		p := filepath.Join(root, rule.name)
		if !plainPath(p) {
			continue
		}
		if rule.kind == replacedImage {
			images(p)
			continue
		}
		info, err := os.Lstat(p)
		if err != nil && !(rule.kind == ownedExe && os.IsNotExist(err)) {
			continue
		}
		switch rule.kind {
		case ownedExe:
			images(p)
			digest := oldDigests[rule.name]
			if entry, ok := rec.Executables[rule.name]; ok {
				digest = entry.SHA256
			}
			if info != nil {
				if ownsExecutable(p, digest) {
					if os.Remove(p) != nil {
						keepProof = true
						continue
					}
					startupTidy = append(startupTidy, rule.name+" (retired installer executable)")
				} else {
					startupKept = append(startupKept, rule.name+" (not the installed copy)")
				}
			}
			if _, ok := rec.Executables[rule.name]; ok {
				delete(rec.Executables, rule.name)
				changed = true
			}
		case bookkeeping:
			if !keepProof && info.Mode().IsRegular() && os.Remove(p) == nil {
				startupTidy = append(startupTidy, rule.name+" (retired installer record)")
			}
		case emptyDir:
			if info.IsDir() {
				if os.Remove(p) == nil {
					startupTidy = append(startupTidy, rule.name+" (empty folder)")
				} else if !rule.game {
					startupKept = append(startupKept, rule.name+" (the folder holds files)")
				}
			}
		case logTree:
			if info.IsDir() && os.RemoveAll(p) == nil {
				startupTidy = append(startupTidy, rule.name+" (earlier runtime logs)")
			}
		}
	}
	for name := range rec.Executables {
		if executableName(name) {
			images(filepath.Join(data, name))
		}
	}
	if running != "" {
		images(running)
	}
	if changed {
		_ = saveStateAt(data, rec)
	}
}
