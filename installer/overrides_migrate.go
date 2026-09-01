package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// Before override packages existed, everything a user overrode lived in one shared tree,
// overrides\generated\{decls,resources,requirements}. The backend used to special-case that folder as a
// package with no package.json so those installs kept working. That left two rules where one would do, and
// the shared tree has the problems packages exist to solve: two mods land in the same folders, uninstalling
// one means knowing which files were its, and nothing carries a name the diagnostics can blame.
//
// So the installer migrates it, once, into a real package. What that does NOT change is which bytes the
// engine can be served: the file shadow resolves every engine resource name against the overrides root
// directly (overrides\<engine name>), which is a separate path from package resolution and is untouched --
// dropping a file at overrides\<name> still shadows that resource. What the package adds is publishing
// identities DOOM never shipped, and uninstalling by deleting one folder.

// starterPackageName is where a user's own content lives: the destination of the migration above, and the
// folder a fresh install gets so there is somewhere obvious to drop things.
const starterPackageName = "my-overrides"

// legacyOverridesName is the pre-package shared tree this migration retires.
const legacyOverridesName = "generated"

// starterPackageJSON marks the folder as a package. Nothing parses it -- the backend only checks that the
// file exists -- so this deliberately carries no `contents` hash map: those belong to a package someone
// distributes, and would be stale the moment the user edited their own files.
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

// migrateLegacyOverrides moves overrides\generated into overrides\my-overrides and marks it as a package,
// then makes sure that starter package exists whether or not there was anything to migrate.
//
// The move is VERIFIED, matching migrateUserData: files are copied without ever overwriting, and the source
// is removed only once every one of its files is confirmed present at the destination. A user who already
// has a my-overrides package keeps their copy of any colliding file. Best-effort throughout -- this never
// fails an install.
func migrateLegacyOverrides() {
	overrides := overridesDir()
	if overrides == "" {
		return
	}
	legacy := filepath.Join(overrides, legacyOverridesName)
	starter := filepath.Join(overrides, starterPackageName)

	// copyTreeMissing tolerates a missing source (it copies nothing), so this needs no separate existence
	// check -- and a legacy tree holding only empty directories correctly reports zero files moved.
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
