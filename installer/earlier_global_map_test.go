package main

import (
	"bytes"
	"testing"
)

func TestEarlierSavedEntityStateUsesOnlyTypedReferences(t *testing.T) {
	var g earlierGlobalNames
	a := earlierGlobalName{"entitydef", "actor/own", "entities/actor/own.decl", "entities/actor/own", "mods/a1234567/actor/own", "pin one"}
	if err := g.add(a); err != nil {
		t.Fatal(err)
	}
	other := a
	other.build = "another pin"
	if err := g.add(other); err == nil {
		t.Fatal("distinct earlier pins claimed the same typed global")
	}
	src := []byte(`{"entities":[{"displayName":"actor/own","entityDef":{"targetType":"idDeclEntityDef","~type":"idDeclEntityDef","className":"idAI2","inherit":"actor/own","state":{"edit":{"entityDef":"actor/own","label":"actor/own","foreign":{"value":"actor/own","targetType":"idDeclMaterial","~type":"|pointer"},"owned":{"value":"actor/own","targetType":"idDeclEntityDef","~type":"|pointer"}}}}}],"description":"actor/own"}`)
	want := []byte(`{"entities":[{"displayName":"actor/own","entityDef":{"targetType":"idDeclEntityDef","~type":"idDeclEntityDef","className":"idAI2","inherit":"mods/a1234567/actor/own","state":{"edit":{"entityDef":"mods/a1234567/actor/own","label":"actor/own","foreign":{"value":"actor/own","targetType":"idDeclMaterial","~type":"|pointer"},"owned":{"value":"mods/a1234567/actor/own","targetType":"idDeclEntityDef","~type":"|pointer"}}}}}],"description":"actor/own"}`)
	got, err := g.placedReferences(src)
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("%s: %v", got, err)
	}
	ambiguous := bytes.Replace(src, []byte(`"label":`), []byte(`"unknown":`), 1)
	if _, err = g.placedReferences(ambiguous); err == nil {
		t.Fatal("accepted an untyped saved field that names the added resource")
	}
}
