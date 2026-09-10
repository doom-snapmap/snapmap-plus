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
// marker. Existing destination files win. Source removal uses fullyMirrored
// path-presence checks, not byte comparison. Failures do not fail installation.
func migrateLegacyOverrides() {
	overrides := overridesDir()
	if overrides == "" {
		return
	}
	legacy := filepath.Join(overrides, legacyOverridesName)
	starter := filepath.Join(overrides, starterPackageName)

	// Missing sources and empty directory trees copy no files.
	moved := copyTreeMissing(legacy, starter)

	if err := os.MkdirAll(starter, 0o755); err != nil {
		return
	}
	marker := filepath.Join(starter, "package.json")
	if _, err := os.Stat(marker); err != nil {
		// Never overwrite: a user may have edited the name or description, and this runs on every update.
		os.WriteFile(marker, []byte(starterPackageJSON), 0o644)
	}

	if _, err := os.Stat(legacy); err != nil {
		return // no legacy tree at all: the starter package above is all that was needed
	}
	if !fullyMirrored(legacy, starter) {
		fmt.Printf("  ! kept overrides\\%s -- %d file(s) were copied to overrides\\%s but not all could be verified\n",
			legacyOverridesName, moved, starterPackageName)
		return
	}
	if os.RemoveAll(legacy) != nil {
		return
	}
	if moved > 0 {
		fmt.Printf("  ~ moved %d override file(s) from overrides\\%s into overrides\\%s\n",
			moved, legacyOverridesName, starterPackageName)
	}
}
