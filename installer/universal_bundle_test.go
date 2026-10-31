package main

import (
	"archive/zip"
	"bytes"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// The 2018 release's files as v0.2.1-beta.19 publishes them (synthetic bytes in these tests).
const (
	legacyBackendBytes = "beta.19 XINPUT1_3.dll"
	legacyUIBytes      = "beta.19 snapmap-plus-ui.dll"
)

// zipFiles builds a bundle zip with a MANIFEST.sha256 listing every file, as package.ps1 writes it.
func zipFiles(t *testing.T, files map[string]string) []byte {
	t.Helper()
	names := make([]string, 0, len(files))
	for rel := range files {
		names = append(names, rel)
	}
	sort.Strings(names)
	manifest := ""
	for _, rel := range names {
		manifest += strings.ToUpper(sha256Hex([]byte(files[rel]))) + "  " + strings.ReplaceAll(rel, "/", `\`) + "\r\n"
	}
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, rel := range append([]string{"MANIFEST.sha256"}, names...) {
		w, err := zw.Create(rel)
		if err != nil {
			t.Fatal(err)
		}
		if rel == "MANIFEST.sha256" {
			w.Write([]byte(manifest))
		} else {
			w.Write([]byte(files[rel]))
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// universalRelease is a release whose bundle carries the 2024 files and the 2018 release's files, as
// the bundles of v0.2.1-beta.20 and the releases after it did.
func universalRelease(t *testing.T, tag string) *mockRelease {
	t.Helper()
	z := zipFiles(t, map[string]string{
		"XINPUT1_4.dll": "backend-" + tag,
		"snapmap-plus/snapmap-plus-ui-13954591.dll": "ui-" + tag,
		"XINPUT1_3.dll":                    legacyBackendBytes,
		"snapmap-plus/snapmap-plus-ui.dll": legacyUIBytes,
	})
	exe := installerBytes(tag)
	return &mockRelease{tag: tag, prerelease: true, assets: map[string]*mockAsset{
		releaseAsset: {body: z, digest: "sha256:" + sha256Hex(z)},
		selfExeAsset: {body: exe, digest: "sha256:" + sha256Hex(exe)},
	}}
}

// The backend, package.ps1, the CI bundle guard and release.yml name the same paths as the installer.
func TestBundlePathsAgreeAcrossBackendPackagingAndInstaller(t *testing.T) {
	read := func(p string) string {
		b, err := os.ReadFile(filepath.Join("..", p))
		if err != nil {
			t.Skipf("%s not available: %v", p, err)
		}
		return string(b)
	}
	m := regexp.MustCompile(`#define SH_UI_DLL_PATH "\.\\\\(.+)"`).FindStringSubmatch(read("src/backend/ui_bridge.h"))
	if m == nil || strings.ReplaceAll(m[1], `\\`, `\`) != currentUIDll {
		t.Errorf("SH_UI_DLL_PATH = %v, want .\\%s", m, currentUIDll)
	}
	pkg := read("package.ps1")
	if !strings.Contains(pkg, `$uiRel = "`+currentUIDll+`"`) {
		t.Errorf("package.ps1 does not package the frontend as %s", currentUIDll)
	}
	// The bundle carries only the April 2024 pair: the 2018 release's files are no longer packaged.
	for _, gone := range []string{"LegacyBundle", "$legacyRels", "XINPUT1_3.dll"} {
		if strings.Contains(pkg, gone) {
			t.Errorf("package.ps1 still mentions %q", gone)
		}
	}
	mustNot := `$mustNot = @("XINPUT1_3.dll", "snapmap-plus\snapmap-plus-ui.dll")`
	ci := read(".github/workflows/ci.yml")
	if !strings.Contains(ci, `"`+currentUIDll+`"`) {
		t.Errorf("the CI bundle guard does not require %s", currentUIDll)
	}
	if !strings.Contains(ci, mustNot) {
		t.Errorf("the CI bundle guard does not refuse the 2018 pair")
	}
	rel := read(".github/workflows/release.yml")
	if !strings.Contains(rel, `"$GITHUB_REF_NAME" = "`+legacyReleaseTag+`"`) {
		t.Errorf("release.yml no longer refuses to republish %s", legacyReleaseTag)
	}
	for _, gone := range []string{"LegacyBundle", `dist\XINPUT1_3.dll`, "legacy-bundle.zip"} {
		if strings.Contains(rel, gone) {
			t.Errorf("release.yml still packages the 2018 files (%q)", gone)
		}
	}
	if !strings.Contains(rel, mustNot) {
		t.Errorf("release.yml does not refuse a bundle holding the 2018 pair")
	}
}

// On the supported build the universal bundle installs and records only the 2024 files.
func TestUniversalBundleInstallsOnlyTheSupportedBuild(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	m := newMockGitHub(t)
	m.publish(universalRelease(t, "v1.0.0-beta.2"))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatalf("install: %v", err)
	}
	r.installed(t, "v1.0.0-beta.2")
	for _, rel := range legacyBundleFiles {
		if exists(filepath.Join(r.doom, rel)) {
			t.Errorf("the supported build got the 2018 file %s", rel)
		}
	}
	rec, _ := loadRecord()
	if strings.Join(rec.Files, "|") != "XINPUT1_4.dll|"+currentUIDll {
		t.Errorf("recorded files = %q", rec.Files)
	}
}

// After an earlier installer's blind update put all four files on the supported build, the next
// update removes the 2018 files: recorded ones, and unrecorded ones whose bytes are the bundle's.
// A different file at a 2018 path is not the release's and stays.
func TestUpdateRemovesThe2018FilesAnEarlierInstallerLeft(t *testing.T) {
	for _, c := range []struct {
		name     string
		recorded []string
		uiBytes  string
		wantUI   bool
	}{
		{"recorded", []string{"XINPUT1_4.dll", currentUIDll, "XINPUT1_3.dll", `snapmap-plus\snapmap-plus-ui.dll`}, legacyUIBytes, false},
		{"unrecorded", []string{"XINPUT1_4.dll", currentUIDll}, legacyUIBytes, false},
		{"someone else's file", []string{"XINPUT1_4.dll", currentUIDll}, "a different frontend", true},
	} {
		t.Run(c.name, func(t *testing.T) {
			r := newUpdateRig(t, "v1.0.0-beta.2")
			writeF(t, filepath.Join(r.doom, "XINPUT1_4.dll"), "backend-v1.0.0-beta.2")
			writeF(t, filepath.Join(r.doom, currentUIDll), "ui-v1.0.0-beta.2")
			writeF(t, filepath.Join(r.doom, "XINPUT1_3.dll"), legacyBackendBytes)
			writeF(t, filepath.Join(r.doom, "snapmap-plus", "snapmap-plus-ui.dll"), c.uiBytes)
			if err := saveRecord(&installRecord{Version: "v1.0.0-beta.2", DoomPath: r.doom, Files: c.recorded}); err != nil {
				t.Fatal(err)
			}
			m := newMockGitHub(t)
			m.publish(universalRelease(t, "v1.0.0-beta.2"))
			if err := cmdUpdate(flags{yes: true}); err != nil {
				t.Fatalf("update: %v", err)
			}
			r.installed(t, "v1.0.0-beta.2")
			if exists(filepath.Join(r.doom, "XINPUT1_3.dll")) {
				t.Error("the 2018 backend was left on the supported build")
			}
			if got := exists(filepath.Join(r.doom, "snapmap-plus", "snapmap-plus-ui.dll")); got != c.wantUI {
				t.Errorf("snapmap-plus-ui.dll present = %v, want %v", got, c.wantUI)
			}
			if rec, _ := loadRecord(); strings.Join(rec.Files, "|") != "XINPUT1_4.dll|"+currentUIDll {
				t.Errorf("recorded files = %q", rec.Files)
			}
		})
	}
}

// A bundle without the supported build's frontend path is refused before anything changes.
func TestBundleWithoutTheBuildSpecificFrontendIsRefused(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	z := zipFiles(t, map[string]string{"XINPUT1_4.dll": "backend", "snapmap-plus/snapmap-plus-ui.dll": "ui"})
	m := newMockGitHub(t)
	m.publish(&mockRelease{tag: "v1.0.0-beta.2", prerelease: true, assets: map[string]*mockAsset{
		releaseAsset: {body: z, digest: "sha256:" + sha256Hex(z)}}})
	err := cmdInstall(flags{yes: true, doom: r.doom})
	if err == nil || !strings.Contains(err.Error(), currentUIDll) {
		t.Fatalf("err = %v", err)
	}
	if exists(filepath.Join(r.doom, "XINPUT1_4.dll")) {
		t.Error("a refused bundle was installed")
	}
}
