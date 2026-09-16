//go:build !windows

package main

import "sync"

// Non-Windows builds exercise the portable converter and synthetic fixtures;
// deployment and authoring-library conversion run on Windows.
var overrideMigrationMutex sync.Mutex

func lockOverrideMigration(root string) (func(), error) {
	overrideMigrationMutex.Lock()
	return overrideMigrationMutex.Unlock, nil
}
