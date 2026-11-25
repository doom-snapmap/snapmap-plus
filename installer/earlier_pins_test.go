package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const tinyBuild = "d199e9576a74b1e648f7b971bcb09f705ba1fd1a0ec0c6099b0050e65e305f45"

func tinyPin(names ...string) earlierObject {
	return earlierObject{"id": pinJSON("alex.tiny"), "version": pinJSON("1.0.0"), "build": pinJSON(1), "bundle": pinJSON(tinyBuild), "size": pinJSON(34), "names": pinJSON(names), "choices": pinJSON(map[string]string{"color": "blue"})}
}
func tinyFixture(t *testing.T, data string) string {
	t.Helper()
	mod := filepath.Join(data, "mods", "alex.tiny@1.0.0+1")
	if err := os.MkdirAll(filepath.Dir(mod), 0755); err != nil {
		t.Fatal(err)
	}
	if err := copyEarlierPinTree("testdata/earlier-tiny", mod); err != nil {
		t.Fatal(err)
	}
	return mod
}
func tinyResolver(t *testing.T, data string, z *zip.Reader) *earlierPins {
	t.Helper()
	p := newEarlierPins(data, os.Getenv("SNAPMAP_TEST_DOOM"), z)
	t.Cleanup(p.close)
	if len(earlierUnitBinary) == 0 {
		tool, e := filepath.Abs("../tests/obj/unit_cli.exe")
		if e != nil {
			t.Fatal(e)
		}
		if !isRegular(tool) {
			t.Skip("build the native unit helper with tests/run-tests.ps1 -Only unit_cli")
		}
		p.tool = tool
	}
	return p
}
func TestEarlierPinExactObjectAndFallback(t *testing.T) {
	data := t.TempDir()
	tinyFixture(t, data)
	p := tinyResolver(t, data, nil)
	row, e := p.row(tinyPin("mods/alex.tiny/a"))
	if e != nil {
		t.Fatal(e)
	}
	var objects map[string]string
	_ = json.Unmarshal(row["objects"], &objects)
	if len(objects) != 1 || row["whole"] != nil || string(row["size"]) != "17" || row["choices"] == nil {
		t.Fatalf("wrong object row: %s", pinJSON(row))
	}
	for _, h := range objects {
		if !isRegular(filepath.Join(data, "cache", "content", "units", h+".json")) {
			t.Fatal(h)
		}
	}
	req := tinyPin("mods/alex.tiny/a")
	req["attached"] = pinJSON(true)
	row, e = p.row(req)
	if e != nil {
		t.Fatal(e)
	}
	if row["whole"] == nil || string(row["size"]) != "34" {
		t.Fatalf("wrong attached row: %s", pinJSON(row))
	}
	row, e = p.row(tinyPin("mods/alex.tiny/absent"))
	if e != nil {
		t.Fatal(e)
	}
	if row["whole"] == nil || string(row["objects"]) != "{}" {
		t.Fatalf("wrong fallback: %s", pinJSON(row))
	}
}
func TestEarlierPinNeedsOriginalAndVerifiedFiles(t *testing.T) {
	data := t.TempDir()
	src := tinyFixture(t, data)
	p := tinyResolver(t, data, nil)
	row := tinyPin()
	row["bundle"] = pinJSON(strings.Repeat("f", 64))
	if _, e := p.row(row); e == nil {
		t.Fatal("substituted a different release")
	}
	if e := os.WriteFile(filepath.Join(src, "assets/generated/decls/entitydef/mods/alex.tiny/a.decl"), []byte("changed"), 0644); e != nil {
		t.Fatal(e)
	}
	if _, e := p.row(tinyPin()); e == nil {
		t.Fatal("accepted changed asset")
	}
	if _, e := os.Stat(filepath.Join(data, "cache")); !os.IsNotExist(e) {
		t.Fatal("failed pin published content")
	}
}
func tinyZIP(t *testing.T, extra string) *zip.Reader {
	var buf bytes.Buffer
	w := zip.NewWriter(&buf)
	const prefix = "mods/alex.tiny@1.0.0+1/"
	err := filepath.WalkDir("testdata/earlier-tiny", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		b, e := os.ReadFile(p)
		if e != nil {
			return e
		}
		rel, _ := filepath.Rel("testdata/earlier-tiny", p)
		f, e := w.Create(prefix + filepath.ToSlash(rel))
		if e != nil {
			return e
		}
		_, e = f.Write(b)
		return e
	})
	if err != nil {
		t.Fatal(err)
	}
	if extra != "" {
		f, e := w.Create(extra)
		if e != nil {
			t.Fatal(e)
		}
		f.Write([]byte("original only"))
	}
	if e := w.Close(); e != nil {
		t.Fatal(e)
	}
	z, e := zip.NewReader(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
	if e != nil {
		t.Fatal(e)
	}
	return z
}
func TestEarlierPinArchiveCompleteness(t *testing.T) {
	for _, extra := range []string{"", "mods/alex.tiny@1.0.0+1/unverified.txt"} {
		data := t.TempDir()
		p := tinyResolver(t, data, tinyZIP(t, extra))
		if _, e := p.row(tinyPin("mods/alex.tiny/a")); e != nil {
			t.Fatal(e)
		}
		var out bytes.Buffer
		w := zip.NewWriter(&out)
		e := p.addToArchive(w, nil)
		if (extra == "") != (e == nil) {
			t.Fatalf("archive %q: %v", extra, e)
		}
		w.Close()
		if extra == "" {
			z, e := zip.NewReader(bytes.NewReader(out.Bytes()), int64(out.Len()))
			if e != nil || len(z.File) != 2 {
				t.Fatalf("exported entries: %v %v", z, e)
			}
		}
	}
}

func TestEarlierSaltedPins(t *testing.T) {
	const salt = "ac97d421"
	for _, assets := range []bool{true, false} {
		data := t.TempDir()
		dir := filepath.Join(data, "mods", "salted")
		if err := os.MkdirAll(filepath.Join(dir, "entities"), 0755); err != nil {
			t.Fatal(err)
		}
		p := tinyResolver(t, data, nil)
		payload := []byte("{\n\tsize = \"1\";\n}\n")
		id := map[string]any{"format": 2, "salt": salt}
		author := map[string]any{"name": "Salted", "snapmap-plus": id}
		makeFile := func(rel string, b []byte) {
			out := filepath.Join(dir, filepath.FromSlash(rel))
			if err := os.MkdirAll(filepath.Dir(out), 0755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(out, b, 0644); err != nil {
				t.Fatal(err)
			}
		}
		var hash string
		if assets {
			route := "generated/decls/entitydef/mods/" + salt + "/a.decl"
			rel := "assets/" + route
			sha := fmt.Sprintf("%x", sha256.Sum256(payload))
			provided := map[string]string{"name": "mods/" + salt + "/a", "type": "entityDef"}
			file := map[string]any{"blob": sha, "needs": []any{}, "path": rel, "provides": provided, "request": route, "size": len(payload)}
			canonical := pinJSON(map[string]any{"files": []any{file}, "format": 2, "id": salt, "name": "Salted"})
			hash = fmt.Sprintf("%x", sha256.Sum256(canonical))
			id["build"] = hash
			id["files"] = []any{map[string]any{"sha256": sha, "needs": []any{}, "path": rel, "provides": provided, "size": len(payload)}}
			makeFile(rel, payload)
			makeFile("mod.json", pinJSON(author))
		} else {
			makeFile("mod.json", pinJSON(author))
			makeFile("entities/a.decl", payload)
			if err := p.ready(); err != nil {
				t.Fatal(err)
			}
			// The native builder records the exact canonical hash in this test folder's sidecar.
			out, e := p.run(dir, "mod=.", "out=.test-output", "names=*")
			if e != nil {
				t.Fatalf("build: %v %s", e, out)
			}
			raw, e := os.ReadFile(filepath.Join(dir, ".build", "manifest.json"))
			if e != nil {
				t.Fatal(e)
			}
			var block earlierObject
			_ = json.Unmarshal(raw, &block)
			hash = pinString(block, "build")
		}
		row := earlierObject{"mod": pinJSON(salt + "@" + hash), "names": pinJSON([]string{"mods/" + salt + "/a"})}
		next, e := p.row(row)
		if e != nil {
			t.Fatalf("assets=%v: %v", assets, e)
		}
		if next["whole"] != nil || string(next["size"]) != "17" {
			t.Fatalf("%v: %s", assets, pinJSON(next))
		}
		if assets {
			author["name"] = "Changed"
			makeFile("mod.json", pinJSON(author))
			other := tinyResolver(t, data, nil)
			if _, e = other.row(row); e == nil {
				t.Fatal("accepted an edited descriptor")
			}
		}
	}
}

func TestEarlierPreSaltAuthorBuild(t *testing.T) {
	data := t.TempDir()
	src := tinyFixture(t, data)
	raw, err := os.ReadFile(filepath.Join(src, "bundle.canonical.json"))
	if err != nil {
		t.Fatal(err)
	}
	var canonical earlierObject
	if err = json.Unmarshal(raw, &canonical); err != nil {
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
	doc := earlierObject{"id": canonical["id"], "version": canonical["version"], "name": canonical["name"], "author": canonical["author"], "snapmap-plus": pinJSON(earlierObject{"build": pinJSON(1), "bundle": pinJSON(tinyBuild), "files": pinJSON(files)})}
	if err = os.WriteFile(filepath.Join(src, "mod.json"), pinJSON(doc), 0644); err != nil {
		t.Fatal(err)
	}
	for _, leaf := range []string{"bundle.canonical.json", "bundle.json"} {
		if err = os.Remove(filepath.Join(src, leaf)); err != nil {
			t.Fatal(err)
		}
	}
	p := tinyResolver(t, data, nil)
	next, err := p.row(tinyPin("mods/alex.tiny/b"))
	if err != nil {
		t.Fatal(err)
	}
	if string(next["size"]) != "17" || !strings.Contains(string(next["objects"]), "/b") {
		t.Fatalf("incorrect row %s", pinJSON(next))
	}
}
