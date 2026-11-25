package main

import (
	"encoding/json"
	"path/filepath"
	"testing"
)

func TestRemoveLeftovers(t *testing.T) {
	local, _ := newDataDirs(t)
	data := filepath.Join(local, "snapmap-plus")
	doom := t.TempDir()
	name := "snapmap-plus-store.exe"
	tool := filepath.Join(data, name)
	writeF(t, tool, "retired installer")
	sum, err := fileSHA256(tool)
	if err != nil {
		t.Fatal(err)
	}
	old, _ := json.Marshal(map[string]string{name: sum})
	writeF(t, filepath.Join(data, retiredExecutableRecord), string(old))
	writeF(t, filepath.Join(data, retiredVersionsRecord), "{}")
	writeF(t, filepath.Join(data, selfExeAsset), "current installer")
	writeF(t, filepath.Join(data, selfExeAsset+".old"), "old installer")
	writeF(t, filepath.Join(data, selfExeAsset+".old-notes"), "keep")
	writeF(t, filepath.Join(data, name+".old"), "old image")
	writeF(t, filepath.Join(data, "package-staging", "mine"), "keep")
	writeF(t, filepath.Join(doom, "snaphak_logs", "old.log"), "old")
	writeF(t, filepath.Join(doom, "snapmap-plus", "logs", "current.log"), "keep")
	removeLeftovers(doom, data, "")
	for _, p := range []string{tool, tool + ".old", filepath.Join(data, retiredExecutableRecord), filepath.Join(data, retiredVersionsRecord),
		filepath.Join(data, selfExeAsset+".old"), filepath.Join(doom, "snaphak_logs")} {
		if exists(p) {
			t.Errorf("still present: %s", p)
		}
	}
	if stateExists(data) {
		t.Fatal("cleanup created an install record")
	}
	writeF(t, tool, "player file")
	if err := saveExecutableRecord(data, executableRecord{name: sum}); err != nil {
		t.Fatal(err)
	}
	removeLeftovers(doom, data, "")
	removeLeftovers(doom, data, "")
	if readF(t, tool) != "player file" || readF(t, filepath.Join(data, "package-staging", "mine")) != "keep" ||
		readF(t, filepath.Join(data, selfExeAsset+".old-notes")) != "keep" || readF(t, filepath.Join(doom, "snapmap-plus", "logs", "current.log")) != "keep" {
		t.Fatal("lost unrelated files")
	}
	rec, err := loadStateAt(data)
	if err != nil || len(rec.Executables) != 0 {
		t.Fatalf("stale ownership: %v %v", rec, err)
	}
	missing := filepath.Join(t.TempDir(), "downloader.exe")
	writeF(t, missing+".old", "only image")
	removeLeftovers("", data, missing)
	if readF(t, missing+".old") != "only image" {
		t.Fatal("lost the only image")
	}
}
