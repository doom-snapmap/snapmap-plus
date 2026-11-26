package main

import (
	"fmt"
	"path/filepath"
	"testing"
)

func TestModCachePinnedFileTypeCollision(t *testing.T) {
	for _, conflict := range []string{"blob", "unit", "shard", "files", "units", "content"} {
		t.Run(conflict, func(t *testing.T) {
			data := t.TempDir()
			old, dst := filepath.Join(data, "mods", ".content"), filepath.Join(data, "cache", "content")
			payload := "a pinned, offline object"
			sha := sha256Hex([]byte(payload))
			blob := filepath.Join("files", sha[:2], sha)
			manifest := fmt.Sprintf("{\"files\":[{\"path\":\"assets/lamp\",\"sha256\":\"%s\",\"size\":%d}]}\n", sha, len(payload))
			unit := filepath.Join("units", sha256Hex([]byte(manifest))+".json")
			writeF(t, filepath.Join(old, blob), payload)
			writeF(t, filepath.Join(old, unit), manifest)
			rel := map[string]string{"blob": blob, "unit": unit, "shard": filepath.Dir(blob), "files": "files", "units": "units", "content": "."}[conflict]
			victim := filepath.Join(dst, rel)
			if conflict == "blob" || conflict == "unit" {
				victim = filepath.Join(victim, "kept")
			}
			writeF(t, victim, "existing conflicting data")
			if err := upgradeModLayout(data); err == nil {
				t.Fatal("conflicting file and directory must not be overwritten")
			}
			for k, w := range map[string]string{blob: payload, unit: manifest} {
				if readF(t, filepath.Join(old, k)) != w {
					t.Fatal("lost original cache content")
				}
			}
			if readF(t, victim) != "existing conflicting data" || exists(filepath.Join(data, "backups")) {
				t.Fatal("displaced conflicting user content")
			}

		})
	}
}

func TestModCacheDigestDoesNotDisplaceACompositionDirectory(t *testing.T) {
	data := t.TempDir()
	payload := "composition identity"
	hash := sha256Hex([]byte(payload))
	rel := filepath.Join("content", "served", hash)
	src := filepath.Join(data, "mods", ".content", "served", hash)
	dst := filepath.Join(data, "cache", rel)
	writeF(t, src, payload)
	writeF(t, filepath.Join(dst, "mod.json"), "existing composition")
	if err := upgradeModLayout(data); err == nil || readF(t, src) != payload || readF(t, filepath.Join(dst, "mod.json")) != "existing composition" {
		t.Fatal("a digest displaced unrelated composition data")
	}
}
