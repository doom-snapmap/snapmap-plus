package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func oneStockRequest(t *testing.T, root, request string) {
	t.Helper()
	var index bytes.Buffer
	index.Write(make([]byte, 0x24))
	index.Write(make([]byte, 4))
	for _, word := range []string{"entityDef", request, request} {
		binary.Write(&index, binary.LittleEndian, uint32(len(word)))
		index.WriteString(word)
	}
	fixed := make([]byte, 21)
	binary.BigEndian.PutUint32(fixed[8:], 2)
	binary.BigEndian.PutUint32(fixed[12:], 2)
	index.Write(fixed)
	data := index.Bytes()
	copy(data, []byte{5, 'S', 'E', 'R'})
	binary.BigEndian.PutUint32(data[4:], uint32(len(data)-0x20))
	binary.BigEndian.PutUint32(data[0x20:], 1)
	writeF(t, filepath.Join(root, "base/gameresources.pindex"), string(data))
	writeF(t, filepath.Join(root, "base/gameresources.resources"), "{}")
}

func TestEarlierGlobalSnapshotPlacesItsOwnEntity(t *testing.T) {
	data := t.TempDir()
	retail := filepath.Join(data, "retail")
	const old = "z/future"
	const stock = "z/stock"
	const id = "future.owner"
	oneStockRequest(t, retail, "generated/decls/entitydef/"+stock+".decl")
	src := filepath.Join(data, "mods", ".builds", "frozen")
	var files []earlierObject
	for _, name := range []string{old, stock} {
		request := "generated/decls/entitydef/" + name + ".decl"
		contents := "{}"
		if name == stock {
			contents = "{ edit = { entityDef = \"" + old + "\"; } }"
		}
		writeF(t, filepath.Join(src, "assets", filepath.FromSlash(request)), contents)
		files = append(files, earlierObject{"path": pinJSON("assets/" + request), "request": pinJSON(request), "size": pinJSON(len(contents)), "blob": pinJSON(fmt.Sprintf("%x", sha256.Sum256([]byte(contents)))), "needs": pinJSON([]string{}), "provides": pinJSON(map[string]string{"type": "entityDef", "name": name})})
	}
	canonical := pinJSON(earlierObject{"format": pinJSON(1), "id": pinJSON(id), "name": pinJSON("Future"), "version": pinJSON("1.0"), "build": pinJSON(1), "files": pinJSON(files), "extra": pinJSON(map[string]any{"legacy": map[string]string{"source": "overrides"}})})
	writeF(t, filepath.Join(src, "bundle.canonical.json"), string(canonical))
	pin := fmt.Sprintf("%x", sha256.Sum256(canonical))
	row := earlierObject{"id": pinJSON(id), "version": pinJSON("1.0"), "build": pinJSON(1), "bundle": pinJSON(pin), "names": pinJSON([]string{old})}
	input := pinJSON(map[string]any{"~type": "idSnapMap", "entities": []any{map[string]any{"entityDef": map[string]any{"inherit": old, "edit": map[string]any{"displayName": old}}}, map[string]any{"entityDef": map[string]any{"inherit": stock}}}, "snapmap-plus": map[string]any{"format": 1, "mods": []earlierObject{row}}})
	p := tinyResolver(t, data, nil)
	p.doom = retail
	next, changed, err := convertEarlierPinnedJSON(input, nil, p)
	if err != nil || !changed {
		t.Fatalf("exact carrier: %v", err)
	}
	var out struct {
		Entities []struct {
			EntityDef struct {
				Inherit string
				Edit    map[string]string
			}
		}
		Block json.RawMessage `json:"snapmap-plus"`
	}
	if json.Unmarshal(next, &out) != nil || len(out.Entities) != 2 || out.Entities[0].EntityDef.Inherit != "mods/"+earlierSalt(id)+"/"+old || out.Entities[0].EntityDef.Edit["displayName"] != old || out.Entities[1].EntityDef.Inherit != stock || !bytes.Contains(out.Block, []byte("entities/"+old)) {
		t.Fatal(string(next))
	}
	staged := p.candidates[pin][0].staged
	want := filepath.Join(staged, "mod/native/generated/decls/entitydef", stock+".decl")
	if b, err := os.ReadFile(want); err != nil || !strings.Contains(string(b), "entities/"+old) {
		t.Fatalf("stock edit: %s %v", b, err)
	}
	row["bundle"] = pinJSON(strings.Repeat("1", 64))
	if _, err = p.row(row); err == nil {
		t.Fatal("accepted a foreign pin")
	}
}

func TestEarlierGlobalKindsAndOwners(t *testing.T) {
	var names earlierGlobalNames
	const bare = "shared/icon"
	for _, kind := range []string{"material", "image"} {
		ref := kind + "s/" + bare
		if e := names.add(earlierGlobalName{kind, bare, ref, ref, "mods/a1234567/" + bare, "one"}); e != nil {
			t.Fatal(e)
		}
	}
	if e := names.add(earlierGlobalName{"image", bare, "other", "other", "mods/b1234567/" + bare, "two"}); e == nil {
		t.Fatal("ambiguous image owner")
	}
	in := "{ previewImage = \"shared/icon\"; displayName = \"shared/icon\"; // shared/icon\n }"
	out, e := names.decl(in, "snapeditorentitydef")
	if e != nil || out != strings.Replace(in, "previewImage = \"shared/icon\"", "previewImage = \"materials/shared/icon\"", 1) {
		t.Fatalf("%s %v", out, e)
	}
	out, e = names.decl("{ transmap shared/icon.tga }", "material")
	if e != nil || !strings.Contains(out, "images/shared/icon.tga") {
		t.Fatalf("%s %v", out, e)
	}
	if _, e = names.decl("{ unknown = \"shared/icon\"; }", "fx"); e == nil {
		t.Fatal("untyped collision guessed")
	}
	if names.opaqueFile([]byte("\x00ShArEd/IcOn\x00")) == nil || names.opaqueFile([]byte("other bytes")) != nil {
		t.Fatal("opaque embedded reference")
	}
}

func TestEarlierAddedSkinnedMesh(t *testing.T) {
	data := t.TempDir()
	stock := "generated/basemodel/md6/original/stock.bmd6model"
	game := filepath.Join(data, "game")
	oneStockRequest(t, game, stock)
	p := tinyResolver(t, data, nil)
	p.doom = game
	const id = "future.owner"
	const req = "generated/basemodel/md6/old/port.bmd6model"
	const name = "md6/old/port.md6mesh"
	const ref = "meshes/old/port.md6mesh"
	src := filepath.Join(data, "original")
	writeF(t, filepath.Join(src, "mod.json"), `{"id":"future.owner","name":"Port"}`)
	opaque := "\x00original native payload\xff"
	writeF(t, filepath.Join(src, "assets", strings.ToUpper(req)), opaque)
	writeF(t, filepath.Join(src, "assets", stock), "unchanged original")
	const model = "generated/decls/md6def/old/rig.md6.decl"
	writeF(t, filepath.Join(src, "assets", model), `{ init { mesh "MD6/OLD/PORT.MD6MESH" } }`)
	var discovered earlierGlobalNames
	discover := func(id string, names []string) (earlierGlobalNames, error) {
		namesFound, err := p.sourceGlobalNames(id, data, names)
		discovered = namesFound
		return namesFound, err
	}
	dest := filepath.Join(data, "converted")
	if _, err := translateEarlierFiles(src, dest, earlierGlobalNames{}, nil, nil, discover, ""); err != nil {
		t.Fatal(err)
	}
	if got := readF(t, filepath.Join(dest, "meshes/old/port.bmd6model")); got != opaque {
		t.Fatal("skinned model bytes changed")
	}
	if got := readF(t, filepath.Join(dest, "native", stock)); got != "unchanged original" {
		t.Fatal("installed game model was relocated")
	}
	if got := readF(t, filepath.Join(dest, "meshes/old/rig.md6.decl")); got != `{ init { mesh "`+ref+`" } }` {
		t.Fatal(got)
	}
	add := discovered.files[req]
	if add.engine != "md6/mods/"+earlierSalt(id)+"/old/port.md6mesh" {
		t.Fatal(add)
	}
	if _, err := discovered.decl(`{ unknown = "`+name+`"; }`, "entitydef"); err == nil {
		t.Fatal("an untyped field cannot establish which mesh to rename")
	}
	f := earlierProvided{Request: strings.ToUpper(req)}
	f.Provides.Type, f.Provides.Name = "baseModel", "md6/different.md6mesh"
	if _, err := p.classifiedNames(id, "", data, []earlierProvided{f}); err == nil {
		t.Fatal("accepted a disagreement between the typed name and request")
	}
	f.Stock = pinJSON(map[string]string{"source": stock})
	f.Provides.Name = name
	borrowed, err := p.classifiedNames(id, "", data, []earlierProvided{f})
	if err != nil || borrowed.kinds[req] != "basemodel" {
		t.Fatalf("relocated stock payload bypassed inspection: %v", err)
	}
	writeF(t, filepath.Join(src, "assets", strings.ToUpper(req)), "\x00"+strings.ToUpper(name)+"\x00")
	if _, err := translateEarlierFiles(src, filepath.Join(data, "refused"), earlierGlobalNames{}, nil, nil, discover, ""); err == nil ||
		!strings.Contains(err.Error(), "inside this file") {
		t.Fatalf("unsafe opaque embedded reference: %v", err)
	}
}

func TestPrunedAuthorStock(t *testing.T) {
	data := t.TempDir()
	source, game, work := filepath.Join(data, "mod"), filepath.Join(data, "game"), filepath.Join(data, "work")
	req := "generated/decls/entitydef/campaign/stock.decl"
	oneStockRequest(t, game, req)
	writeF(t, filepath.Join(work, "placeholder"), "")
	sum := fmt.Sprintf("%x", sha256.Sum256([]byte("{}")))
	hash := strings.Repeat("a", 64)
	m := map[string]any{"id": "earlier.stock", "name": "Earlier stock", "snapmap-plus": map[string]any{
		"format": 1, "bundle": hash, "stock": []any{
			map[string]any{"path": "assets/" + req, "inAssets": false},
			map[string]any{"path": "assets/derived", "inAssets": false, "closure": true},
			map[string]any{"path": "assets/deleted", "inAssets": true},
			map[string]any{"path": "assets/previously-present"},
		},
	}}
	writeF(t, filepath.Join(source, "mod.json"), string(pinJSON(m)))
	missing := prunedAuthorPaths(m)
	if len(missing.paths) != 1 || !missing.paths["assets/"+req] {
		t.Fatalf("unexpected restored paths: %v", missing)
	}
	f := earlierBlob{Path: "assets/" + req, Request: req, Stock: &struct{ SHA256, Source string }{sum, req}}
	b := earlierBundle{assets: true, hash: hash, files: []earlierBlob{f}}
	pins := tinyResolver(t, data, nil)
	pins.doom = game
	recovered, err := pins.prunedAuthorFiles(source, work, &b, missing)
	if err != nil || len(recovered) != 1 {
		t.Fatal(recovered, err)
	}
	// The older release removed the last physical asset from this folder.
	dst := filepath.Join(work, "converted")
	if _, err := translateEarlierAuthor(source, dst, &b, nil, recovered); err != nil {
		t.Fatal(err)
	}
	if got := readF(t, filepath.Join(dst, "native", filepath.FromSlash(req))); got != "{}" {
		t.Fatalf("wrong recovered stock: %q", got)
	}
	if exists(filepath.Join(source, "assets")) {
		t.Fatal("the source was altered during preflight")
	}
	b.hash = strings.Repeat("b", 64)
	if _, err := pins.prunedAuthorFiles(source, work, &b, missing); err == nil {
		t.Fatal("accepted a different recorded build")
	}
	b.hash = hash
	b.files[0].Stock.SHA256 = strings.Repeat("0", 64)
	if _, err := pins.prunedAuthorFiles(source, work, &b, missing); err == nil {
		t.Fatal("accepted stock without its original bytes")
	}
	writeF(t, filepath.Join(source, "assets", filepath.FromSlash(req)), "subsequent edit")
	recovered, err = pins.prunedAuthorFiles(source, work, &b, missing)
	if err != nil || len(recovered) != 0 {
		t.Fatal("replaced a subsequent edit", err)
	}
}
