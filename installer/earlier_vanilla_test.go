package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func vanillaTestPins(t *testing.T, providers ...testResource) *earlierPins {
	t.Helper()
	doom := t.TempDir()
	writeResourceIndex(t, doom, providers...)
	if err := os.Rename(filepath.Join(doom, "base", "gameresources.pindex"), filepath.Join(doom, "base", "snap_gameresources.pindex")); err != nil {
		t.Fatal(err)
	}
	return newEarlierPins("", doom, nil)
}
func vanillaTestEntity(id int, parent string, edit any) any {
	return map[string]any{"~type": "idSnapEntity", "uniqueId": id, "entityDef": map[string]any{"~type": "idDeclEntityDef", "inherit": parent, "state": map[string]any{"edit": edit}}}
}
func vanillaTestTable(offsets, values []int) any {
	return map[string]any{"~type": "idIndexMultimap", "keyValues": offsets, "values": values}
}
func vanillaTestRow(id string, names ...string) any {
	return map[string]any{"id": id, "bundle": strings.Repeat("a", 64), "names": names}
}
func vanillaTestVariable(n, v string) any {
	return map[string]any{"info": map[string]any{"~type": "snapVarInfo_t", "name": n}, "initialValue": v}
}
func TestVanillaProjectionUsesTypedSnapMapProvidersAndRebuildsNativeGraph(t *testing.T) {
	const room = "maps/modules/smpgrid/v1/m/10000_20000_10000.decl"
	stock := vanillaTestPins(t, testResource{"entityDef", "stock/override", "x", 1, 1, 1, 0}, testResource{"material", "stock/glow", "glow", 2, 1, 1, 0}, testResource{"md6Def", "stock/door.md6", "x2", 3, 1, 1, 0}, testResource{"material", "fake/entity", "m", 4, 1, 1, 0})
	old := vanillaTestRow("old", "stock/override", "fake/entity", "other/mod", "campaign/old.lwo", "stock/glow", "stock/door.md6", "default", room)
	edit := map[string]any{"renderModelInfo": map[string]any{"model": "campaign/old.lwo", "color": 1}, "customMaterial": "stock/glow", "text": "default", "caption": "fake/entity"}
	rows := []any{
		vanillaTestEntity(0, "stock/override", edit), vanillaTestEntity(1, "fake/entity", nil),
		vanillaTestEntity(2, "snapmaps/unknown", map[string]any{"renderModelInfo": map[string]any{"model": "stock/door.md6"}}),
		vanillaTestEntity(3, "mods/other123/old", nil),
	}
	const st = 4 << 24
	const settings = "smp.bounds.v1"
	root := map[string]any{"~type": "idSnapMap", "instances": []any{map[string]any{"moduleName": room}}, "entities": rows,
		"instanceEntities": vanillaTestTable([]int{0, 3, 4}, []int{0, 1, 2, 3}),
		"references":       map[string]any{"entityEntRefs": vanillaTestTable([]int{0, 2, 3, 4, 4}, []int{1, 2, 2, 3}), "entityVarRefs": vanillaTestTable([]int{0, 3, 4, 5, 5}, []int{st | 2, 1, st | 0, 0, st | 1})},
		"targets":          map[string]any{"connections": []int{-1, 1, 2, -2, 0, -3, 3, 0}, "entitySources": vanillaTestTable([]int{}, []int{}), "entityTargets": vanillaTestTable([]int{}, []int{})},
		"doorsAndCaps":     map[string]any{"portalDoors": []int{0, 1}, "portalFrames": []int{3, -1}},
		"snapmap-plus":     map[string]any{"format": 1, "mods": []any{old, map[string]any{"mod": "other123@" + strings.Repeat("b", 64), "names": []string{"mods/other123/old"}}}},
		"variables":        map[string]any{"allocCount": []int{4, 2, 0, 0, 3}, "string": []any{vanillaTestVariable(settings, "1;32000"), vanillaTestVariable("smp.bounds.v1", "A real authored string"), vanillaTestVariable("my_variable", "mods/other123/old")}}}
	plan := func(root any) earlierObject {
		t.Helper()
		out, e := projectVanillaMap(pinJSON(root), stock)
		if e != nil {
			t.Fatal(e)
		}
		var obj earlierObject
		if e = json.Unmarshal(out, &obj); e != nil {
			t.Fatal(e)
		}
		return obj
	}
	got := plan(root)
	if got["snapmap-plus"] != nil || !bytes.Equal(got["instances"], pinJSON(root["instances"])) {
		t.Fatal("metadata or room layout")
	}
	check := func(field string, expected any) {
		t.Helper()
		var a, b any
		if json.Unmarshal(got[field], &a) != nil || json.Unmarshal(pinJSON(expected), &b) != nil || !reflect.DeepEqual(a, b) {
			t.Fatalf("%s: actual %s; expected %s", field, got[field], pinJSON(expected))
		}
	}
	delete(edit["renderModelInfo"].(map[string]any), "model")
	check("entities", []any{rows[0], rows[2]})
	check("instanceEntities", vanillaTestTable([]int{0, 2, 2}, []int{0, 2}))
	check("references", map[string]any{"entityEntRefs": vanillaTestTable([]int{0, 2, 2, 3, 3}, []int{-1, 2, -1}), "entityVarRefs": vanillaTestTable([]int{0, 3, 3, 4, 4}, []int{st | 1, 1, st | 0xffffff, st | 0})})
	check("targets", map[string]any{"connections": []int{-1, 2, -3, 0}, "entitySources": vanillaTestTable([]int{}, []int{}), "entityTargets": vanillaTestTable([]int{}, []int{})})
	check("doorsAndCaps", map[string]any{"portalDoors": []int{0, -1}, "portalFrames": []int{-1, -1}})
	check("variables", map[string]any{"allocCount": []int{4, 2, 0, 0, 2}, "string": []any{vanillaTestVariable(settings, "A real authored string"), vanillaTestVariable("my_variable", "mods/other123/old")}})
}

func TestVanillaProjectionRefusesMalformedGraphAndUnknownMap(t *testing.T) {
	pins := vanillaTestPins(t)
	entity := vanillaTestEntity(1, "mods/missing123/boss", nil)
	stock := vanillaTestEntity(2, "snapmaps/unknown", nil)
	original := map[string]any{"~type": "idSnapMap", "entities": []any{entity, stock}, "snapmap-plus": map[string]any{"format": 1, "mods": []any{vanillaTestRow("missing123")}}}
	for k, v := range map[string]any{"instanceEntities": vanillaTestTable([]int{0, 2}, []int{}), "targets": map[string]any{"connections": []int{9}}, "references": map[string]any{"entityEntRefs": vanillaTestTable([]int{0, 2, 1}, []int{1})}, "entities": []any{entity, entity}, "~type": "idSnapEntityPrefab", "snapmap-plus": map[string]any{"format": 9, "mods": []any{vanillaTestRow("missing123")}}} {
		t.Run(k, func(t *testing.T) {
			old := original[k]
			original[k] = v
			defer func() {
				if old == nil {
					delete(original, k)
				} else {
					original[k] = old
				}
			}()
			raw := pinJSON(original)
			copy := bytes.Clone(raw)
			out, err := projectVanillaMap(raw, pins)
			if err == nil || out != nil || !bytes.Equal(raw, copy) {
				t.Fatal("projected invalid map", err)
			}
		})
	}
	raw := []byte(`{"~type":"idSnapMap","variables":{"string":[{"info":{"~type":"snapVarInfo_t","name":"smpmod1.0.1"},"initialValue":"user value"}]}}`)
	if out, err := projectVanillaMap(raw, pins); err == nil || out != nil {
		t.Fatal("ordinary authored variable was classified")
	}
}

func TestVanillaProjectionDropsOnlyOwnedRoomAndPortalGroups(t *testing.T) {
	pins := vanillaTestPins(t)
	group := func(n string) any { return map[string]any{"moduleName": n} }
	root := map[string]any{"~type": "idSnapMap", "snapmap-plus": map[string]any{"format": 1, "mods": []any{vanillaTestRow("old")}},
		"instances":        []any{group("maps/modules/stock/room.decl"), group("maps/modules/mods/old/test.decl")},
		"entities":         []any{vanillaTestEntity(3, "stock/door", nil), vanillaTestEntity(7, "stock/door", nil), vanillaTestEntity(8, "stock/global", nil)},
		"instanceEntities": vanillaTestTable([]int{0, 1, 2, 3}, []int{3, 7, 8}), "doorsAndCaps": map[string]any{"portalDoors": []int{3, 7}, "portalFrames": []int{-1, -1}}}
	out, err := projectVanillaMap(pinJSON(root), pins)
	if err != nil {
		t.Fatal(err)
	}
	var doc struct {
		Instances        []any
		Entities         []struct{ UniqueID int }
		InstanceEntities struct{ KeyValues, Values []int }
		DoorsAndCaps     struct{ PortalDoors, PortalFrames []int }
	}
	if e := json.Unmarshal(out, &doc); e != nil {
		t.Fatal(e)
	}
	if len(doc.Instances) != 1 || len(doc.Entities) != 2 || doc.Entities[1].UniqueID != 8 || !reflect.DeepEqual(doc.InstanceEntities.KeyValues, []int{0, 1, 2}) || !reflect.DeepEqual(doc.InstanceEntities.Values, []int{3, 8}) || !reflect.DeepEqual(doc.DoorsAndCaps.PortalDoors, []int{3}) {
		t.Fatalf("ambiguous module/portal splice: %s", out)
	}
	root["instances"] = []any{group("maps/modules/mods/old/test.decl")}
	if out, e := projectVanillaMap(pinJSON(root), pins); e == nil || out != nil || !strings.Contains(e.Error(), "no surviving room") {
		t.Fatal("invented a missing author room", e)
	}
	root["instances"] = []any{group("maps/modules/stock/room.decl"), group("maps/modules/mods/old/test.decl")}
	root["doorsAndCaps"] = map[string]any{"portalDoors": []int{-1, 7}, "portalFrames": []int{-1, -1}}
	if out, e := projectVanillaMap(pinJSON(root), pins); e == nil || out != nil {
		t.Fatal("guessed an unknown portal owner")
	}
}

func TestVanillaProjectionRestoresOnlyBoundStockPlacementAndRealStockAsset(t *testing.T) {
	pins := vanillaTestPins(t, testResource{"entityDef", "shared/barrel", "x", 1, 1, 1, 0}, testResource{"weapon", "player/pistol", "w", 2, 1, 1, 0})
	b := bytes.NewBufferString("SMPBIND\x02")
	number := func(v int) { _ = binary.Write(b, binary.LittleEndian, uint32(v)) }
	word := func(s string) { number(len(s)); b.WriteString(s) }
	number(1)
	number(2)
	number(1)
	b.Write(make([]byte, 32))
	word("")
	barrelAlias := "smp/" + strings.Repeat("a", 64)
	weaponAlias := "smp/" + strings.Repeat("b", 64)
	number(2)
	for _, v := range [][3]string{{"entitydef", "shared/barrel", barrelAlias}, {"weapon", "player/pistol", weaponAlias}} {
		for _, w := range v {
			word(w)
		}
		number(1)
		number(0)
	}
	number(2)
	for i, ref := range []string{"/k:entityDef/k:inherit", "/k:entityDef/k:state/k:edit/k:inventoryItemDecl"} {
		number(1)
		word("entity/0")
		word(ref)
		number(i)
		number(1)
		number(0)
		word(strings.Repeat("1", 64))
	}
	hash := sha256.Sum256(b.Bytes())
	v := vanillaTestVariable("smpbind.0.1."+hex.EncodeToString(hash[:8]), base64.StdEncoding.EncodeToString(b.Bytes()))
	root := map[string]any{"~type": "idSnapMap", "variables": map[string]any{"string": []any{v}}, "entities": []any{vanillaTestEntity(12, barrelAlias, map[string]string{"inventoryItemDecl": weaponAlias})}}
	raw := pinJSON(root)
	out, e := projectVanillaMap(raw, pins)
	if e != nil {
		t.Fatal(e)
	}
	if !bytes.Contains(out, []byte(`"inherit":"shared/barrel"`)) || !bytes.Contains(out, []byte(`"inventoryItemDecl":"player/pistol"`)) || bytes.Contains(out, []byte("smp/")) || bytes.Contains(out, []byte("smpbind.")) {
		t.Fatalf("wrong binding restoration: %s", out)
	}
	root["entities"] = []any{vanillaTestEntity(99, "smp/"+strings.Repeat("c", 64), nil), vanillaTestEntity(12, barrelAlias, map[string]string{"inventoryItemDecl": weaponAlias})}
	if out, e := projectVanillaMap(pinJSON(root), pins); e == nil || out != nil {
		t.Fatal("accepted stale object-site identity")
	}
}

func TestVanillaProjectionPreservesNativeCommentAndAuthoredVariableBytes(t *testing.T) {
	raw := []byte("{\n// map metadata\n" + `"~type":"idSnapMap","version":12,"snapmap-plus":{"format":1,"mods":[{"mod":"abcd1234@` + strings.Repeat("a", 64) + `","names":[]}]},"entities":[],"variables":{"string":[` + "\n" + `{"info":{"~type":"snapVarInfo_t","name":"smpmod1.my_story"},"initialValue":"mods/abcd1234/keep"}` + `]},"keep":` + "/* intact */" + `1.234500e+00}`)
	raw = append([]byte{0xef, 0xbb, 0xbf}, raw...)
	out, e := projectVanillaMap(raw, nil)
	if e != nil {
		t.Fatal(e)
	}
	for _, s := range []string{"// map metadata", "/* intact */", "1.234500e+00", "mods/abcd1234/keep", "smpmod1.my_story"} {
		if !bytes.Contains(out, []byte(s)) {
			t.Fatalf("lost %q in %s", s, out)
		}
	}
	if !bytes.Equal(out[:3], raw[:3]) || bytes.Contains(out, []byte("snapmap-plus")) {
		t.Fatal("incorrect carrier removal")
	}
}

func TestVanillaProjectionChecksStructuredAssetArraysAndNativeIcon(t *testing.T) {
	pins := vanillaTestPins(t)
	e := vanillaTestEntity(8, "stock/speaker", map[string]any{"steps": []any{map[string]any{"soundShader": "sounds/custom/loop", "volume": 72}}}).(map[string]any)
	e["customIcon"] = map[string]any{"~type": "|pointer", "targetType": "idDeclSnapCustomIcon", "value": "icons/old/logo"}
	root := map[string]any{"~type": "idSnapMap", "snapmap-plus": map[string]any{"format": 1, "mods": []any{vanillaTestRow("old", "sounds/custom/loop", "icons/old/logo")}}, "entities": []any{e}}
	out, err := projectVanillaMap(pinJSON(root), pins)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(out, []byte(`"value":null`)) || bytes.Contains(out, []byte("sounds/custom/loop")) || !bytes.Contains(out, []byte(`"volume":72`)) {
		t.Fatalf("left part of an unavailable array or icon: %s", out)
	}
	e["entityDef"] = map[string]any{"~type": "idDeclEntityDef", "inherit": "stock/base", "state": map[string]any{"edit": map[string]any{"skins": []string{"mods/old/unknown"}}}}
	if out, err := projectVanillaMap(pinJSON(root), pins); err == nil || out != nil {
		t.Fatal("guessed the native type of an opaque selected array")
	}
}
