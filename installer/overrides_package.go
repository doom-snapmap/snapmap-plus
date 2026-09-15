package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// starterPackageName is the editable fresh-install package folder.
const starterPackageName = "my-overrides"

// Authored packages have one descriptor and an engine-shaped assets tree.
const starterPackageJSON = `{
  "id": "local.my-overrides",
  "name": "My overrides",
  "description": "Your own overrides. Copy extracted game paths into assets/. Keep requirements, strings and other package policy in package.json."
}
`

func overridesDir() string {
	dir := appDataDir()
	if dir == "" {
		return ""
	}
	return filepath.Join(dir, "overrides")
}

// Existing descriptors and authored files remain exactly as the user left them.
func ensureStarterPackage() error {
	overrides := overridesDir()
	if overrides == "" {
		return nil
	}
	starter := filepath.Join(overrides, starterPackageName)
	assets := filepath.Join(starter, "assets")
	if !plainPath(starter) || !plainPath(assets) {
		return fmt.Errorf("starter package uses a linked path")
	}
	if err := os.MkdirAll(assets, 0o755); err != nil {
		return err
	}
	return ensureStarterMarker(filepath.Join(starter, "package.json"))
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
