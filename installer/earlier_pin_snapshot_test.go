package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func frozenTiny(t *testing.T, data string) (string, earlierObject) {
	t.Helper()
	src := filepath.Join(data, "mods", ".builds", "alex.tiny@"+tinyBuild[:16])
	if err := os.MkdirAll(filepath.Dir(src), 0755); err != nil {
		t.Fatal(err)
	}
	if err := copyEarlierPinTree("testdata/earlier-tiny", src); err != nil {
		t.Fatal(err)
	}
	original, err := os.ReadFile(filepath.Join(src, "bundle.canonical.json"))
	if err != nil {
		t.Fatal(err)
	}
	var canonical earlierObject
	if err = json.Unmarshal(original, &canonical); err != nil {
		t.Fatal(err)
	}
	var files []earlierObject
	if err = json.Unmarshal(canonical["files"], &files); err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		f["sha256"] = f["blob"]
		delete(f, "blob")
		delete(f, "request")
	}
	block := earlierObject{
		"format": pinJSON(1), "key": canonical["id"], "build": canonical["build"],
		"validated": pinJSON(true), "bundle": pinJSON(tinyBuild), "files": pinJSON(files),
	}
	doc := earlierObject{"id": canonical["id"], "version": canonical["version"], "name": canonical["name"],
		"author": canonical["author"], "snapmap-plus": pinJSON(block)}
	if err = os.WriteFile(filepath.Join(src, "bundle.json"), pinJSON(doc), 0644); err != nil {
		t.Fatal(err)
	}
	if err = os.Remove(filepath.Join(src, "bundle.canonical.json")); err != nil {
		t.Fatal(err)
	}
	return src, doc
}

func TestEarlierFrozenSnapshotPin(t *testing.T) {
	for _, location := range []string{"mods", "archived", "zip"} {
		t.Run(location, func(t *testing.T) {
			data := t.TempDir()
			src, _ := frozenTiny(t, data)
			var z *zip.Reader
			switch location {
			case "archived":
				for range 2 {
					if err := upgradeModLayout(data); err != nil {
						t.Fatal(err)
					}
				}
			case "zip":
				var buf bytes.Buffer
				out := zip.NewWriter(&buf)
				err := filepath.WalkDir(src, func(path string, entry fs.DirEntry, err error) error {
					if err != nil || entry.IsDir() {
						return err
					}
					rel, err := filepath.Rel(src, path)
					if err != nil {
						return err
					}
					f, err := out.Create("mods/alex.tiny@" + tinyBuild[:16] + "/" + filepath.ToSlash(rel))
					if err != nil {
						return err
					}
					b, err := os.ReadFile(path)
					if err != nil {
						return err
					}
					_, err = f.Write(b)
					return err
				})
				if err != nil {
					t.Fatal(err)
				}
				if err = out.Close(); err != nil {
					t.Fatal(err)
				}
				z, err = zip.NewReader(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
				if err != nil {
					t.Fatal(err)
				}
				data = t.TempDir()
			}
			p := tinyResolver(t, data, z)
			next, err := p.row(tinyPin("mods/alex.tiny/b"))
			if err != nil || string(next["size"]) != "17" || !strings.Contains(string(next["objects"]), "/b") {
				t.Fatalf("%v: %s", err, pinJSON(next))
			}
			if z != nil {
				var output bytes.Buffer
				writer := zip.NewWriter(&output)
				if err := p.addToArchive(writer, map[string]bool{}); err != nil {
					t.Fatal(err)
				}
				if err := writer.Close(); err != nil {
					t.Fatal(err)
				}
			}
		})
	}
}

func TestEarlierFrozenSnapshotRejectsChanges(t *testing.T) {
	for _, failure := range []string{"requested-hash", "claimed-hash", "author", "unvalidated", "format", "asset"} {
		t.Run(failure, func(t *testing.T) {
			data := t.TempDir()
			src, doc := frozenTiny(t, data)
			var block earlierObject
			if err := json.Unmarshal(doc["snapmap-plus"], &block); err != nil {
				t.Fatal(err)
			}
			row := tinyPin()
			switch failure {
			case "requested-hash":
				row["bundle"] = pinJSON(strings.Repeat("1", 64))
			case "claimed-hash":
				block["bundle"] = pinJSON(strings.Repeat("1", 64))
			case "author":
				doc["author"] = pinJSON("changed")
			case "unvalidated":
				block["validated"] = pinJSON(false)
			case "format":
				block["format"] = pinJSON(2)
			case "asset":
				if err := os.WriteFile(filepath.Join(src, "assets/generated/decls/entitydef/mods/alex.tiny/a.decl"), []byte("different"), 0644); err != nil {
					t.Fatal(err)
				}
			}
			doc["snapmap-plus"] = pinJSON(block)
			if err := os.WriteFile(filepath.Join(src, "bundle.json"), pinJSON(doc), 0644); err != nil {
				t.Fatal(err)
			}
			if _, err := tinyResolver(t, data, nil).row(row); err == nil {
				t.Fatal("accepted incompatible earlier snapshot")
			}
			if _, err := os.Stat(filepath.Join(data, "cache")); !os.IsNotExist(err) {
				t.Fatal("failed snapshot published content")
			}
		})
	}
}

func TestUnrelatedBundleJSONAboveFrozenSnapshot(t *testing.T) {
	data := t.TempDir()
	frozenTiny(t, data)
	if err := os.WriteFile(filepath.Join(data, "mods", ".builds", "bundle.json"), []byte("{\"name\":\"different use\"}"), 0644); err != nil {
		t.Fatal(err)
	}
	p := tinyResolver(t, data, nil)
	if err := p.index(); err != nil || len(p.candidates[tinyBuild]) != 1 {
		t.Fatalf("nested exact snapshot lost: %v", err)
	}
}
