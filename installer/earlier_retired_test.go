package main

import (
	"bytes"
	"encoding/json"
	"testing"
)

func TestEarlierOpaqueStringSlots(t *testing.T) {
	const binding = `{ "info" : { "~type":"snapVarInfo_t", "name" : "smpbind.playerObjectives.0123456789abcdef" }, /* authored */ "initialValue" : "mods/alex.old/leave" }`
	const reference = `{"info":{"~type":"snapVarInfo_t","name":"smpref.arena.fedcba9876543210"}, "initialValue": "not a package"}`
	const bounds = `{"info":{"~type":"snapVarInfo_t","name":"smp.bounds.v1"},"initialValue":"1;32000"}`
	const badNav = `{"info":{"~type":"snapVarInfo_t","name":"smnav1.cat/room.monster48.0.1.b5d4045c3f466fa9.0123456789abcdef"},"initialValue":"Zm9v"}`
	stock := []byte(`{"entities":[],"variables":{"string":[` + binding + "," + reference + `]}}`)
	out, changed, err := convertEarlierJSON(stock, nil)
	if err != nil || changed || !bytes.Equal(out, stock) {
		t.Fatalf("ordinary map changed: %v", err)
	}
	input := []byte(`{"entities":[{"inherit":"mods/alex.old/npc"}],"variables":{"allocCount":[0,0,0,0,4],"string":[` + bounds + "," + binding + "," + reference + "," + bounds + `]},"snapmap-plus":{"format":1,"mods":[{"id":"alex.old","bundle":"retained"}]}}`)
	resolve := func(row earlierObject) (earlierObject, error) {
		return earlierObject{"mod": json.RawMessage(`"a1234567"`), "whole": json.RawMessage(`"checked"`)}, nil
	}
	mixed, changed, err := convertEarlierJSONWithRows(input, nil, resolve)
	if err != nil || !changed {
		t.Fatalf("mixed map: changed=%t, %v", changed, err)
	}
	var doc struct {
		Variables struct {
			AllocCount []int
			String     []earlierVariable
		}
	}
	if err := json.Unmarshal(earlierJSONText(mixed), &doc); err != nil {
		t.Fatal(err)
	}
	v := doc.Variables
	if len(v.String) != 3 || v.String[0].Info.Name != "String 0" || v.String[1].Info.Name != "smpbind.playerObjectives.0123456789abcdef" ||
		v.String[2].Info.Name != "smpref.arena.fedcba9876543210" || len(v.AllocCount) < 5 || v.AllocCount[4] != 3 {
		t.Fatalf("changed native string indices: %+v", v)
	}
	for _, want := range []string{binding, reference, `"inherit":"mods/a1234567/npc"`, `"bounds":"1;32671"`} {
		if !bytes.Contains(mixed, []byte(want)) {
			t.Fatalf("missing exact %q in %s", want, mixed)
		}
	}
	again, changed, err := convertEarlierJSONWithRows(mixed, nil, resolve)
	if err != nil || changed || !bytes.Equal(again, mixed) {
		t.Fatalf("reconverted a current document: %v", err)
	}
	damaged := bytes.Replace(input, []byte(bounds), []byte(badNav), 1)
	if out, _, err := convertEarlierJSONWithRows(damaged, nil, resolve); err == nil || out != nil {
		t.Fatalf("partly rewrote a damaged map: %s, %v", out, err)
	}
}
