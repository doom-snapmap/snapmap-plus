package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func overridesPath(la string, parts ...string) string {
	return filepath.Join(append([]string{la, "snapmap-plus", "overrides"}, parts...)...)
}

func TestStarterPackageUsesAuthoredFormat(t *testing.T) {
	la, _ := newDataDirs(t)
	migrateUserData()
	if err := ensureStarterPackage(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(overridesPath(la, starterPackageName, "package.json"))
	if err != nil {
		t.Fatal(err)
	}
	var descriptor map[string]any
	if err := json.Unmarshal(data, &descriptor); err != nil {
		t.Fatal(err)
	}
	if descriptor["id"] != "local.my-overrides" || descriptor["name"] != "My overrides" {
		t.Fatalf("starter identity missing: %s", data)
	}
	for _, key := range []string{"schema", "version", "priority", "digest", "files"} {
		if _, exists := descriptor[key]; exists {
			t.Fatalf("unexpected author field: %s", key)
		}
	}
	info, err := os.Stat(overridesPath(la, starterPackageName, "assets"))
	if err != nil || !info.IsDir() {
		t.Fatalf("assets directory missing: %v", err)
	}
	entries, err := os.ReadDir(overridesPath(la, starterPackageName))
	if err != nil || len(entries) != 2 {
		t.Fatalf("unexpected starter layout: %v, %v", entries, err)
	}
}

func TestStarterPackagePreservesAuthoredContent(t *testing.T) {
	la, _ := newDataDirs(t)
	marker := overridesPath(la, starterPackageName, "package.json")
	source := overridesPath(la, starterPackageName, "assets", "generated", "decls", "entitydef", "mine.decl")
	original := `{"id":"personal.tools","name":"My custom package","strings":{"en":{"mine":"My label"}}}`
	writeF(t, marker, original)
	writeF(t, source, "MY DECLARATION")
	for i := 0; i < 2; i++ {
		migrateUserData()
		if err := ensureStarterPackage(); err != nil {
			t.Fatal(err)
		}
	}
	if readF(t, marker) != original || readF(t, source) != "MY DECLARATION" {
		t.Fatal("install changed existing authored content")
	}
}

func TestStarterPackageDoesNotReinterpretLooseFiles(t *testing.T) {
	la, _ := newDataDirs(t)
	source := overridesPath(la, "generated", "decls", "file.decl")
	writeF(t, source, "UNCLAIMED")
	migrateUserData()
	if err := ensureStarterPackage(); err != nil {
		t.Fatal(err)
	}
	if readF(t, source) != "UNCLAIMED" {
		t.Fatal("loose content was moved or changed")
	}
	entries, err := os.ReadDir(overridesPath(la, starterPackageName, "assets"))
	if err != nil || len(entries) != 0 {
		t.Fatalf("loose content was silently packaged: %v", err)
	}
}

func TestStarterPackageRefusesUnusablePaths(t *testing.T) {
	la, _ := newDataDirs(t)
	assets := overridesPath(la, starterPackageName, "assets")
	writeF(t, assets, "KEEP")
	if ensureStarterPackage() == nil {
		t.Fatal("file accepted as assets directory")
	}
	if readF(t, assets) != "KEEP" {
		t.Fatal("assets obstruction changed")
	}
	if _, err := os.Stat(overridesPath(la, starterPackageName, "package.json")); !os.IsNotExist(err) {
		t.Fatal("marker published for unusable assets directory")
	}
}

func TestStarterPackagePreservesMarkerDirectory(t *testing.T) {
	la, _ := newDataDirs(t)
	marker := overridesPath(la, starterPackageName, "package.json")
	mkdirAll(t, marker)
	if ensureStarterPackage() == nil {
		t.Fatal("directory accepted as package descriptor")
	}
	if info, err := os.Stat(marker); err != nil || !info.IsDir() {
		t.Fatal("marker obstruction changed")
	}
}
