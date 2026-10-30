package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"testing"
)

// These tests run install, update, uninstall and the installer's self-update against a local mock
// of the GitHub Releases API (reached through the SNAPMAP_PLUS_API_BASE loopback seam), so every
// download, digest and manifest check runs as it does against GitHub.

type mockAsset struct {
	body     []byte
	digest   string // advertised "sha256:<hex>"; empty means the release has none
	truncate bool   // send half the bytes under the full Content-Length, then drop the connection
}

type mockRelease struct {
	tag        string
	prerelease bool
	assets     map[string]*mockAsset
}

type mockGitHub struct {
	t        *testing.T
	srv      *httptest.Server
	mu       sync.Mutex
	releases []*mockRelease // newest first, as GitHub lists them
}

func sha256Hex(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

func newMockGitHub(t *testing.T) *mockGitHub {
	t.Helper()
	m := &mockGitHub{t: t}
	m.srv = httptest.NewServer(http.HandlerFunc(m.serve))
	t.Cleanup(m.srv.Close)
	t.Setenv(apiBaseEnv, m.srv.URL)
	return m
}

// publish adds a release ahead of the existing ones.
func (m *mockGitHub) publish(r *mockRelease) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.releases = append([]*mockRelease{r}, m.releases...)
}

func (m *mockGitHub) json(r *mockRelease) ghRelease {
	out := ghRelease{TagName: r.tag, Name: r.tag, Prerelease: r.prerelease}
	names := make([]string, 0, len(r.assets))
	for name := range r.assets {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		a := r.assets[name]
		u := m.srv.URL + "/dl/" + r.tag + "/" + name
		out.Assets = append(out.Assets, ghAsset{Name: name, URL: u, BrowserDownloadURL: u, Digest: a.digest, Size: int64(len(a.body))})
	}
	return out
}

func (m *mockGitHub) serve(w http.ResponseWriter, req *http.Request) {
	m.mu.Lock()
	defer m.mu.Unlock()
	base := "/repos/" + repoSlug + "/releases"
	path := req.URL.Path
	switch {
	case path == base:
		list := []ghRelease{}
		for _, r := range m.releases {
			list = append(list, m.json(r))
		}
		json.NewEncoder(w).Encode(list)
	case path == base+"/latest":
		for _, r := range m.releases {
			if !r.prerelease {
				json.NewEncoder(w).Encode(m.json(r))
				return
			}
		}
		http.NotFound(w, req)
	case strings.HasPrefix(path, base+"/tags/"):
		for _, r := range m.releases {
			if r.tag == strings.TrimPrefix(path, base+"/tags/") {
				json.NewEncoder(w).Encode(m.json(r))
				return
			}
		}
		http.NotFound(w, req)
	case strings.HasPrefix(path, "/dl/"):
		parts := strings.SplitN(strings.TrimPrefix(path, "/dl/"), "/", 2)
		for _, r := range m.releases {
			if a := r.assets[parts[len(parts)-1]]; r.tag == parts[0] && a != nil {
				if a.truncate {
					conn, buf, err := w.(http.Hijacker).Hijack()
					if err != nil {
						m.t.Errorf("hijack: %v", err)
						return
					}
					fmt.Fprintf(buf, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Type: application/octet-stream\r\n\r\n", len(a.body))
					buf.Write(a.body[:len(a.body)/2])
					buf.Flush()
					conn.Close()
					return
				}
				w.Write(a.body)
				return
			}
		}
		http.NotFound(w, req)
	default:
		http.NotFound(w, req)
	}
}

// zipBundle builds snapmap-plus-bundle.zip as release.yml does: the two DLLs and MANIFEST.sha256.
// badManifest lists a wrong hash for the backend, as a bundle whose manifest does not match would.
func zipBundle(t *testing.T, tag string, badManifest bool) []byte {
	t.Helper()
	files := map[string]string{
		"XINPUT1_4.dll":                    "backend-" + tag,
		"snapmap-plus/snapmap-plus-ui.dll": "ui-" + tag,
	}
	manifest := ""
	for _, rel := range []string{"XINPUT1_4.dll", "snapmap-plus/snapmap-plus-ui.dll"} {
		sum := sha256Hex([]byte(files[rel]))
		if badManifest && rel == "XINPUT1_4.dll" {
			sum = sha256Hex([]byte("something else"))
		}
		manifest += strings.ToUpper(sum) + "  " + strings.ReplaceAll(rel, "/", `\`) + "\r\n"
	}
	files["MANIFEST.sha256"] = manifest
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, name := range []string{"MANIFEST.sha256", "XINPUT1_4.dll", "snapmap-plus/snapmap-plus-ui.dll"} {
		f, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		f.Write([]byte(files[name]))
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func installerBytes(tag string) []byte { return []byte("snapmap-plus.exe " + tag) }

// goodRelease is a release whose bundle and installer match their digests.
func goodRelease(t *testing.T, tag string, prerelease bool) *mockRelease {
	t.Helper()
	z, exe := zipBundle(t, tag, false), installerBytes(tag)
	return &mockRelease{tag: tag, prerelease: prerelease, assets: map[string]*mockAsset{
		releaseAsset: {body: z, digest: "sha256:" + sha256Hex(z)},
		selfExeAsset: {body: exe, digest: "sha256:" + sha256Hex(exe)},
	}}
}

// updateRig is a temporary DOOM root, app data, profile, temp folder and a stand-in for the
// running snapmap-plus.exe, with the installer's version set to runningTag.
type updateRig struct {
	tmp, doom, data, temp, exe string
}

func newUpdateRig(t *testing.T, runningTag string) *updateRig {
	t.Helper()
	syntheticProcessGuard(t, false)
	tmp := t.TempDir()
	r := &updateRig{
		tmp:  tmp,
		doom: doomWith(t, tmp, fakeExe, fakeGame),
		data: filepath.Join(tmp, "appdata", "snapmap-plus"),
		temp: filepath.Join(tmp, "temp"),
		exe:  filepath.Join(tmp, "bin", selfExeAsset),
	}
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	t.Setenv("USERPROFILE", filepath.Join(tmp, "profile"))
	mkdirAll(t, r.temp)
	t.Setenv("TMP", r.temp)
	t.Setenv("TEMP", r.temp)
	writeF(t, r.exe, string(installerBytes(runningTag)))
	savedExe, savedVersion := executablePath, version
	executablePath = func() (string, error) { return r.exe, nil }
	version = runningTag
	t.Cleanup(func() { executablePath, version = savedExe, savedVersion })
	return r
}

// seedUserData writes the player data the installer must leave exactly as it is: the backend's
// config, prefabs and strings, a mods folder holding both the pre-Contract-5 layout and the
// current one, and the retired overrides and override-backups folders with packages in them.
func (r *updateRig) seedUserData(t *testing.T) {
	t.Helper()
	for rel, content := range map[string]string{
		"config.json":                                        `{"schema_version":1,"settings":{"theme":"dark"}}`,
		`prefabs\arena.json`:                                 `{"prefab":1}`,
		`strings\en.json`:                                    `{"hello":"world"}`,
		`mods\boss@1.0.0+0123abcd\bundle.json`:               `{"id":"boss","version":"1.0.0"}`,
		`mods\boss@1.0.0+0123abcd\.state.json`:               `{"state":"installed"}`,
		`mods\blobs\9f86d081884c7d65`:                        "blob bytes",
		`mods\legacy\pack@0123456789abcdef\bundle.json`:      `{"id":"pack"}`,
		`mods\claims.json`:                                   `{"claims":[]}`,
		`mods\publish-links.json`:                            `{}`,
		`mods\imp\mod.json`:                                  `{"id":"imp","version":"2.0.0"}`,
		`mods\imp\decls\entitydef\imp.decl`:                  "entityDef imp {}",
		`mods\snapmap-plus.json`:                             `{"schema":1}`,
		`overrides\boss\package.json`:                        `{"id":"boss"}`,
		`overrides\boss\generated\decls\entitydef\boss.decl`: "entityDef boss {}",
		`overrides\map-0123456789abcdef\pack\package.json`:   `{"id":"pack"}`,
		`override-backups\old\note.txt`:                      "kept",
	} {
		writeF(t, filepath.Join(r.data, rel), content)
	}
}

// userDataNames are the app-data entries that belong to the player or the backend.
var userDataNames = []string{"config.json", "prefabs", "strings", "mods", "overrides", "override-backups", "package-cache", "package-staging"}

// userSnapshot maps every user-data path to its SHA-256 ("dir" for a folder).
func (r *updateRig) userSnapshot(t *testing.T) map[string]string {
	t.Helper()
	snap := map[string]string{}
	for _, name := range userDataNames {
		root := filepath.Join(r.data, name)
		if _, err := os.Lstat(root); err != nil {
			continue
		}
		filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				t.Fatal(err)
			}
			rel, _ := filepath.Rel(r.data, path)
			if d.IsDir() {
				snap[rel] = "dir"
				return nil
			}
			b, rerr := os.ReadFile(path)
			if rerr != nil {
				t.Fatal(rerr)
			}
			snap[rel] = sha256Hex(b)
			return nil
		})
	}
	return snap
}

func sameSnapshot(t *testing.T, stage string, before, after map[string]string) {
	t.Helper()
	for k, v := range before {
		if after[k] != v {
			t.Errorf("%s changed or removed user data %s", stage, k)
		}
	}
	for k := range after {
		if _, ok := before[k]; !ok {
			t.Errorf("%s created user data %s", stage, k)
		}
	}
}

func (r *updateRig) noTempLeftovers(t *testing.T, stage string) {
	t.Helper()
	entries, err := os.ReadDir(r.temp)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		t.Errorf("%s left %s in the temp folder", stage, e.Name())
	}
}

func (r *updateRig) installed(t *testing.T, tag string) {
	t.Helper()
	if got := readF(t, filepath.Join(r.doom, "XINPUT1_4.dll")); got != "backend-"+tag {
		t.Errorf("XINPUT1_4.dll = %q, want the %s backend", got, tag)
	}
	if got := readF(t, filepath.Join(r.doom, "snapmap-plus", "snapmap-plus-ui.dll")); got != "ui-"+tag {
		t.Errorf("snapmap-plus-ui.dll = %q, want the %s frontend", got, tag)
	}
	if rec, err := loadRecord(); err != nil || rec.Version != tag {
		t.Errorf("install.json = %+v, %v; want version %s", rec, err, tag)
	}
	for _, rel := range []string{"XINPUT1_4.dll.snapmap-plus-bak", `snapmap-plus\snapmap-plus-ui.dll.snapmap-plus-bak`} {
		if exists(filepath.Join(r.doom, rel)) {
			t.Errorf("an update backed up the installer's own %s", rel)
		}
	}
}

func TestAPIBaseOverrideIsLoopbackOnly(t *testing.T) {
	cases := map[string]string{
		"":                           githubAPI,
		"http://127.0.0.1:8080/":     "http://127.0.0.1:8080",
		"http://localhost:9":         "http://localhost:9",
		"http://[::1]:7":             "http://[::1]:7",
		"https://api.example.com":    githubAPI,
		"http://127.0.0.1.evil.test": githubAPI,
		"file:///C:/releases":        githubAPI,
	}
	for in, want := range cases {
		t.Setenv(apiBaseEnv, in)
		if got := apiBase(); got != want {
			t.Errorf("apiBase with %q = %q, want %q", in, got, want)
		}
	}
	t.Setenv(apiBaseEnv, "")
	if got := releasesURL(); got != "https://api.github.com/repos/"+repoSlug+"/releases" {
		t.Errorf("default releases URL = %q", got)
	}
}

// The default channel takes the newest stable release, or the newest beta while none exists;
// --beta takes the newest prerelease; --release takes that tag.
func TestFetchReleaseChannels(t *testing.T) {
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	if r, err := fetchRelease(flags{}, ""); err != nil || r.TagName != "v1.0.0-beta.1" {
		t.Fatalf("default with no stable release = %+v, %v; want the newest beta", r, err)
	}
	m.publish(goodRelease(t, "v1.0.0", false))
	m.publish(goodRelease(t, "v1.1.0-beta.1", true))
	for _, c := range []struct {
		f    flags
		want string
	}{
		{flags{}, "v1.0.0"},
		{flags{beta: true}, "v1.1.0-beta.1"},
		{flags{release: "v1.0.0-beta.1"}, "v1.0.0-beta.1"},
	} {
		if r, err := fetchRelease(c.f, ""); err != nil || r.TagName != c.want {
			t.Errorf("fetchRelease(%+v) = %+v, %v; want %s", c.f, r, err, c.want)
		}
	}
	if _, err := fetchRelease(flags{release: "v9.9.9"}, ""); err == nil || !strings.Contains(err.Error(), "404") {
		t.Errorf("an unknown tag: %v", err)
	}
}

// A newer release replaces both DLLs and the installer's own image, records the update, and
// leaves the player's data, including mods and retired package folders, exactly as it was.
func TestUpdateFromMockInstallsNewerRelease(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	r.seedUserData(t)
	before := r.userSnapshot(t)
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("install: %v", err)
	}
	r.installed(t, "v1.0.0-beta.1")

	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	if err := cmdUpdate(flags{yes: true}); err != nil {
		t.Fatalf("update: %v", err)
	}
	r.installed(t, "v1.0.0-beta.2")
	if got := readF(t, r.exe); got != string(installerBytes("v1.0.0-beta.2")) {
		t.Errorf("snapmap-plus.exe = %q, want the new release's installer", got)
	}
	if got := readF(t, r.exe+".old"); got != string(installerBytes("v1.0.0-beta.1")) {
		t.Errorf("the replaced image was not kept aside: %q", got)
	}
	if rec, err := loadUpdateRecord(r.exe); err != nil || rec.Backups[filepath.Base(r.exe)+".old"] == "" {
		t.Errorf("the replaced image is not recorded for cleanup: %+v, %v", rec, err)
	}
	if got := readF(t, filepath.Join(r.data, selfExeAsset)); got != string(installerBytes("v1.0.0-beta.2")) {
		t.Errorf("the stable app-data copy = %q, want the new installer", got)
	}
	sameSnapshot(t, "install and update", before, r.userSnapshot(t))
	r.noTempLeftovers(t, "update")

	// The next run sweeps the recorded backup.
	cleanupSelfUpdateLeftovers()
	if exists(r.exe + ".old") {
		t.Error("the recorded backup survived the next run's cleanup")
	}
}

// With no newer release, the installer keeps its image and the update rewrites identical DLLs.
func TestUpdateFromMockWithNoNewerRelease(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("install: %v", err)
	}
	if err := cmdUpdate(flags{yes: true}); err != nil {
		t.Fatalf("update: %v", err)
	}
	r.installed(t, "v1.0.0-beta.2")
	if got := readF(t, r.exe); got != string(installerBytes("v1.0.0-beta.2")) || exists(r.exe+".old") || exists(r.exe+".updates.json") {
		t.Error("an update with no newer release replaced the installer")
	}
	r.noTempLeftovers(t, "update")
}

// An installer asset whose bytes differ from the release digest is refused; the old image stays.
func TestSelfUpdateRefusesCorruptInstaller(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("install: %v", err)
	}
	bad := goodRelease(t, "v1.0.0-beta.2", true)
	bad.assets[selfExeAsset].body = []byte("snapmap-plus.exe tampered")
	m.publish(bad)
	if err := cmdUpdate(flags{yes: true}); err != nil {
		t.Fatalf("update: %v", err)
	}
	if got := readF(t, r.exe); got != string(installerBytes("v1.0.0-beta.1")) {
		t.Fatalf("a corrupt installer replaced the running image: %q", got)
	}
	if exists(r.exe+".old") || exists(r.exe+".updates.json") {
		t.Error("a refused self-update moved the running image aside")
	}
	if got := readF(t, filepath.Join(r.data, selfExeAsset)); got != string(installerBytes("v1.0.0-beta.1")) {
		t.Errorf("a corrupt installer replaced the stable copy: %q", got)
	}
	r.noTempLeftovers(t, "refused self-update")
}

// A bundle whose bytes differ from the release digest, or whose MANIFEST does not match its
// files, is refused before the DOOM folder, the record or the installer changes.
func TestUpdateRefusesCorruptBundles(t *testing.T) {
	for _, c := range []struct {
		name string
		edit func(*mockRelease)
		want string
	}{
		{"digest", func(rel *mockRelease) {
			a := rel.assets[releaseAsset]
			a.body = append(append([]byte{}, a.body...), 0)
		}, "integrity check failed on " + releaseAsset},
		{"manifest", func(rel *mockRelease) {
			z := zipBundle(t, rel.tag, true)
			rel.assets[releaseAsset] = &mockAsset{body: z, digest: "sha256:" + sha256Hex(z)}
		}, "integrity check failed on XINPUT1_4.dll"},
	} {
		t.Run(c.name, func(t *testing.T) {
			r := newUpdateRig(t, "v1.0.0-beta.1")
			r.seedUserData(t)
			before := r.userSnapshot(t)
			m := newMockGitHub(t)
			m.publish(goodRelease(t, "v1.0.0-beta.1", true))
			if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
				t.Fatalf("install: %v", err)
			}
			bad := goodRelease(t, "v1.0.0-beta.2", true)
			c.edit(bad)
			m.publish(bad)
			err := cmdUpdate(flags{yes: true})
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("update: %v, want %q", err, c.want)
			}
			r.installed(t, "v1.0.0-beta.1")
			if got := readF(t, r.exe); got != string(installerBytes("v1.0.0-beta.1")) {
				t.Error("a refused update replaced the installer")
			}
			sameSnapshot(t, "refused update", before, r.userSnapshot(t))
			r.noTempLeftovers(t, "refused update")
		})
	}
}

// A download cut short is refused and its temporary files are removed.
func TestUpdateInterruptedDownloadsCleanUp(t *testing.T) {
	for _, c := range []struct {
		name, asset string
		noDigest    bool
	}{
		{"bundle", releaseAsset, false},
		{"bundle-without-digest", releaseAsset, true},
		{"installer", selfExeAsset, false},
		{"installer-without-digest", selfExeAsset, true},
	} {
		t.Run(c.name, func(t *testing.T) {
			r := newUpdateRig(t, "v1.0.0-beta.1")
			m := newMockGitHub(t)
			m.publish(goodRelease(t, "v1.0.0-beta.1", true))
			if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
				t.Fatalf("install: %v", err)
			}
			cut := goodRelease(t, "v1.0.0-beta.2", true)
			cut.assets[c.asset].truncate = true
			if c.noDigest {
				cut.assets[c.asset].digest = ""
			}
			m.publish(cut)
			err := cmdUpdate(flags{yes: true})
			if c.asset == releaseAsset {
				if err == nil || !strings.Contains(err.Error(), "did not complete") {
					t.Fatalf("update: %v, want an incomplete-download refusal", err)
				}
				r.installed(t, "v1.0.0-beta.1")
			} else {
				if err != nil {
					t.Fatalf("update: %v", err)
				}
				r.installed(t, "v1.0.0-beta.2")
			}
			if got := readF(t, r.exe); got != string(installerBytes("v1.0.0-beta.1")) || exists(r.exe+".old") {
				t.Errorf("an interrupted download changed the installer: %q", got)
			}
			r.noTempLeftovers(t, "interrupted download")
		})
	}
}

// Install, update, uninstall and reinstall never create, change or remove mods\, the retired
// package folders or the backend's config: the backend converts them at its first start.
func TestLifecycleLeavesModsAndRetiredFolders(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	r.seedUserData(t)
	before := r.userSnapshot(t)
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("install: %v", err)
	}
	sameSnapshot(t, "install", before, r.userSnapshot(t))
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	if err := cmdUpdate(flags{yes: true}); err != nil {
		t.Fatalf("update: %v", err)
	}
	sameSnapshot(t, "update", before, r.userSnapshot(t))
	if err := cmdUninstall(flags{yes: true}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	for _, rel := range []string{"XINPUT1_4.dll", "snapmap-plus"} {
		if exists(filepath.Join(r.doom, rel)) {
			t.Errorf("uninstall left %s", rel)
		}
	}
	if exists(filepath.Join(r.data, "install.json")) {
		t.Error("uninstall left install.json")
	}
	sameSnapshot(t, "uninstall", before, r.userSnapshot(t))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("reinstall: %v", err)
	}
	r.installed(t, "v1.0.0-beta.2")
	sameSnapshot(t, "reinstall", before, r.userSnapshot(t))
	r.noTempLeftovers(t, "lifecycle")
}
