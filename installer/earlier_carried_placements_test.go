package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func placedCarriers(t *testing.T, actor string, shipments ...map[string]string) string {
	t.Helper()
	var variables []any
	for i, files := range shipments {
		raw := zipFiles(t, files)
		sum := sha256.Sum256(raw)
		variables = append(variables, map[string]any{"info": map[string]any{"~type": "snapVarInfo_t", "name": fmt.Sprintf("smpkg.delivery.%d.0.1.%x", i, sum[:8])}, "initialValue": base64.StdEncoding.EncodeToString(raw)})
	}
	raw, e := earlierSetMember([]byte(placedBetaDoc("idSnapMap", actor)), "variables", pinJSON(map[string]any{"string": variables}))
	if e != nil {
		t.Fatal(e)
	}
	return string(raw)
}

func TestCarriedGlobalPlacements(t *testing.T) {
	useEarlierUnitTool(t)
	r := newUpdateRig(t, "dev")
	stock := "ai/demon/baron_coop"
	oneStockRequest(t, r.doom, "generated/decls/entitydef/"+stock+".decl")
	id := "old-boss"
	descriptor := string(pinJSON(map[string]string{"id": id, "name": "Old Boss"}))
	// beta.19's old-boss package uses the original decls/entitydef/boss.decl spelling.
	main := map[string]string{"package.json": descriptor, "decls/entitydef/boss.decl": "{ inherit = \"boss-parent\"; edit = {} }"}
	support := map[string]string{"package.json": descriptor, "decls/entitydef/boss-parent.decl": "{ class = \"idAI2\"; edit = {} }", "decls/entitydef/other.decl": "{ inherit = \"\"; }", "decls/entitydef/" + stock + ".decl": "{ inherit = \"\"; }"}
	p := newEarlierPins(r.data, r.doom, nil)
	defer p.close()
	out, changed, err := convertEarlierPinnedJSON([]byte(placedCarriers(t, "boss", main, support)), nil, p)
	if err != nil || !changed {
		t.Fatalf("carried actor: %v", err)
	}
	var doc struct {
		Entities []struct {
			DisplayName string
			EntityDef   struct{ Inherit string }
		}
		Block struct{ Mods []earlierObject } `json:"snapmap-plus"`
	}
	if err = json.Unmarshal(out, &doc); err != nil || len(doc.Block.Mods) != 1 || len(doc.Entities) != 1 || doc.Entities[0].EntityDef.Inherit != "mods/"+earlierSalt(id)+"/boss" || doc.Entities[0].DisplayName != "boss" {
		t.Fatalf("carried actor and display: %s %v", out, err)
	}
	row := doc.Block.Mods[0]
	var objects map[string]string
	if err = json.Unmarshal(row["objects"], &objects); err != nil || len(objects) != 1 || !pinHash(objects["entities/boss"]) || row["whole"] != nil {
		t.Fatalf("unexpected object pin %s", pinJSON(row))
	}
	unit := readF(t, filepath.Join(r.data, "cache/content/units", objects["entities/boss"]+".json"))
	if !strings.Contains(unit, "entities/boss-parent.decl") || strings.Contains(unit, "entities/other.decl") {
		t.Fatalf("cross-delivery dependencies: %s", unit)
	}
	// A broken installed package with the same global spelling cannot supersede verified map contents.
	broken := filepath.Join(r.data, "overrides", "broken")
	writeF(t, filepath.Join(broken, "package.json"), "{\"id\":\"broken\"}")
	writeF(t, filepath.Join(broken, "decls/entitydef/boss.decl"), "{}")
	writeF(t, filepath.Join(broken, "resources/missing.manifest"), "some/request\tmissing/file\n")
	dropped := filepath.Join(t.TempDir(), "drop.zip")
	writeF(t, dropped, string(zipFiles(t, map[string]string{"map.json": placedCarriers(t, "boss", main, support)})))
	plan, done, e := preparePlayerMigration(r.doom, r.data, dropped)
	if e != nil {
		t.Fatal(e)
	}
	if e = plan.commit(); e != nil {
		t.Fatal(e)
	}
	done()
	again, done, e := preparePlayerMigration(r.doom, r.data, dropped)
	if e != nil || again.ready() {
		t.Fatalf("repeated portable delivery: %v", e)
	}
	done()
	encoded, changed, err := convertEarlierPinnedJSON([]byte(placedCarriers(t, stock, support)), nil, p)
	if err != nil || !changed || !bytes.Contains(encoded, []byte("\"inherit\":\""+stock+"\"")) {
		t.Fatalf("retail actor %s: %v", encoded, err)
	}
	ambiguous := map[string]string{"package.json": "{\"id\":\"second-owner\",\"name\":\"Second\"}", "decls/entitydef/boss.decl": "{}"}
	_, _, err = convertEarlierPinnedJSON([]byte(placedCarriers(t, "boss", main, ambiguous)), nil, p)
	if err == nil || !strings.Contains(err.Error(), "two earlier mods claim") {
		t.Fatalf("different global owners: %v", err)
	}
}

func TestDamagedPackageDoesNotClaimRetailPlacement(t *testing.T) {
	useEarlierUnitTool(t)
	r := newUpdateRig(t, "dev")
	const stock = "ai/demon/baron_coop"
	oneStockRequest(t, r.doom, "generated/decls/entitydef/"+stock+".decl")
	src := filepath.Join(r.data, "overrides", "broken")
	writeF(t, filepath.Join(src, "package.json"), "{\"id\":\"broken\"}")
	writeF(t, filepath.Join(src, "decls/entitydef/"+stock+".decl"), "{}")
	writeF(t, filepath.Join(src, "decls/entitydef/actual-custom.decl"), "{}")
	writeF(t, filepath.Join(src, "resources/assets.manifest"), "some/request\tmissing/actual.bin\n")
	withoutGame := filepath.Join(t.TempDir(), "unrelated.json")
	writeF(t, withoutGame, placedBetaDoc("idSnapMap", "unrelated/actor"))
	plan, done, err := preparePlayerMigration(t.TempDir(), r.data, withoutGame)
	if err != nil || plan.ready() {
		t.Fatalf("vanilla drop required game archives: %v", err)
	}
	done()
	for _, actor := range []string{stock, "unrelated/stock", "actual-custom"} {
		in := filepath.Join(t.TempDir(), "placed.json")
		original := placedBetaDoc("idSnapMap", actor)
		writeF(t, in, original)
		stamp := time.Unix(1603000000, 0)
		if err := os.Chtimes(in, stamp, stamp); err != nil {
			t.Fatal(err)
		}
		plan, done, err := preparePlayerMigration(r.doom, r.data, in)
		if actor == "actual-custom" {
			if err == nil || !strings.Contains(err.Error(), "missing file") {
				t.Fatalf("damaged owner of actual addition: %v", err)
			}
		} else if err != nil || plan.ready() {
			t.Fatalf("retail/unrelated placement %s: %v", actor, err)
		}
		done()
		st, err := os.Stat(in)
		if err != nil || !st.ModTime().Equal(stamp) || readF(t, in) != original {
			t.Fatal("changed source", err)
		}
	}
}
