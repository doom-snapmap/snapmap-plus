package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// Synthetic executables used by the installer tests stand in for the real DOOM executables, so
// their digests are registered as recognized builds for this test binary.
const (
	fakeExe  = "exe"  // the lifecycle tests: the supported build
	fakeGame = "game" // TestRuntimeConfigSurvivesInstallerLifecycle and friends: also supported
	fake2018 = "doom-2018-wrapped"
	fakeMod  = "patched-by-a-mod-loader"
)

func digestOf(content string) string {
	sum := sha256.Sum256([]byte(content))
	return hex.EncodeToString(sum[:])
}

func init() {
	knownDoomBuilds[digestOf(fakeExe)] = doomBuild{build2024, "test 13954591"}
	knownDoomBuilds[digestOf(fakeGame)] = doomBuild{build2024, "test 13954591"}
	knownDoomBuilds[digestOf(fake2018)] = doomBuild{build2018, "test 2018"}
}

// bundleDir writes a bundle shaped like package.ps1's output with the given backend name.
func bundleDir(t *testing.T, dir, proxy, tag string) string {
	t.Helper()
	overlay := map[string]string{
		proxy: "backend-" + tag,
		filepath.Join("snapmap-plus", "snapmap-plus-ui.dll"): "ui-" + tag,
	}
	manifest := ""
	for rel, content := range overlay {
		writeF(t, filepath.Join(dir, rel), content)
		manifest += digestOf(content) + "  " + rel + "\n"
	}
	writeF(t, filepath.Join(dir, "MANIFEST.sha256"), manifest)
	return dir
}

// stubDownloads replaces the download step: it records which release each install asked for
// and serves the matching synthetic bundle (the legacy tag gets an XINPUT1_3.dll bundle).
func stubDownloads(t *testing.T, tmp string) *[]string {
	t.Helper()
	var asked []string
	saved := downloadReleaseFn
	downloadReleaseFn = func(f flags) (string, string, func(), error) {
		asked = append(asked, f.release)
		if f.release == legacyReleaseTag {
			return bundleDir(t, filepath.Join(tmp, "legacy-bundle"), "XINPUT1_3.dll", "legacy"), legacyReleaseTag, func() {}, nil
		}
		return bundleDir(t, filepath.Join(tmp, "latest-bundle"), "XINPUT1_4.dll", "latest"), "v9.9.9", func() {}, nil
	}
	t.Cleanup(func() { downloadReleaseFn = saved })
	return &asked
}

func doomWith(t *testing.T, tmp, vk, gl string) string {
	t.Helper()
	doom := filepath.Join(tmp, "DOOM")
	mkdirAll(t, doom)
	if vk != "" {
		writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), vk)
	}
	if gl != "" {
		writeF(t, filepath.Join(doom, "DOOMx64.exe"), gl)
	}
	return doom
}

func TestDetectDoomBuildByHash(t *testing.T) {
	cases := []struct {
		vk, gl, wantBuild, wantErr string
	}{
		{fakeExe, "", build2024, ""},
		{"", fakeExe, build2024, ""},
		{fakeExe, fakeGame, build2024, ""},
		{fake2018, fake2018, build2018, ""},
		{fakeMod, "", "", "is not a DOOM 2016 executable Snapmap+ recognizes"},
		{fakeExe, fakeMod, "", "Steam build 13954591"},
		{fakeExe, fake2018, "", "must come from the same DOOM update"},
		{"", "", "", "no DOOM 2016 executable"},
	}
	for i, c := range cases {
		b, err := detectDoomBuild(doomWith(t, t.TempDir(), c.vk, c.gl))
		if c.wantErr != "" {
			if err == nil || !strings.Contains(err.Error(), c.wantErr) {
				t.Errorf("case %d: err = %v, want it to mention %q", i, err, c.wantErr)
			}
			continue
		}
		if err != nil || b.build != c.wantBuild {
			t.Errorf("case %d: got %+v, %v; want build %s", i, b, err, c.wantBuild)
		}
	}
}

// The supported build downloads the current release and installs XINPUT1_4.dll.
func TestInstallSupportedBuildTakesLatest(t *testing.T) {
	syntheticProcessGuard(t, false)
	tmp := t.TempDir()
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	asked := stubDownloads(t, tmp)
	doom := doomWith(t, tmp, fakeExe, fakeExe)
	if err := cmdInstall(flags{doom: doom, yes: true}); err != nil {
		t.Fatalf("install: %v", err)
	}
	if len(*asked) != 1 || (*asked)[0] != "" {
		t.Fatalf("download requests = %q, want one request for the latest release", *asked)
	}
	if readF(t, filepath.Join(doom, "XINPUT1_4.dll")) != "backend-latest" || exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Fatal("the supported build must get the current release's XINPUT1_4.dll and nothing else")
	}
	if rec, err := loadRecord(); err != nil || rec.Version != "v9.9.9" {
		t.Fatalf("record = %+v, %v", rec, err)
	}
}

// The 2018 build downloads the final 2018 release by tag and installs it as that release would;
// moving the game to the supported build later replaces it with the current release.
func TestInstall2018BuildTakesLegacyRelease(t *testing.T) {
	syntheticProcessGuard(t, false)
	tmp := t.TempDir()
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	asked := stubDownloads(t, tmp)
	doom := doomWith(t, tmp, fake2018, fake2018)
	if err := cmdInstall(flags{doom: doom, yes: true, beta: true}); err != nil {
		t.Fatalf("install on 2018: %v", err)
	}
	if len(*asked) != 1 || (*asked)[0] != legacyReleaseTag {
		t.Fatalf("download requests = %q, want %s", *asked, legacyReleaseTag)
	}
	if readF(t, filepath.Join(doom, "XINPUT1_3.dll")) != "backend-legacy" || exists(filepath.Join(doom, "XINPUT1_4.dll")) {
		t.Fatal("the 2018 build must get the legacy release's XINPUT1_3.dll")
	}
	if rec, err := loadRecord(); err != nil || rec.Version != legacyReleaseTag {
		t.Fatalf("record = %+v, %v", rec, err)
	}

	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), fakeExe)
	writeF(t, filepath.Join(doom, "DOOMx64.exe"), fakeExe)
	if err := cmdInstall(flags{doom: doom, yes: true}); err != nil {
		t.Fatalf("update after the game moved to 13954591: %v", err)
	}
	if !exists(filepath.Join(doom, "XINPUT1_4.dll")) || exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Fatal("after the move only the current release's XINPUT1_4.dll may remain")
	}
}

// Unknown, mixed and mismatched requests are refused before any download or write.
func TestInstallRefusalsDownloadNothing(t *testing.T) {
	syntheticProcessGuard(t, false)
	cases := []struct {
		vk, gl string
		f      flags
		want   string
	}{
		{fakeMod, "", flags{}, "Verify integrity"},
		{fakeExe, fake2018, flags{}, "same DOOM update"},
		{fake2018, "", flags{release: "v0.3.0"}, legacyReleaseTag},
		{fakeExe, "", flags{release: legacyReleaseTag}, "final release for the 2018 build"},
	}
	for i, c := range cases {
		tmp := t.TempDir()
		t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
		asked := stubDownloads(t, tmp)
		doom := doomWith(t, tmp, c.vk, c.gl)
		f := c.f
		f.doom, f.yes = doom, true
		err := cmdInstall(f)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("case %d: err = %v, want it to mention %q", i, err, c.want)
		}
		if len(*asked) != 0 {
			t.Errorf("case %d: refused install still downloaded %q", i, *asked)
		}
		entries, _ := os.ReadDir(doom)
		if len(entries) > 2 {
			t.Errorf("case %d: a refused install wrote into the DOOM folder", i)
		}
	}
}

// A local build targets the supported build only; a pre-port bundle without XINPUT1_4.dll is refused.
func TestLocalBundleRouting(t *testing.T) {
	syntheticProcessGuard(t, false)
	tmp := t.TempDir()
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	doom2018 := doomWith(t, filepath.Join(tmp, "a"), fake2018, "")
	if err := cmdInstall(flags{doom: doom2018, local: bundleDir(t, filepath.Join(tmp, "b1"), "XINPUT1_4.dll", "l")}); err == nil ||
		!strings.Contains(err.Error(), "local Snapmap+ build") {
		t.Fatalf("local build on 2018: err = %v", err)
	}
	doom := doomWith(t, filepath.Join(tmp, "c"), fakeExe, "")
	if err := cmdInstall(flags{doom: doom, local: bundleDir(t, filepath.Join(tmp, "b2"), "XINPUT1_3.dll", "old")}); err == nil ||
		!strings.Contains(err.Error(), "XINPUT1_4.dll") {
		t.Fatalf("pre-port bundle on 13954591: err = %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Fatal("the wrong backend must never be placed")
	}
}

// The installer's table must name exactly the backend's supported digests plus the 2018 ones it routes.
func TestKnownBuildsMatchBackendTable(t *testing.T) {
	src, err := os.ReadFile(filepath.Join("..", "src", "backend", "host_image.c"))
	if err != nil {
		t.Skipf("backend source not available: %v", err)
	}
	table := strings.SplitN(strings.SplitN(string(src), "SH_KNOWN_BUILDS[] = {", 2)[1], "\n};", 2)[0]
	re := regexp.MustCompile(`\{\{([0-9a-fx,\s]+)\},\s*\d,\s*"`)
	backend := map[string]bool{}
	for _, m := range re.FindAllStringSubmatch(table, -1) {
		var sb strings.Builder
		for _, b := range strings.Split(m[1], ",") {
			v, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimSpace(b), "0x"), 16, 8)
			if err != nil {
				t.Fatalf("parse %q: %v", b, err)
			}
			sb.WriteString(hex.EncodeToString([]byte{byte(v)}))
		}
		backend[sb.String()] = true
	}
	if len(backend) != 2 {
		t.Fatalf("parsed %d backend digests, want 2", len(backend))
	}
	supported, legacy := 0, 0
	for digest, b := range knownDoomBuilds {
		if strings.HasPrefix(b.label, "test ") {
			continue
		}
		switch b.build {
		case build2024:
			supported++
			if !backend[digest] {
				t.Errorf("installer digest %s (%s) is not supported by the backend", digest, b.label)
			}
		case build2018:
			legacy++
			if backend[digest] {
				t.Errorf("2018 digest %s must not be a backend-supported build", digest)
			}
		}
	}
	if supported != 2 || legacy != 2 {
		t.Errorf("installer knows %d supported and %d 2018 digests, want 2 and 2", supported, legacy)
	}
}

// install.ps1 must route by the same digests and the same legacy tag.
func TestBootstrapScriptMatchesRouting(t *testing.T) {
	script, err := os.ReadFile("install.ps1")
	if err != nil {
		t.Fatal(err)
	}
	s := strings.ToLower(string(script))
	for digest, b := range knownDoomBuilds {
		if strings.HasPrefix(b.label, "test ") {
			continue
		}
		if !strings.Contains(s, digest) {
			t.Errorf("install.ps1 lacks the %s digest %s", b.label, digest)
		}
	}
	if !strings.Contains(string(script), legacyReleaseTag) {
		t.Errorf("install.ps1 lacks the legacy tag %s", legacyReleaseTag)
	}
}
