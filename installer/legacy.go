package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// Migration away from the original SnapHak.
//
// A DOOM folder that ran the original SnapHak (the closed-source tool this project supersedes) carries
// its overlay: proxy DLLs at the game root (dinput8.dll -- a bundled console-unlock mod -- plus
// XINPUT1_3.dll and two helper DLLs), a Qt UI runtime inside snaphak\, a Qt platform plugin, and two
// text files. None of these are vanilla game files, and this SnapHak replaces all of them. Install
// (and therefore update) removes them first: left in place they'd either be backed up as if they were
// genuine game files (XINPUT1_3.dll -- uninstall would later "restore" a non-vanilla DLL) or linger
// as dead files nothing loads (the helper DLLs, the Qt runtime).
//
// Detection is deliberately conservative: it keys on files ONLY the original SnapHak ships. A lone
// dinput8.dll (which could be an unrelated mod) or a lone changelog.txt never triggers migration --
// those are removed only alongside a real marker.

// legacyMarkers: any one of these present under the DOOM dir means the original SnapHak is installed.
var legacyMarkers = []string{
	"snaphak_algo.dll",
	"snaphak_ext.dll",
	"doomlegacymod.txt",
	filepath.Join("snaphak", "Qt5Core.dll"),
	filepath.Join("snaphak", "Qt5Gui.dll"),
	filepath.Join("snaphak", "Qt5Widgets.dll"),
	filepath.Join("snaphak", "Qt5Svg.dll"),
	filepath.Join("snaphak", "lua51.dll"),
	filepath.Join("snaphak", "ds_descriptions.json"),
	filepath.Join("plugins", "platforms", "qwindows.dll"),
}

// legacyExtras: part of the original SnapHak's bundle, but too generic (or name-shared with our own
// files) to prove anything alone. Removed only when a marker fired. XINPUT1_3.dll and
// snaphak\snaphakui.dll are the original's versions of paths we also use -- removing them before the
// copy loop keeps the backup logic from saving them as "genuine" pre-existing files.
var legacyExtras = []string{
	"dinput8.dll",
	"changelog.txt",
	"XINPUT1_3.dll",
	filepath.Join("snaphak", "snaphakui.dll"),
	filepath.Join("platforms", "qwindows.dll"), // some installs carry the Qt plugin here instead
}

// legacySharedBakRels: overlay-relative paths whose ".snaphak-bak" backup cannot be a genuine game
// file once the original SnapHak is confirmed present -- vanilla DOOM 2016 ships none of these paths,
// so a pre-existing copy that an earlier install backed up was the original SnapHak's file, and
// uninstall must NOT "restore" it.
var legacySharedBakRels = map[string]bool{
	"XINPUT1_3.dll": true,
	filepath.Join("snaphak", "snaphakui.dll"):             true,
	filepath.Join("platforms", "qwindows.dll"):            true,
	filepath.Join("plugins", "platforms", "qwindows.dll"): true,
	"dinput8.dll": true,
}

// detectLegacy returns every original-SnapHak file present under doom, or nil when none of the
// unambiguous markers are there (extras alone never trigger).
func detectLegacy(doom string) []string {
	var present []string
	marker := false
	for _, rel := range legacyMarkers {
		if st, err := os.Stat(filepath.Join(doom, rel)); err == nil && !st.IsDir() {
			present = append(present, rel)
			marker = true
		}
	}
	if !marker {
		return nil
	}
	for _, rel := range legacyExtras {
		if st, err := os.Stat(filepath.Join(doom, rel)); err == nil && !st.IsDir() {
			present = append(present, rel)
		}
	}
	return present
}

// removeLegacy deletes the detected original-SnapHak files and prunes the Qt plugin dirs they leave
// empty (never the snaphak\ dir itself -- our own UI lives there). Best-effort per file: a locked
// file is reported and left behind; the shared-name files (XINPUT1_3.dll, snaphakui.dll) get
// overwritten by the copy that follows anyway. Returns what was actually removed.
func removeLegacy(doom string, files []string) []string {
	var removed []string
	for _, rel := range files {
		if err := os.Remove(filepath.Join(doom, rel)); err != nil {
			if !os.IsNotExist(err) {
				fmt.Fprintf(os.Stderr, "  ! couldn't remove the original SnapHak's %s (%v) -- is DOOM still running?\n", rel, err)
			}
			continue
		}
		removed = append(removed, rel)
		fmt.Printf("  - %s (original SnapHak)\n", rel)
	}
	removeIfEmpty(filepath.Join(doom, "plugins", "platforms"))
	removeIfEmpty(filepath.Join(doom, "plugins"))
	removeIfEmpty(filepath.Join(doom, "platforms"))
	return removed
}

// dropLegacyBackups is the record-side half of the migration: an install made before this migration
// existed may have backed up the original SnapHak's XINPUT1_3.dll (etc.) as if it were a genuine
// game file. With the original SnapHak confirmed present, delete those backup files and drop their
// record entries, so a later uninstall restores nothing that isn't truly vanilla.
func dropLegacyBackups(doom string, baks []backup) []backup {
	kept := baks[:0]
	for _, bk := range baks {
		if !legacySharedBakRels[filepath.FromSlash(bk.Rel)] {
			kept = append(kept, bk)
			continue
		}
		if err := os.Remove(bk.Backup); err != nil && !os.IsNotExist(err) {
			fmt.Fprintf(os.Stderr, "  ! couldn't remove the original SnapHak's saved-aside %s (%v)\n", bk.Rel, err)
			kept = append(kept, bk) // couldn't delete it -> keep the record entry so nothing dangles
			continue
		}
		fmt.Printf("  - %s (original SnapHak, saved aside by an earlier install)\n", bk.Rel)
	}
	return kept
}
