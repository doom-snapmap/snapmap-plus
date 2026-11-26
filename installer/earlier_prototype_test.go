package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"path/filepath"
	"strings"
	"testing"
)

func TestArchivedPrivatePlacementAndPackageReference(t *testing.T) {
	alias := "smp/" + strings.Repeat("a", 64)
	text := func(s string) []byte {
		return append(binary.LittleEndian.AppendUint32(nil, uint32(len(s))), s...)
	}
	words := func(v ...uint32) []byte {
		var out []byte
		for _, n := range v {
			out = binary.LittleEndian.AppendUint32(out, n)
		}
		return out
	}
	var b bytes.Buffer
	b.WriteString("SMPBIND\x02")
	b.Write(words(1, 2, 1))
	b.Write(bytes.Repeat([]byte{1}, 32))
	b.Write(words(0, 1))
	for _, s := range []string{"entitydef", "test/speaker", alias} {
		b.Write(text(s))
	}
	b.Write(words(1, 0, 1, 1))
	b.Write(text("entity/0"))
	b.Write(text("/k:entityDef/k:inherit"))
	b.Write(words(0, 1, 0))
	b.Write(text(strings.Repeat("2", 64)))
	digest := func(data []byte) string { v := sha256.Sum256(data); return hex.EncodeToString(v[:8]) }
	row := func(name, value string) string {
		return string(pinJSON(map[string]any{"info": map[string]string{"~type": "snapVarInfo_t", "name": name}, "initialValue": value}))
	}
	b64 := base64.StdEncoding.EncodeToString(b.Bytes())
	chunks := row("smpbind.1.2."+digest(b.Bytes()), b64[29:]) + "," + row("smpbind.0.2."+digest(b.Bytes()), b64[:29])
	file := func(parent, rows string) []byte {
		return []byte(`{"~type":"idSnapMap","entities":[{"~type":"idSnapEntity","uniqueId":1,"displayName":` + string(pinJSON(alias)) + `,"entityDef":{"~type":"idDeclEntityDef","inherit":` + string(pinJSON(parent)) + `}}],"variables":{"string":[` + rows + `]}}`)
	}
	external := []byte(`{"id":"local.boss-demons","name":"Boss demons","packed":1234,"sha256":"` + strings.Repeat("e", 64) + `"}`)
	ref := row("smpref.delivery."+strings.Repeat("1", 64)+"."+digest(external), base64.StdEncoding.EncodeToString(external))
	bounds := row("smp.bounds.v1", "1;32000")
	cases := []struct{ name, parent, rows, reason string }{
		{"matched bound actor", alias, chunks, alias},
		{"references alone", "stock/actor", ref, "local.boss-demons"},
		{"only a display label matches", "stock/actor", chunks, ""},
		{"another private name", "smp/" + strings.Repeat("c", 64), chunks, "cannot establish"},
		{"wrong checksum", alias, strings.ReplaceAll(chunks, digest(b.Bytes()), strings.Repeat("0", 16)), "cannot establish"},
		{"damaged binder unused", "stock/actor", strings.ReplaceAll(chunks, digest(b.Bytes()), strings.Repeat("0", 16)), ""},
		{"wrong reference checksum", alias, strings.ReplaceAll(ref, digest(external), strings.Repeat("0", 16)), ""},
		{"partial set", alias, row("smpbind.0.2."+digest(b.Bytes()), b64[:29]), "cannot establish"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			raw := file(tc.parent, tc.rows)
			if got := documentEvidence(raw, nil); got != (tc.reason != "") {
				t.Fatalf("evidence=%t", got)
			}
			mixed := file(tc.parent, bounds+","+tc.rows)
			out, changed, err := convertEarlierJSON(mixed, nil)
			if tc.reason != "" {
				var why unavailablePrototype
				if !errors.As(err, &why) || !strings.Contains(err.Error(), tc.reason) || changed || out != nil {
					t.Fatalf("expected refusal for %s, got %v", tc.reason, err)
				}
			} else if err != nil || !changed || !bytes.Contains(out, []byte(tc.rows)) {
				t.Fatalf("changed an opaque variable or refused a healthy map: %v", err)
			}
		})
	}
	r := newUpdateRig(t, "dev")
	oneStockRequest(t, r.doom, "generated/decls/entitydef/stock/actor.decl")
	writeF(t, filepath.Join(r.doom, "base/snap_gameresources.pindex"), readF(t, filepath.Join(r.doom, "base/gameresources.pindex")))
	path := filepath.Join(r.data, "rawmaps", "Bound.json")
	good := filepath.Join(r.data, "rawmaps", "Neighbor.json")
	original := file(alias, chunks+","+bounds)
	writeF(t, path, string(original))
	writeF(t, good, string(file("stock/actor", bounds)))
	for i := 0; i < 2; i++ {
		plan, clean, err := preparePlayerMigration(r.doom, r.data, "")
		if err != nil {
			clean()
			t.Fatal(err)
		}
		want := 1 - i
		if len(plan.Deferred) != 0 || len(plan.VanillaMaps) != want {
			clean()
			t.Fatalf("wrong map disposition: %v, %v", plan.VanillaMaps, plan.Deferred)
		}
		err = plan.commit()
		clean()
		if err != nil {
			t.Fatal(err)
		}
	}
	if strings.Contains(readF(t, path), "smpbind.") || strings.Contains(readF(t, path), alias) || !strings.Contains(readF(t, good), `"format":2`) {
		t.Fatalf("wrong projected map or neighboring conversion in %s", r.data)
	}
}
