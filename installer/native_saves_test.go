package main

import (
	"bytes"
	"compress/zlib"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func slotBody(t *testing.T, path string, body string) []byte {
	t.Helper()
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

func TestNativeSaveCopies(t *testing.T) {
	data, root := t.TempDir(), t.TempDir()
	slot := filepath.Join(root, "profile", "SNAPMAPSaf01")
	src := filepath.Join(slot, "map.decl")
	body := "{\"entities\":[],\"~type\":\"idSnapMap\",\"snapmap-plus\":{\"format\":1,\"settings\":{\"bounds\":\"1;32671\"}}}"
	original := slotBody(t, src, body)
	details := "metadata={\"name\":\"Hazard Room\"}\r\n"
	writeF(t, filepath.Join(slot, "game.details"), details)
	writeF(t, src+".verify", "native integrity record")
	slotBody(t, filepath.Join(root, "profile2", "SNAPMAPSb", "map.decl"), "{\"~type\":\"idSnapMap\"}")
	slotBody(t, filepath.Join(root, "profile", "SNAPMAPSc", "map.decl"), strings.Replace(body, ":1,", ":2,", 1))

	dir := filepath.Join(data, "rawmaps", "Updated saves")
	writeF(t, filepath.Join(dir, "Hazard Room.json"), "author's existing copy")
	out, err := convertEarlierNativeSaves(data, root)
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
	if again, err := convertEarlierNativeSaves(data, root); err != nil || len(again) != 0 {
		t.Fatalf("repeat %v: %v", again, err)
	}
	writeF(t, out[0], "edit made to the copy")
	again, err := convertEarlierNativeSaves(data, root)
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
	bad := filepath.Join(root, "profile", "SNAPMAPSbad", "map.decl")
	writeF(t, bad, "unexpected data")
	good := filepath.Join(root, "profile", "SNAPMAPSgood", "map.decl")
	original := slotBody(t, good, `{"entities":[],"snapmap-plus":{"format":1,"settings":{"bounds":"1;32671"}}}`)
	paths, err := convertEarlierNativeSaves(data, root)
	if err == nil || len(paths) != 1 || !bytes.Equal(original, []byte(readF(t, good))) || readF(t, bad) != "unexpected data" {
		t.Fatalf("recovery %v: %v", paths, err)
	}
	dir := t.TempDir()
	var wg sync.WaitGroup
	result := make(chan string, 2)
	for i := 0; i < 2; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			p, err := writeNativeCopy(dir, "Shared", []byte("same"))
			if err != nil {
				t.Errorf("concurrent write: %v", err)
			}
			result <- p
		}()
	}
	wg.Wait()
	close(result)
	var published int
	for p := range result {
		if p != "" {
			published++
		}
	}
	if published != 1 {
		t.Fatalf("%d copies published", published)
	}
}
