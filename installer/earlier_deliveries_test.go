package main

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestVanillaExportPreservesInstallerState(t *testing.T) {
	work := t.TempDir()
	t.Setenv("LOCALAPPDATA", filepath.Join(work, "app"))
	t.Setenv("USERPROFILE", filepath.Join(work, "player"))
	room := filepath.Join(work, "stock.json")
	writeF(t, room, `{"~type":"idSnapMap","entities":[]}`)
	data := appDataDir()
	if err := convertWithConsent(data, room, true); err != nil || earlierPathExists(data) {
		t.Fatalf("checking a vanilla export with no profile: %v", err)
	}
	record := statePath(data)
	writeF(t, record, "{damaged")
	writeF(t, record+unreadableSuffix, "older record")
	if err := convertWithConsent(data, room, true); err != nil {
		t.Fatal(err)
	}
	if readF(t, record) != "{damaged" || readF(t, record+unreadableSuffix) != "older record" {
		t.Fatal("checking a vanilla export changed the installer state")
	}
}

func TestMergeEarlierDeliveryFiles(t *testing.T) {
	first, second, joined := t.TempDir(), t.TempDir(), t.TempDir()
	for _, folder := range []string{first, second} {
		writeF(t, filepath.Join(folder, "entities/common.decl"), "{ }")
	}
	writeF(t, filepath.Join(first, "entities/left.decl"), "left")
	writeF(t, filepath.Join(second, "entities/right.decl"), "right")
	for _, folder := range []string{first, second} {
		if err := copyEarlierPinTree(folder, joined); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{"left", "right"} {
		if readF(t, filepath.Join(joined, "entities", name+".decl")) != name {
			t.Fatal(name)
		}
	}
	writeF(t, filepath.Join(second, "entities/common.decl"), "{ different }")
	err := copyEarlierPinTree(second, joined)
	if err == nil || !strings.Contains(err.Error(), "common.decl") || readF(t, filepath.Join(joined, "entities/common.decl")) != "{ }" {
		t.Fatalf("conflicting declarations: %v", err)
	}
}

func TestEarlierModelMaterialNotes(t *testing.T) {
	source, author := t.TempDir(), filepath.Join(t.TempDir(), "author")
	const note = "assets/cooked/model/models/example.materials.txt"
	const body = "materials/metal\r\n"
	writeF(t, filepath.Join(source, "package.json"), `{"id":"local.door","name":"Door"}`)
	writeF(t, filepath.Join(source, "assets/generated/decls/entitydef/mods/local.door/a.decl"), "{ }")
	writeF(t, filepath.Join(source, note), body)
	consumed := map[string]bool{}
	if _, err := translateEarlierAuthor(source, author, nil, consumed, nil); err != nil {
		t.Fatal(err)
	}
	if err := earlierExtraFiles(source, author, consumed); err != nil {
		t.Fatal(err)
	}
	if readF(t, filepath.Join(author, note)) != body || earlierPathExists(filepath.Join(author, "native", strings.TrimPrefix(note, "assets/"))) {
		t.Fatal("earlier model notes must survive folder migration as author files")
	}
	writeF(t, filepath.Join(source, "resources", "declared.manifest"), "request\t"+note+"\n")
	files, err := earlierPackageFiles(source, map[string]any{}, nil)
	if err != nil || files[strings.TrimPrefix(note, "assets/")] == "" {
		t.Fatalf("explicitly listed material report lost: %v", err)
	}
}
