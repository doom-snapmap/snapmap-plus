package main

import (
	"bytes"
	"compress/zlib"
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func slotBody(t *testing.T, path string, body string) []byte {
	t.Helper()
	id := strings.TrimPrefix(filepath.Base(filepath.Dir(path)), "SNAPMAPS")
	writeF(t, filepath.Join(filepath.Dir(path), "game.details"), "metadata="+string(pinJSON(map[string]string{"localId": id, "name": "Map " + strings.TrimLeft(id, "0")}))+"\n")

	var envelope bytes.Buffer
	envelope.Write([]byte{0x42, 0x11, 0, 0x88})
	z := zlib.NewWriter(&envelope)
	if _, err := z.Write([]byte(body)); err != nil {
		t.Fatal(err)
	}
	if err := z.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, envelope.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
	return envelope.Bytes()
}

func applyNativeMaps(t *testing.T, data, root string) ([]string, error) {
	t.Helper()
	p := &migrationPlan{data: data, work: t.TempDir(), Roots: []string{data}, destinations: map[string]int{}}
	pins := newEarlierPins(data, "", nil)
	defer pins.close()
	p.pins = pins
	err := p.nativeFrom(root, pins)
	var out []string
	for _, c := range p.Changes {
		if filepath.Dir(c.Path) == filepath.Join(data, "rawmaps", "Updated saves") && filepath.Ext(c.Path) == ".json" {
			out = append(out, c.Path)
		}
	}
	if err == nil && p.ready() {
		err = p.commit()
	}
	return out, err
}

func TestNativeSaveCopies(t *testing.T) {
	data, root := t.TempDir(), t.TempDir()
	slot := filepath.Join(root, "701", "SNAPMAPS0000000000000000af01")
	src := filepath.Join(slot, "map.decl")
	body := "{\"entities\":[],\"~type\":\"idSnapMap\",\"snapmap-plus\":{\"format\":1,\"settings\":{\"bounds\":\"1;32671\"}}}"
	original := slotBody(t, src, body)
	details := "metadata={\"localId\":\"0000000000000000af01\",\"name\":\"Hazard Room\"}\r\n"
	writeF(t, filepath.Join(slot, "game.details"), details)
	writeF(t, src+".verify", "native integrity record")
	slotBody(t, filepath.Join(root, "702", "SNAPMAPS0000000000000000000b", "map.decl"), "{\"~type\":\"idSnapMap\"}")
	slotBody(t, filepath.Join(root, "701", "SNAPMAPS0000000000000000000c", "map.decl"), strings.Replace(body, ":1,", ":2,", 1))

	dir := filepath.Join(data, "rawmaps", "Updated saves")
	writeF(t, filepath.Join(dir, "Hazard Room.json"), "author's existing copy")
	out, err := applyNativeMaps(t, data, root)
	if err != nil || len(out) != 1 || filepath.Base(out[0]) != "Hazard Room (2).json" {
		t.Fatalf("saved %v: %v", out, err)
	}
	var current map[string]json.RawMessage
	if err = json.Unmarshal([]byte(readF(t, out[0])), &current); err != nil || !bytes.Contains(current["snapmap-plus"], []byte(`"format":2`)) {
		t.Fatalf("converted contents %s: %v", current, err)
	}
	if readF(t, src) != string(original) || readF(t, filepath.Join(slot, "game.details")) != details || readF(t, src+".verify") != "native integrity record" {
		t.Fatal("the native slot changed")
	}
	if readF(t, filepath.Join(dir, "Hazard Room.json")) != "author's existing copy" {
		t.Fatal("existing rawmap changed")
	}
	if again, err := applyNativeMaps(t, data, root); err != nil || len(again) != 0 {
		t.Fatalf("repeat %v: %v", again, err)
	}
	writeF(t, out[0], "edit made to the copy")
	again, err := applyNativeMaps(t, data, root)
	if err != nil || len(again) != 1 || filepath.Base(again[0]) != "Hazard Room (3).json" || readF(t, out[0]) != "edit made to the copy" {
		t.Fatalf("edited copy %v: %v", again, err)
	}
}

func TestNativeSaveScanAndConcurrentCopies(t *testing.T) {
	const marker = "\"snapmap-plus\""
	for cut := 0; cut <= len(marker); cut++ {
		var scan nativeMapMarker
		scan.Write([]byte("before" + marker[:cut]))
		scan.Write([]byte(marker[cut:] + "after"))
		if !scan.found {
			t.Fatalf("lost split key at %d", cut)
		}
	}
	data, root := t.TempDir(), t.TempDir()
	bad := filepath.Join(root, "701", "SNAPMAPS00000000000000000bad", "map.decl")
	slotBody(t, bad, "{}")
	writeF(t, bad, "unexpected data")
	good := filepath.Join(root, "701", "SNAPMAPS0000000000000000face", "map.decl")
	original := slotBody(t, good, `{"entities":[],"snapmap-plus":{"format":1,"settings":{"bounds":"1;32671"}}}`)
	paths, err := applyNativeMaps(t, data, root)
	if err == nil || len(paths) != 1 || !bytes.Equal(original, []byte(readF(t, good))) || readF(t, bad) != "unexpected data" {
		t.Fatalf("recovery %v: %v", paths, err)
	}
	dir := t.TempDir()
	var wg sync.WaitGroup
	result := make(chan error, 2)
	dest := filepath.Join(dir, "Shared.json")
	for _, name := range []string{"first", "second"} {
		src := filepath.Join(dir, name)
		writeF(t, src, "same")
		wg.Add(1)
		go func() { defer wg.Done(); result <- publishNativeCopy(src, dest) }()
	}
	wg.Wait()
	close(result)
	var published int
	for err := range result {
		if err == nil {
			published++
		} else if !errors.Is(err, fs.ErrExist) {
			t.Fatal(err)
		}
	}
	if published != 1 || readF(t, dest) != "same" {
		t.Fatalf("%d concurrent copies", published)
	}
}

func TestNativeMapDirectoryMatchesItsOwnerRecord(t *testing.T) {
	data, root := t.TempDir(), t.TempDir()
	const id = "0123456789ABCDEF0123"
	const body = `{"entities":[],"snapmap-plus":{"format":1,"settings":{"bounds":"1;32671"}}}`
	for _, tc := range []struct{ account, folder, record, title string }{
		{"701", "SNAPMAPS" + id, strings.ToLower(id), "Live local save"},
		{"702", "SNAPMAPS" + id, id, "Other local account"},
		{"703", "SNAPMAPS" + id, "FFFFFFFFFFFFFFFFFFFF", "Copied record for a different slot"},
		{"704", "SNAPMAPS" + id, "", "No catalog identity"},
		{"705", "SNAPMAPS12", "12", "Short slot name"},
		{"backup", "SNAPMAPS" + id, id, "Old profile backup"},
	} {
		dir := filepath.Join(root, tc.account, tc.folder)
		slotBody(t, filepath.Join(dir, "map.decl"), body)
		writeF(t, filepath.Join(dir, "game.details"), "metadata="+string(pinJSON(map[string]string{"localId": tc.record, "name": tc.title}))+"\n")
	}
	slot := filepath.Join(root, "706", "SNAPMAPS"+id, "map.decl")
	slotBody(t, slot, body)
	if err := os.Remove(filepath.Join(filepath.Dir(slot), "game.details")); err != nil {
		t.Fatal(err)
	}
	out, err := applyNativeMaps(t, data, root)
	if err != nil || len(out) != 2 {
		t.Fatalf("local slots: %v, %v", out, err)
	}
	if filepath.Base(out[0]) != "Live local save.json" || filepath.Base(out[1]) != "Other local account.json" {
		t.Fatal(out)
	}
}
