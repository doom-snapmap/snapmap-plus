package main

import (
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func mustWrite(t *testing.T, root, rel, content string) {
	t.Helper()
	if err := write(root, rel, []byte(content)); err != nil {
		t.Fatal(err)
	}
}

// installedLayout writes a DOOM and data folder that meet the contract.
func installedLayout(t *testing.T) (doom, data string) {
	t.Helper()
	tmp := t.TempDir()
	doom, data = filepath.Join(tmp, "DOOM"), filepath.Join(tmp, "data")
	for _, rel := range append(vanilla, "XINPUT1_4.dll", pair2024UI) {
		mustWrite(t, doom, rel, rel)
	}
	mustWrite(t, data, "snapmap-plus.exe", "exe")
	mustWrite(t, data, "snapmap-plus-store.exe", "tool")
	mustWrite(t, data, `prefabs\a.snapmap.json`, "{}")
	exe, tool := hexSHA([]byte("exe")), hexSHA([]byte("tool"))
	mustWrite(t, data, "install.json", `{
  "version": "v0.3.0-sim.2",
  "doom_path": "`+strings.ReplaceAll(doom, `\`, `\\`)+`",
  "files": ["XINPUT1_4.dll", "snapmap-plus\\snapmap-plus-ui-13954591.dll"],
  "backups": [],
  "executables": {"snapmap-plus.exe": {"sha256": "`+exe+`"}, "snapmap-plus-store.exe": {"sha256": "`+tool+`"}}
}`)
	return doom, data
}

func check(t *testing.T, args ...string) (bool, string) {
	t.Helper()
	var out strings.Builder
	ok, err := cmdCheck(args, &out)
	if err != nil {
		t.Fatal(err)
	}
	return ok, out.String()
}

func TestCheckAcceptsTheContract(t *testing.T) {
	doom, data := installedLayout(t)
	ok, out := check(t, "--stage", "s", "--doom", doom, "--data", data, "--phase", "installed",
		"--expect", "XINPUT1_4.dll", "--expect", pair2024UI, "--keep", `prefabs\a.snapmap.json`)
	if !ok {
		t.Fatalf("a layout that meets the contract failed:\n%s", out)
	}
	for _, want := range []string{"the pair", "vanilla", "installer", "player data", "ok  contract met"} {
		if !strings.Contains(out, want) {
			t.Errorf("listing lacks %q:\n%s", want, out)
		}
	}
}

// Every kind of leftover the owner reported fails loudly.
func TestCheckFailsOnEveryLeftover(t *testing.T) {
	for _, c := range []struct {
		name, root, rel, want string
	}{
		{"other pair", "doom", "XINPUT1_3.dll", "the DOOM folder holds XINPUT1_3.dll"},
		{"backup in DOOM", "doom", "XINPUT1_4.dll.snapmap-plus-bak", "the DOOM folder holds XINPUT1_4.dll.snapmap-plus-bak"},
		{"leftover frontend", "doom", pair2018UI, "the DOOM folder holds " + pair2018UI},
		{"installer-files.json", "data", "installer-files.json", "the data folder holds installer-files.json"},
		{"updates.json", "data", "snapmap-plus.exe.updates.json", "the data folder holds snapmap-plus.exe.updates.json"},
		{".old", "data", "snapmap-plus.exe.old", "the data folder holds snapmap-plus.exe.old"},
		{"override-backups", "data", `override-backups\x`, `the data folder holds override-backups\x`},
		{"unrecorded cache", "data", `cache\installer\snapmap-plus.exe.old`, "which install.json does not record"},
	} {
		t.Run(c.name, func(t *testing.T) {
			doom, data := installedLayout(t)
			root := map[string]string{"doom": doom, "data": data}[c.root]
			mustWrite(t, root, c.rel, "leftover")
			ok, out := check(t, "--stage", "s", "--doom", doom, "--data", data, "--phase", "installed",
				"--expect", "XINPUT1_4.dll", "--expect", pair2024UI)
			if ok || !strings.Contains(out, c.want) {
				t.Errorf("ok=%v, want a failure naming %q:\n%s", ok, c.want, out)
			}
		})
	}
}

func TestCheckUninstalled(t *testing.T) {
	doom, data := installedLayout(t)
	ok, out := check(t, "--stage", "s", "--doom", doom, "--data", data, "--phase", "uninstalled")
	if ok || !strings.Contains(out, "uninstall left install.json") || !strings.Contains(out, "the DOOM folder holds XINPUT1_4.dll") {
		t.Fatalf("an installed layout passed as uninstalled:\n%s", out)
	}
	for _, rel := range []string{"XINPUT1_4.dll", pair2024UI} {
		os.Remove(filepath.Join(doom, rel))
	}
	os.Remove(filepath.Join(doom, "snapmap-plus"))
	for _, rel := range []string{"snapmap-plus.exe", "snapmap-plus-store.exe", "install.json"} {
		os.Remove(filepath.Join(data, rel))
	}
	if ok, out := check(t, "--stage", "s", "--doom", doom, "--data", data, "--phase", "uninstalled", "--keep", `prefabs\a.snapmap.json`); !ok {
		t.Fatalf("a vanilla layout failed:\n%s", out)
	}
}

// The fake API lists releases newest first with the size and digest the installer verifies, and a
// release added later appears on the next request.
func TestServerListsReleasesWithDigests(t *testing.T) {
	root := t.TempDir()
	mustWrite(t, root, `v0.3.0-sim.1\snapmap-plus.exe`, "older")
	s := &releaseServer{root: root}
	srv := httptest.NewServer(s)
	defer srv.Close()
	s.base = srv.URL
	get := func(path string, v any) {
		t.Helper()
		resp, err := srv.Client().Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		if err := json.NewDecoder(resp.Body).Decode(v); err != nil {
			t.Fatal(err)
		}
	}
	mustWrite(t, root, `v0.3.0-sim.2\snapmap-plus.exe`, "newer")
	var list []ghRelease
	get("/repos/"+repoSlug+"/releases", &list)
	if len(list) != 2 || list[0].TagName != "v0.3.0-sim.2" || !list[0].Prerelease {
		t.Fatalf("list = %+v", list)
	}
	a := list[0].Assets[0]
	if a.Digest != "sha256:"+hexSHA([]byte("newer")) || a.Size != 5 || !strings.HasPrefix(a.BrowserDownloadURL, srv.URL+"/dl/") {
		t.Errorf("asset = %+v", a)
	}
	resp, err := srv.Client().Get(a.BrowserDownloadURL)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Errorf("download status %d", resp.StatusCode)
	}
	if r, _ := srv.Client().Get(srv.URL + "/dl/../x/y"); r != nil && r.StatusCode == 200 {
		t.Error("the server served a path outside its root")
	}
	if compareVersions("v0.3.0-sim.10", "v0.3.0-sim.9") <= 0 || compareVersions("v0.3.0", "v0.3.0-sim.1") <= 0 {
		t.Error("release order is wrong")
	}
}
