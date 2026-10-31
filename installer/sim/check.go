package main

import (
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// The layout contract the harness checks after every step.
//
// DOOM folder: exactly the vanilla executables plus the files the step expects (--expect: the pair,
// and any file of another tool the scenario seeded), nothing else. Runtime folders the DLLs write
// while the game runs (snapmap-plus\logs, snapmap-plus\crash) are never present in a simulation.
//
// Data folder, installed: snapmap-plus.exe, snapmap-plus-store.exe and install.json, plus the
// player's data; install.json starts with its version key, names this DOOM folder and the pair, and
// records both executables with the hashes on disk; cache\installer holds only what install.json
// records (backups of files that were not ours, replaced images still to remove). Uninstalled: none
// of the installer's files remain. Either way every --keep file (the player's data) is still there.

var vanilla = []string{"DOOMx64.exe", "DOOMx64vk.exe"}

// pairNames are both builds' pairs, lower-case.
var pairNames = lowerSet([]string{"XINPUT1_4.dll", pair2024UI, "XINPUT1_3.dll", pair2018UI})

// playerData are the data-folder entries that belong to the player or the game, with how they are
// labelled in the listing.
var playerData = map[string]string{
	"config.json": "player data", "prefabs": "player data", "strings": "player data", "mods": "player data",
	"rawmap.json": "player data", "test-maps": "player data", "cache": "regenerable cache", "token": "player data",
	"pinned.json": "player data",
	// The store tool moves the packages of the retired overrides\ folder into mods\ once and leaves
	// overrides\ as it was: the player's to delete.
	"overrides": "player data (retired; the store tool leaves it unchanged)",
}

var installerFiles = map[string]bool{"snapmap-plus.exe": true, "snapmap-plus-store.exe": true, "install.json": true}

type simRecord struct {
	Version   string   `json:"version"`
	DoomPath  string   `json:"doom_path"`
	DoomBuild string   `json:"doom_build"`
	Files     []string `json:"files"`
	Backups   []struct {
		Rel    string `json:"rel"`
		Backup string `json:"backup"`
	} `json:"backups"`
	Executables map[string]struct {
		SHA256  string `json:"sha256"`
		Version string `json:"version"`
	} `json:"executables"`
	SelfUpdate *struct {
		Release      string `json:"release"`
		PlacedSHA256 string `json:"placed_sha256"`
		SetAside     []struct {
			Path string `json:"path"`
		} `json:"set_aside"`
	} `json:"self_update"`
}

func listFiles(root string) []string {
	var out []string
	filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		rel, _ := filepath.Rel(root, path)
		if rel == "." {
			return nil
		}
		if d.IsDir() {
			if entries, _ := os.ReadDir(path); len(entries) == 0 {
				out = append(out, rel+`\`)
			}
			return nil
		}
		out = append(out, rel)
		return nil
	})
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i]) < strings.ToLower(out[j]) })
	return out
}

func lowerSet(list []string) map[string]bool {
	m := map[string]bool{}
	for _, s := range list {
		m[strings.ToLower(filepath.Clean(s))] = true
	}
	return m
}

func topName(rel string) string { return strings.SplitN(rel, `\`, 2)[0] }

// cmdCheck prints both folders and reports whether the step met the contract.
func cmdCheck(args []string, w io.Writer) (bool, error) {
	o, _ := parseOpts(args)
	if err := o.need("stage", "doom", "data", "phase"); err != nil {
		return false, err
	}
	phase := o.one("phase")
	if phase != "installed" && phase != "uninstalled" && phase != "before" {
		return false, fmt.Errorf("--phase is installed, uninstalled or before")
	}
	// "before" lists a seeded layout without judging it.
	unexpected := "UNEXPECTED"
	if phase == "before" {
		unexpected = "seeded"
	}
	doom, data := o.one("doom"), o.one("data")
	var problems []string
	fail := func(format string, a ...any) { problems = append(problems, fmt.Sprintf(format, a...)) }

	fmt.Fprintf(w, "\nContract check: %s\n%s\n", o.one("stage"), strings.Repeat("-", len("Contract check: ")+len(o.one("stage"))))

	// DOOM folder.
	expect := lowerSet(o["expect"])
	van := lowerSet(append(append([]string{}, vanilla...), o["vanilla"]...)) // --vanilla: more game files the scenario copied
	fmt.Fprintf(w, "  DOOM folder: %s\n", doom)
	got := listFiles(doom)
	seen := map[string]bool{}
	for _, rel := range got {
		k := strings.ToLower(filepath.Clean(rel))
		seen[k] = true
		label := unexpected
		switch {
		case van[k]:
			label = "vanilla"
		case expect[k]:
			label = "not ours, left in place"
			if pairNames[k] {
				label = "the pair"
			}
		}
		fmt.Fprintf(w, "      %-52s %s\n", rel, label)
		if label == unexpected {
			fail("the DOOM folder holds %s", rel)
		}
	}
	for k := range expect {
		if !seen[k] {
			fail("the DOOM folder lacks %s", k)
		}
	}
	for k := range van {
		if !seen[k] {
			fail("the DOOM folder lacks the vanilla %s", k)
		}
	}

	// Data folder.
	fmt.Fprintf(w, "  Data folder: %s\n", data)
	var rec *simRecord
	raw, rerr := os.ReadFile(filepath.Join(data, "install.json"))
	if rerr == nil {
		rec = &simRecord{}
		if err := json.Unmarshal(raw, rec); err != nil {
			fail("install.json is not readable: %v", err)
			rec = nil
		}
	}
	recorded := map[string]string{}
	if rec != nil {
		for _, b := range rec.Backups {
			recorded[strings.ToLower(filepath.Clean(b.Backup))] = "backup of a file that was not ours (" + b.Rel + ")"
		}
		if rec.SelfUpdate != nil {
			for _, a := range rec.SelfUpdate.SetAside {
				recorded[strings.ToLower(filepath.Clean(a.Path))] = "replaced image, removed at the next start"
			}
		}
	}
	cache := filepath.Join(data, "cache", "installer")
	for _, rel := range listFiles(data) {
		top := topName(rel)
		label, isPlayer := playerData[top]
		abs := filepath.Join(data, rel)
		switch {
		case installerFiles[strings.ToLower(top)] && rel == top:
			label = "installer"
			if phase == "uninstalled" || phase == "before" {
				label = unexpected + " (uninstall left it)"
				if phase == "before" {
					label = "seeded"
				}
				fail("uninstall left %s", rel)
			}
		case strings.HasPrefix(strings.ToLower(abs), strings.ToLower(cache)+`\`):
			if why, ok := recorded[strings.ToLower(filepath.Clean(abs))]; ok {
				label = why
			} else {
				label = unexpected
				fail("cache\\installer holds %s, which install.json does not record", rel)
			}
		case isPlayer:
		default:
			label = unexpected
			fail("the data folder holds %s", rel)
		}
		fmt.Fprintf(w, "      %-52s %s\n", rel, label)
	}
	if phase == "installed" {
		for name := range installerFiles {
			if _, err := os.Stat(filepath.Join(data, name)); err != nil {
				fail("the data folder lacks %s", name)
			}
		}
		if rec != nil {
			if !strings.HasPrefix(strings.TrimLeft(string(raw), "\xef\xbb\xbf"), "{\n  \"version\"") {
				fail("install.json does not begin with its version key")
			}
			if !strings.EqualFold(filepath.Clean(rec.DoomPath), filepath.Clean(doom)) {
				fail("install.json names DOOM folder %q", rec.DoomPath)
			}
			for _, rel := range rec.Files {
				if !expect[strings.ToLower(filepath.Clean(rel))] {
					fail("install.json records %s, which is not part of the pair", rel)
				}
			}
			for _, name := range []string{"snapmap-plus.exe", "snapmap-plus-store.exe"} {
				e, ok := rec.Executables[name]
				sum, _, err := fileSHA256(filepath.Join(data, name))
				if !ok || err != nil || !strings.EqualFold(e.SHA256, sum) {
					fail("install.json does not record %s with the hash on disk", name)
				}
			}
			fmt.Fprintf(w, "  install.json: version %s, build %s, %d file(s), %d backup(s), executables %s", rec.Version, rec.DoomBuild,
				len(rec.Files), len(rec.Backups), strings.Join(sortedKeys(rec.Executables), ", "))
			if rec.SelfUpdate != nil {
				fmt.Fprintf(w, ", self-update release %s", rec.SelfUpdate.Release)
			}
			fmt.Fprintln(w)
		}
	} else if exists(cache) {
		fail("uninstall left cache\\installer")
	}
	for _, rel := range o["keep"] {
		if !exists(filepath.Join(data, rel)) {
			fail("the player's %s is gone", rel)
		}
	}
	if phase == "before" {
		return true, nil
	}
	if len(problems) == 0 {
		fmt.Fprintf(w, "  ok  contract met\n")
		return true, nil
	}
	for _, p := range problems {
		fmt.Fprintf(w, "  xx  %s\n", p)
	}
	return false, nil
}

func sortedKeys[V any](m map[string]V) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}
