//go:build !windows

package main

import (
	"os"
	"path/filepath"
)

func nativeSaveRoot() (string, error) {
	profile := os.Getenv("USERPROFILE")
	if profile == "" {
		var err error
		profile, err = os.UserHomeDir()
		if err != nil {
			return "", err
		}
	}
	return filepath.Join(profile, "Saved Games", "id Software", "DOOM", "base", "savegame.user"), nil
}

func publishNativeCopy(tmp, dest string) error { return os.Link(tmp, dest) }
