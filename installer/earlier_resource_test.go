package main

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type testResource struct {
	kind, name, alias string
	offset            uint64
	size, stored      uint32
	selector          byte
}

func writeResourceIndex(t *testing.T, root string, rows ...testResource) {
	t.Helper()
	var index bytes.Buffer
	index.Write(make([]byte, 36))
	for _, r := range rows {
		index.Write(make([]byte, 4))
		for _, s := range []string{r.kind, r.name, r.alias} {
			binary.Write(&index, binary.LittleEndian, uint32(len(s)))
			index.WriteString(s)
		}
		var tail [21]byte
		binary.BigEndian.PutUint64(tail[:], r.offset)
		binary.BigEndian.PutUint32(tail[8:], r.size)
		binary.BigEndian.PutUint32(tail[12:], r.stored)
		tail[20] = r.selector
		index.Write(tail[:])
	}
	raw := index.Bytes()
	copy(raw, []byte{5, 'S', 'E', 'R'})
	binary.BigEndian.PutUint32(raw[4:], uint32(len(raw)-32))
	binary.BigEndian.PutUint32(raw[32:], uint32(len(rows)))
	writeF(t, filepath.Join(root, "base/gameresources.pindex"), string(raw))
}

func TestEarlierTypedRetailManifest(t *testing.T) {
	const request = "generated/decls/entitydef/npc/campaign.decl"
	raw := bytes.Repeat([]byte("earlier-game-bytes\n"), 25)
	var packed bytes.Buffer
	z, _ := flate.NewWriter(&packed, flate.DefaultCompression)
	z.Write(raw)
	z.Flush()
	stored := append([]byte{}, packed.Bytes()...)
	z.Close()
	if !bytes.HasSuffix(stored, []byte{0, 0, 255, 255}) {
		t.Fatal("fixture lacks the retail sync-flush boundary")
	}
	for _, tc := range []string{"exact", "wrong-type", "wrong-name", "missing", "same-bytes", "different-bytes", "different-size", "short", "extra", "physical-override", "missing-custom", "case-conflict", "blank-alias"} {
		t.Run(tc, func(t *testing.T) {
			root := t.TempDir()
			src := filepath.Join(root, "author")
			stock := earlierResourceRoot{doom: filepath.Join(root, "doom"), work: root}
			r := testResource{"entityDef", "npc/campaign", request, 0, uint32(len(raw)), uint32(len(stored)), 1}
			line := "entityDef\tnpc/campaign\t" + request
			req := request
			records := []testResource{r}
			payload := append([]byte{}, stored...)
			switch tc {
			case "case-conflict":
				alias := strings.ToUpper(request)
				line += "\nimage\taltered\t" + alias
				records = append(records, testResource{"image", "altered", alias, 0, r.size, r.stored, r.selector})
			case "wrong-type":
				records[0].kind = "md6Def"
			case "wrong-name":
				records[0].name = "npc/other"
			case "missing":
				records = nil
			case "same-bytes", "different-bytes", "different-size":
				second := r
				second.offset = uint64(len(payload))
				other := append([]byte{}, stored...)
				if tc == "different-bytes" {
					other[2] ^= 1
				}
				if tc == "different-size" {
					second.size++
				}
				payload = append(payload, other...)
				records = append(records, second)
			case "short":
				records[0].stored -= 2
				payload = payload[:len(payload)-2]
			case "extra":
				records[0].stored++
				payload = append(payload, 127)
			case "physical-override":
				writeF(t, filepath.Join(src, "decls/entitydef/npc/campaign.decl"), "author takes precedence")
			case "missing-custom":
				line = "request\tcustom/file"
			case "blank-alias":
				r.name = request
				r.alias = ""
				line = "entityDef\t" + request + "\t"
				records = []testResource{r}
			}
			writeResourceIndex(t, stock.doom, records...)
			writeF(t, filepath.Join(stock.doom, "base/gameresources.patch"), string(payload))
			writeF(t, filepath.Join(src, "resources/example.manifest"), line+"\n")
			got, err := earlierPackageFiles(src, map[string]any{}, &stock)
			if tc == "exact" || tc == "same-bytes" || tc == "physical-override" || tc == "blank-alias" {
				if err != nil {
					t.Fatal(err)
				}
				want := raw
				if tc == "physical-override" {
					want = []byte("author takes precedence")
				}
				b, e := os.ReadFile(got[req])
				if e != nil || !bytes.Equal(want, b) {
					t.Fatalf("wanted %d bytes: %v", len(want), e)
				}
			} else if err == nil {
				t.Fatalf("accepted %s", tc)
			}
		})
	}
}

func TestRecoveredModelNameIsACompleteReference(t *testing.T) {
	g := earlierGlobalNames{}
	g.add(earlierGlobalName{kind: "entitydef", name: "example", file: "entities/example.decl"})
	if err := g.retailOpaqueFile([]byte("\x00models/monsters/example/example_shell\x00")); err != nil {
		t.Fatal(err)
	}
	if err := g.retailOpaqueFile([]byte("\x00ExAmPlE\x00")); err == nil {
		t.Fatal("admitted a real ambiguous reference")
	}
}

func TestEarlierPackageKnownSchemaOnly(t *testing.T) {
	d := t.TempDir()
	src := filepath.Join(d, "legacy")
	writeF(t, filepath.Join(src, "package.json"), "{\"schema\":\"snapmap-plus.override-package.v1\",\"version\":\"1.0\",\"contents\":{},\"note\":\"keep this\"}")
	writeF(t, filepath.Join(src, "decls/entitydef/some/old.decl"), "{}")
	dst := filepath.Join(d, "current")
	if _, err := translateEarlierMod(src, dst); err != nil {
		t.Fatal(err)
	}
	text := readF(t, filepath.Join(dst, "mod.json"))
	if strings.Contains(text, "\"schema\"") || strings.Contains(text, "\"contents\"") || !strings.Contains(text, "keep this") {
		t.Fatalf("converted descriptor: %s", text)
	}
}

func TestEarlierDerivedCollisionModel(t *testing.T) {
	const logical = "monsters/model.md6"
	const def = "generated/decls/md6def/monsters/model.md6.decl"
	for _, mode := range []string{"matched", "missing-counterpart", "empty-counterpart", "wrong-decl-type", "explicit-provider", "explicit-index-alias", "wrong-extension", "nonzero-offset", "wrong-kind", "authored"} {
		t.Run(mode, func(t *testing.T) {
			root := t.TempDir()
			src := filepath.Join(root, "author")
			stock := earlierResourceRoot{doom: filepath.Join(root, "game"), work: root}
			cm := testResource{"cm", logical, "", 0, 0, 0, 1}
			md := testResource{"md6Def", logical, def, 0, 2, 2, 1}
			if mode == "wrong-extension" {
				cm.name = "monsters/model.lwo"
				md.name = cm.name
				md.alias = "generated/decls/md6def/" + cm.name + ".decl"
			}
			if mode == "wrong-kind" {
				cm.kind = "model"
			}
			if mode == "nonzero-offset" {
				cm.offset = 172
			}
			if mode == "empty-counterpart" {
				md.size = 0
				md.stored = 0
			}
			if mode == "wrong-decl-type" {
				md.kind = "entityDef"
			}
			if mode == "explicit-index-alias" {
				cm.alias = cm.name
			}
			provider := ""
			if mode == "explicit-provider" {
				provider = cm.name
			}
			line := cm.kind + "\t" + cm.name + "\t" + provider + "\n"
			if mode != "missing-counterpart" {
				line += md.kind + "\t" + md.name + "\t" + md.alias + "\n"
			}
			writeF(t, filepath.Join(src, "resources/model.manifest"), line)
			if mode == "authored" {
				writeF(t, filepath.Join(src, filepath.FromSlash(cm.name)), "author owns this")
			}
			writeResourceIndex(t, stock.doom, cm, md)
			writeF(t, filepath.Join(stock.doom, "base/gameresources.patch"), "{}")
			got, err := earlierPackageFiles(src, map[string]any{}, &stock)
			if mode == "authored" || mode == "matched" {
				if err != nil || readF(t, got[def]) != "{}" {
					t.Fatalf("missing original MD6 def: %v", err)
				}
				if mode == "matched" && got[cm.name] != "" {
					t.Fatal("emitted a nonexistent physical CM")
				}
				if mode == "authored" && readF(t, got[cm.name]) != "author owns this" {
					t.Fatal("lost authored override")
				}
			} else if err == nil {
				t.Fatalf("accepted empty %s", mode)
			}
		})
	}
}

func TestEarlierSettingShape(t *testing.T) {
	for _, tc := range []struct {
		name, value string
		found       bool
	}{
		{"smp.render.v1", "2;check the west door", false}, {"smp.bounds.v1", "2;900", false}, {"smp.render.v1", "3;", false},
		{"smp.render.v1", "2;1;400;", true}, {"smp.render.v1", "3;2e4;70;1500;6500;.35;.4;.45", true},
	} {
		if got := historicalVariable(tc.name, tc.value); got != tc.found {
			t.Errorf("recognition of %q = %v", tc.value, got)
		}
	}
}

func TestEarlyLoosePolicyAndMarkedPackage(t *testing.T) {
	useEarlierUnitTool(t)
	for _, disabled := range []bool{false, true} {
		t.Run(fmt.Sprint(disabled), func(t *testing.T) {
			r := newUpdateRig(t, "dev")
			const request = "generated/decls/entitydef/example/retail.decl"
			oneStockRequest(t, r.doom, request)
			src := filepath.Join(r.data, "overrides", "generated")
			const manifest = "entityDef\tgenerated/decls/entitydef/example/retail.decl\tgenerated/decls/entitydef/example/retail.decl\n"
			const req = "cvar\tg_useResourceBlackList\t0\n"
			writeF(t, filepath.Join(src, "resources/stock.manifest"), manifest)
			writeF(t, filepath.Join(src, "resources/personal.manifest"), "a note\n")
			writeF(t, filepath.Join(src, "requirements/native.requirements"), req)
			writeF(t, filepath.Join(src, "spirv/a.fspv"), "direct shader")
			writeF(t, filepath.Join(src, "shaders/generated/spirv/a.fspv"), "shadowed shader")
			writeF(t, filepath.Join(src, "shaders/generated/spirv/b.fspv"), "packaged shader")
			writeF(t, filepath.Join(r.data, "overrides/assets/generated/decls/material/hybrid.decl"), "{}")
			mark := filepath.Join(r.data, "overrides", "named")
			writeF(t, filepath.Join(mark, "package.json"), "")
			writeF(t, filepath.Join(mark, "decls/entitydef/unique/example.decl"), "{}")
			bad := filepath.Join(r.data, "overrides", "notes", "package.json")
			writeF(t, bad, "")
			if disabled {
				writeF(t, filepath.Join(r.data, "config.json"), "{\"schema_version\":1,\"settings\":{\"overrides.user_enabled\":false}}")
			}
			for i := 0; i < 2; i++ {
				p, done, err := preparePlayerMigration(r.doom, r.data, "")
				if err != nil {
					t.Fatal(err)
				}
				if i == 1 && p.ready() {
					done()
					t.Fatal("second migration scheduled new changes")
				}
				err = p.commit()
				done()
				if err != nil {
					t.Fatal(err)
				}
			}
			target := filepath.Join(r.data, "mods/my-overrides")
			if readF(t, bad) != "" || readF(t, filepath.Join(src, "resources/personal.manifest")) != "a note\n" {
				t.Fatal("changed unrelated files")
			}
			if disabled {
				if earlierPathExists(target) || earlierPathExists(filepath.Join(r.data, "mods/named")) {
					t.Fatal("activated disabled source")
				}
				if readF(t, filepath.Join(src, "resources/stock.manifest")) != manifest || !isRegular(filepath.Join(mark, "package.json")) {
					t.Fatal("removed disabled source")
				}
				return
			}
			if readF(t, filepath.Join(target, "native/generated/spirv/a.fspv")) != "direct shader" ||
				readF(t, filepath.Join(target, "native/generated/spirv/b.fspv")) != "packaged shader" ||
				readF(t, filepath.Join(target, "generated/shaders/generated/spirv/a.fspv")) != "shadowed shader" ||
				readF(t, filepath.Join(target, "materials/hybrid.decl")) != "{}" {
				t.Fatal("lost priority or original shader from the earlier loose layouts")
			}
			if readF(t, filepath.Join(target, "native", request)) != "{}" || readF(t, filepath.Join(target, "generated/resources/stock.manifest")) != manifest ||
				!strings.Contains(readF(t, filepath.Join(target, "mod.json")), "g_useResourceBlackList") || readF(t, filepath.Join(r.data, "mods/named/entities/unique/example.decl")) != "{}" {
				t.Fatal("failed to convert historic sources")
			}
		})
	}
}

func TestPartiallyConvertedPackageKeepsNativeNamespaces(t *testing.T) {
	const marker = "{\"schema\":\"snapmap-plus.override-package.v1\",\"name\":\"my-overrides\",\"version\":\"1.0.0\",\"description\":\"Your own overrides.\"}"
	const decl = "decls/snapeditorentitydef/non_editor/volume/hazard/snap_dot_fire.decl"
	const backup = "decls/snapeditorentitydef/volume/blocking.decl.backup"
	const include = "decls/renderprogs/includes/custom.inc"
	for _, tc := range []string{"original-marker", "literal-package", "absent-index"} {
		t.Run(tc, func(t *testing.T) {
			root := t.TempDir()
			src := filepath.Join(root, "my-overrides")
			doom := filepath.Join(root, "doom")
			row := testResource{kind: "renderProg", name: "prog", alias: "decls/renderprogs/includes/vertex.inc", size: 1, stored: 1}
			if tc != "absent-index" {
				writeLooseIndexes(t, doom, []testResource{row}, nil)
			}
			desc := marker
			if tc == "literal-package" {
				desc = "{\"id\":\"current\"}"
			}
			writeF(t, filepath.Join(src, "package.json"), desc)
			for _, file := range []string{decl, backup, include, "images/icon.bimage", "generated/decls/snapeditorentitydef/already.decl"} {
				writeF(t, filepath.Join(src, "assets", filepath.FromSlash(file)), file)
			}
			files, err := earlierPackageFiles(src, map[string]any{}, &earlierResourceRoot{doom: doom, work: root})
			if tc == "absent-index" {
				if err == nil {
					t.Fatal("classified uncertain paths without their catalog")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if tc == "literal-package" {
				if files[decl] == "" || files["generated/"+decl] != "" {
					t.Fatalf("changed an explicit modern request: %v", files)
				}
			} else if files[decl] != "" || files[backup] != "" || files["generated/"+decl] == "" || files["generated/image/icon.bimage"] == "" {
				t.Fatalf("lost earlier wrapper or served the disabled copy: %v", files)
			}
			if files[include] == "" || files["generated/decls/snapeditorentitydef/already.decl"] == "" {
				t.Fatalf("lost native engine paths: %v", files)
			}
			if readF(t, filepath.Join(src, "assets", filepath.FromSlash(backup))) != backup {
				t.Fatal("changed the author's disabled file")
			}
		})
	}
}
