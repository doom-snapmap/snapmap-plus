package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// Remove the recognized SnapHak runtime before installing its replacement.
// Detection requires a tool-specific marker; generic proxy DLLs and text files
// alone must not trigger migration.

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

// legacyExtras contains shared or generic names removed only after marker detection.
// Remove them before backup so uninstall does not restore the legacy runtime.
var legacyExtras = []string{
	"dinput8.dll",
	"changelog.txt",
	"XINPUT1_3.dll",
	filepath.Join("snaphak", "snaphakui.dll"),
	filepath.Join("platforms", "qwindows.dll"), // some installs carry the Qt plugin here instead
}

// legacySharedBakRels identifies shared-name backups discarded after detecting
// the original runtime; vanilla DOOM does not supply these paths.
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

// removeLegacy reports files it removed and leaves locked files behind.
// It prunes empty Qt plugin directories but retains the snaphak directory.
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

// dropLegacyBackups removes recorded legacy DLL backups so uninstall does not
// restore the runtime being replaced.
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
