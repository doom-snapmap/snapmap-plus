package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// Migrate the old shared overrides/generated layout into a marked package.
// Loose root-level shadows remain a separate backend lookup path.

// starterPackageName is the migration destination and the fresh-install starter folder.
const starterPackageName = "my-overrides"

// legacyOverridesName is the pre-package shared tree this migration retires.
const legacyOverridesName = "generated"

// starterPackageJSON marks editable user content. Omit a contents digest because
// the user changes these files after installation.
const starterPackageJSON = `{
  "schema": "snapmap-plus.override-package.v1",
  "name": "my-overrides",
  "version": "1.0.0",
  "description": "Your own overrides. Decls go under decls/<type>/<name>.decl, resource manifests under resources/, and cvar requirements under requirements/."
}
`

func overridesDir() string {
	dir := appDataDir()
	if dir == "" {
		return ""
	}
	return filepath.Join(dir, "overrides")
}

// migrateLegacyOverrides copies missing files into my-overrides and creates its
// marker. Conflicting or unverifiable source files stay in the legacy folder.
func migrateLegacyOverrides() {
	overrides := overridesDir()
	if overrides == "" {
		return
	}
	legacy := filepath.Join(overrides, legacyOverridesName)
	starter := filepath.Join(overrides, starterPackageName)

	// Missing sources and empty directory trees copy no files.
	moved, copyErr := copyTreeMissing(legacy, starter)

	if err := os.MkdirAll(starter, 0o755); err != nil {
		return
	}
	marker := filepath.Join(starter, "package.json")
	if err := ensureStarterMarker(marker); err != nil {
		fmt.Printf("  ! kept overrides\\%s -- could not create the destination package marker: %v\n", legacyOverridesName, err)
		return
	}

	if _, err := os.Stat(legacy); err != nil {
		return // no legacy tree at all: the starter package above is all that was needed
	}
	if copyErr != nil || !fullyMirrored(legacy, starter) {
		fmt.Printf("  ! kept overrides\\%s -- %d file(s) were copied to overrides\\%s but not all could be verified\n",
			legacyOverridesName, moved, starterPackageName)
		return
	}
	if removeMirroredTree(legacy, starter) != nil {
		return
	}
	if moved > 0 {
		fmt.Printf("  ~ moved %d override file(s) from overrides\\%s into overrides\\%s\n",
			moved, legacyOverridesName, starterPackageName)
	}
}

func ensureStarterMarker(path string) error {
	if !plainPath(path) {
		return fmt.Errorf("package marker uses a linked path")
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if os.IsExist(err) {
		info, err := os.Lstat(path)
		if err == nil && !info.Mode().IsRegular() {
			return fmt.Errorf("package marker is not a regular file")
		}
		return err
	}
	if err != nil {
		return err
	}
	_, writeErr := f.WriteString(starterPackageJSON)
	closeErr := f.Close()
	if writeErr != nil || closeErr != nil {
		os.Remove(path)
		if writeErr != nil {
			return writeErr
		}
		return closeErr
	}
	return nil
}
