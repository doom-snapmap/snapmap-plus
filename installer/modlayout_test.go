package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const testModBlock = `{ "format":2, "salt":"a1234567", "rules":7, "build":"oldhash", "units_of":["unit"] }`
const testModIdentity = `{"format":2,"salt":"a1234567"}`

func TestInstallUpgradesModLayout(t *testing.T) {
	r := newUpdateRig(t, "dev")
	src := filepath.Join(r.data, "mods", "demons", "pack")
	cache := filepath.Join(r.data, "cache", "content")
	old := filepath.Join(r.data, "mods", ".content")
	history := filepath.Join(r.data, "mods", ".builds", "pack@146a9dbac3758deb", "mod.json")
	pack := filepath.Join(src, "mod.json")
	author := "\ufeff{\r\n  \"name\" : \"My pack\", \"snapmap-plus\" : %s,\r\n \"unknown\" : [ 3, 2 ]\r\n}\r\n"
	writeF(t, pack, fmt.Sprintf(author, testModBlock))
	writeF(t, history, fmt.Sprintf(author, testModBlock))
	writeF(t, filepath.Join(old, "served", "pin", "mod.json"), fmt.Sprintf(author, testModBlock))
	blob := filepath.Join(old, "files", "00", "texture")
	writeF(t, blob, "texture bytes")
	oldLink := filepath.Join(old, "served", "pin", "image")
	if err := os.Link(blob, oldLink); err != nil {
		t.Fatal(err)
	}
	for _, rel := range []string{"units/u.json", "partial/u.part"} {
		writeF(t, filepath.Join(old, rel), rel)
	}
	writeF(t, filepath.Join(r.data, "mods", ".served", "banks", "bank.bnk"), "sound")
	// A backend installed for an older release can still use its inline descriptor.
	dist := synthDist(t, r.tmp, "before")
	f := flags{local: dist, doom: r.doom, yes: true, noSelf: true}
	if err := cmdInstall(f); err != nil {
		t.Fatal(err)
	}
	if got := readF(t, pack); got != fmt.Sprintf(author, testModBlock) || exists(cache) || readF(t, history) != got {
		t.Fatal("a backend without layout support upgraded the mod data")
	}

	dist = synthDist(t, r.tmp, "after")
	bytes := "backend " + modBuildManifest
	writeF(t, filepath.Join(dist, currentProxy), bytes)
	manifest := filepath.Join(dist, "MANIFEST.sha256")
	writeF(t, manifest, strings.Replace(readF(t, manifest), sha256Hex([]byte("backend-after")), sha256Hex([]byte(bytes)), 1))
	f.local = dist
	for i := 0; i < 2; i++ {
		if err := cmdUpdate(f); err != nil {
			t.Fatal(err)
		}
		for _, descriptor := range []string{pack, filepath.Join(cache, "served", "pin", "mod.json")} {
			if got := readF(t, descriptor); got != fmt.Sprintf(author, testModIdentity) {
				t.Fatalf("author bytes or identity changed: %q", got)
			}
			if got := readF(t, filepath.Join(filepath.Dir(descriptor), ".build", "manifest.json")); got != testModBlock {
				t.Fatalf("the generated manifest changed: %q", got)
			}
		}
		if exists(old) || exists(filepath.Join(r.data, "mods", ".served")) {
			t.Fatal("a shared directory remains in the mods folder")
		}
		if exists(filepath.Join(r.data, "cache", "legacy-builds")) || readF(t, history) != fmt.Sprintf(author, testModBlock) {
			t.Fatal("an unverified historical folder was rewritten")
		}
		if got := readF(t, filepath.Join(r.data, "cache", "served", "banks", "bank.bnk")); got != "sound" {
			t.Fatal("sound bank did not survive the upgrade")
		}
		for _, rel := range []string{"units/u.json", "partial/u.part"} {
			if got := readF(t, filepath.Join(cache, rel)); got != rel {
				t.Fatalf("lost %s", rel)
			}
		}
		a, err := os.Stat(filepath.Join(cache, "served", "pin", "image"))
		if err != nil {
			t.Fatal(err)
		}
		b, err := os.Stat(filepath.Join(cache, "files", "00", "texture"))
		if err != nil || !os.SameFile(a, b) {
			t.Fatal("a composition lost its hard link")
		}
	}
}

func TestUpgradeModLayoutRecoversOverlap(t *testing.T) {
	data := t.TempDir()
	mods, cache := filepath.Join(data, "mods"), filepath.Join(data, "cache")
	old, dst := filepath.Join(mods, ".content"), filepath.Join(cache, "content")
	blobKey := sha256Hex([]byte("verified"))
	blob := filepath.Join("files", blobKey[:2], blobKey)
	for _, e := range []struct{ rel, src, dest string }{
		{blob, "verified", "incomplete"},
		{"partial/longer.part", "abcdef", "abc"},
		{"partial/current.part", "abc", "abcdef"},
		{"other", "old content", "current content"},
		{"duplicate", "same", "same"},
	} {
		writeF(t, filepath.Join(old, e.rel), e.src)
		writeF(t, filepath.Join(dst, e.rel), e.dest)
	}
	author := fmt.Sprintf(`{"name":"editable","snapmap-plus":%s}`, testModBlock)
	for _, rel := range []string{"maps/old", "mods/pack.old-34", "earlier/inner"} {
		writeF(t, filepath.Join(mods, rel, "mod.json"), author)
	}
	writeF(t, filepath.Join(mods, "earlier", "package.json"), "{}")
	// The first write may have finished before the previous updater stopped.
	pack := filepath.Join(mods, "group", "pack")
	writeF(t, filepath.Join(pack, "mod.json"), author)
	writeF(t, filepath.Join(pack, ".build", "manifest.json"), testModBlock)
	if err := upgradeModLayout(data); err == nil || readF(t, filepath.Join(old, "other")) != "old content" || readF(t, filepath.Join(dst, "other")) != "current content" || readF(t, filepath.Join(pack, "mod.json")) != author {
		t.Fatal("conflict did not preserve both sources before migration")
	}
	if err := os.Rename(filepath.Join(dst, "other"), filepath.Join(data, "user-kept-other")); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(filepath.Join(dst, blob), filepath.Join(data, "user-kept-partial-blob")); err != nil {
		t.Fatal(err)
	}
	if err := upgradeModLayout(data); err != nil {
		t.Fatal(err)
	}
	if readF(t, filepath.Join(dst, blob)) != "verified" || readF(t, filepath.Join(dst, "partial/longer.part")) != "abcdef" || readF(t, filepath.Join(dst, "partial/current.part")) != "abcdef" {
		t.Fatal("lost verified cache data")
	}
	for _, rel := range []string{"maps/old", "mods/pack.old-34", "earlier/inner"} {
		if got := readF(t, filepath.Join(mods, rel, "mod.json")); got != author {
			t.Errorf("unexpected write in %s", rel)
		}
	}
	if readF(t, filepath.Join(pack, "mod.json")) != strings.Replace(author, testModBlock, testModIdentity, 1) {
		t.Fatal("an interrupted descriptor split was not resumed")
	}
}

func TestUpgradeModLayoutLeavesUnbuiltAndUnreadableDescriptors(t *testing.T) {
	data := t.TempDir()
	for i, contents := range []string{
		`{"name":"new"}`,
		`{"snapmap-plus":{"format":2,"salt":"abcdefgh"}}`,
		`{"snapmap-plus":{"format":2,"salt":"abcdefgh","campaign_cvars":{"g_useResourceBlackList":null,"g_useImageBlackList":false}}}`,
		`{"snapmap-plus":{"format":1,"salt":"abcdefgh","rules":3}}`,
		`{"snapmap-plus":{"format":2,"salt":"bad","rules":3}}`,
		`{"snapmap-plus":{"format":2,"salt":"abcdefgh","rules":3}`,
	} {
		folder := filepath.Join(data, "mods", fmt.Sprint(i))
		path := filepath.Join(folder, "mod.json")
		writeF(t, path, contents)
		if err := upgradeModLayout(data); err != nil {
			t.Fatal(err)
		}
		if readF(t, path) != contents || exists(filepath.Join(folder, ".build")) {
			t.Fatalf("unrecognized descriptor %d was changed", i)
		}
	}
}

func TestUpgradeModLayoutPreservesCampaignCVars(t *testing.T) {
	const author = "{\r\n  \"requirements\" : {\"cvars\":{\"g_useResourceBlackList\":0,\"g_useImageBlackList\":\"0\"}},\r\n  \"snapmap-plus\" : %s\r\n}"
	for _, tc := range []struct {
		name      string
		rules     int
		ownership string
	}{
		{"rule 17 with no ownership", 17, ""},
		{"inserted and overridden", 18, `,"campaign_cvars":{"g_useResourceBlackList":null,"g_useImageBlackList":"1"}`},
		{"integer and boolean overrides", 18, `,"campaign_cvars":{"g_useResourceBlackList":1,"g_useImageBlackList":true}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			data := t.TempDir()
			folder := filepath.Join(data, "mods", "pack")
			path := filepath.Join(folder, "mod.json")
			inline := fmt.Sprintf(`{ "rules":%d, "format":2, "salt":"a1234567", "build":"oldhash"%s }`, tc.rules, tc.ownership)
			writeF(t, path, fmt.Sprintf(author, inline))
			identity := `{"format":2,"salt":"a1234567"` + tc.ownership + "}"
			for i := 0; i < 2; i++ {
				if err := upgradeModLayout(data); err != nil {
					t.Fatal(err)
				}
				if got := readF(t, path); got != fmt.Sprintf(author, identity) {
					t.Fatalf("unexpected source after split: %s", got)
				}
				if got := readF(t, filepath.Join(folder, ".build", "manifest.json")); got != inline {
					t.Fatalf("unexpected manifest after split: %s", got)
				}
			}
		})
	}
}

func TestUpgradeModLayoutRetriesWithoutLosingAuthorData(t *testing.T) {
	data := t.TempDir()
	folder := filepath.Join(data, "mods", "props")
	path := filepath.Join(folder, "mod.json")
	source := fmt.Sprintf(`{"author":"me","snapmap-plus":%s}`, testModBlock)
	writeF(t, path, source)
	linked := filepath.Join(data, "author-copy")
	if err := os.Link(path, linked); err != nil {
		t.Fatal(err)
	}
	sidecar := filepath.Join(folder, ".build", "manifest.json")
	mkdirAll(t, sidecar)
	if err := upgradeModLayout(data); err == nil || readF(t, path) != source {
		t.Fatal("a failed sidecar write lost the original descriptor")
	}
	if err := os.Remove(sidecar); err != nil {
		t.Fatal(err)
	}
	writeF(t, sidecar, "pre-existing user file")
	if err := upgradeModLayout(data); err == nil || readF(t, sidecar) != "pre-existing user file" || readF(t, path) != source {
		t.Fatal("pre-existing author sidecar was displaced")
	}
	if err := os.Rename(sidecar, sidecar+".mine"); err != nil {
		t.Fatal(err)
	}
	if err := upgradeModLayout(data); err != nil {
		t.Fatal(err)
	}
	if readF(t, linked) != source || readF(t, sidecar) != testModBlock || readF(t, path) != strings.Replace(source, testModBlock, testModIdentity, 1) {
		t.Fatal("retry did not preserve the linked descriptor and split the mod")
	}
}

func TestSameReleaseUpdateRetriesLayoutAndUninstallKeepsContent(t *testing.T) {
	const tag = "v1.0.0"
	rig := newUpdateRig(t, tag)
	release := goodRelease(t, tag, false)
	z := zipFiles(t, map[string]string{
		currentProxy:                   "backend " + modBuildManifest,
		filepath.ToSlash(currentUIDll): "ui",
	})
	release.assets[releaseAsset] = &mockAsset{body: z, digest: "sha256:" + sha256Hex(z)}
	newMockGitHub(t).publish(release)
	f := flags{yes: true, noSelf: true, doom: rig.doom}
	source := fmt.Sprintf(`{"snapmap-plus":%s}`, testModBlock)
	folder := filepath.Join(rig.data, "mods", "props")
	descriptor := filepath.Join(folder, "mod.json")
	sidecar := filepath.Join(folder, ".build", "manifest.json")
	writeF(t, descriptor, source)
	mkdirAll(t, sidecar)
	writeF(t, filepath.Join(rig.data, "mods", ".content", "units", "kept.json"), "offline map")
	writeF(t, filepath.Join(rig.doom, currentProxy), "another tool")
	if err := cmdInstall(f); err == nil || !strings.Contains(err.Error(), "before installing") {
		t.Fatalf("got %v for a busy manifest", err)
	}
	if rec, _ := loadRecord(); (rec != nil && rec.Version == tag) || readF(t, filepath.Join(rig.doom, currentProxy)) != "another tool" {
		t.Fatal("preflight failure installed the new release")
	}
	if err := os.Remove(sidecar); err != nil {
		t.Fatal(err)
	}
	if err := cmdUpdate(f); err != nil {
		t.Fatal(err)
	}
	if err := cmdUninstall(f); err != nil {
		t.Fatal(err)
	}
	if readF(t, filepath.Join(rig.doom, currentProxy)) != "another tool" ||
		readF(t, filepath.Join(rig.data, "cache", "content", "units", "kept.json")) != "offline map" ||
		readF(t, sidecar) != testModBlock || readF(t, descriptor) != strings.Replace(source, testModBlock, testModIdentity, 1) {
		t.Fatal("same-release update or uninstall lost app data or the original DLL")
	}
}
