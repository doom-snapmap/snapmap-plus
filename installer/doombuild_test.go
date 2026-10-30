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
	knownDoomBuilds[digestOf(fakeExe)] = doomBuild{build2024, "test 13954591", build2024, binary2024, true}
	knownDoomBuilds[digestOf(fakeGame)] = doomBuild{build2024, "test 13954591", build2024, binary2024, false}
	knownDoomBuilds[digestOf(fake2018)] = doomBuild{build2018, "test 2018", "2642713", binary2018, true}
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
	running := filepath.Join(t.TempDir(), selfExeAsset)
	writeF(t, running, "installer")
	savedExe := executablePath
	executablePath = func() (string, error) { return running, nil }
	t.Cleanup(func() { executablePath = savedExe })
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
		if exists(filepath.Join(tmp, "appdata", "snapmap-plus", selfExeAsset)) {
			t.Errorf("case %d: a refused install copied snapmap-plus.exe into app data", i)
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

// The installer prints the same game identity string the backend and the reports use.
func TestGameIdentityMatchesBackendFormat(t *testing.T) {
	vk := knownDoomBuilds["3d8b4393649f80bdae85c19ae2c985c45ec9ac54f74ab04a18816e30ead63c21"]
	gl := knownDoomBuilds["1bddecaa4e86850412f96e3117978bce1ec7bb701ae1b5a33120c21e4421e51e"]
	old := knownDoomBuilds["139763e94f1a75b5310179f9eeeb8a949a1f53c49acbc722fcfc5dfe7bb6d323"]
	cases := []struct {
		b         doomBuild
		sum, want string
	}{
		{vk, "3D8B4393649F80BDAE85C19AE2C985C45EC9AC54F74AB04A18816E30EAD63C21",
			"DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)"},
		{gl, "1bddecaa4e86850412f96e3117978bce1ec7bb701ae1b5a33120c21e4421e51e",
			"DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, OpenGL, 1bddecaa)"},
		{old, "139763e94f1a75b5310179f9eeeb8a949a1f53c49acbc722fcfc5dfe7bb6d323",
			"DOOM 2016 build 2642713 (20180321-154133-liver-goldenrod, Vulkan, 139763e9)"},
	}
	for _, c := range cases {
		if got := gameIdentity(c.b, c.sum); got != c.want {
			t.Errorf("gameIdentity = %q, want %q", got, c.want)
		}
	}
}

func TestDescribeDoomGames(t *testing.T) {
	doom := doomWith(t, t.TempDir(), fakeExe, fakeMod)
	lines := describeDoomGames(doom)
	if len(lines) != 2 {
		t.Fatalf("lines = %q", lines)
	}
	want := "DOOMx64vk.exe: DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, " + digestOf(fakeExe)[:8] + ")"
	if lines[0] != want {
		t.Errorf("line 0 = %q, want %q", lines[0], want)
	}
	if lines[1] != "DOOMx64.exe: unrecognized executable (SHA-256 "+digestOf(fakeMod)[:8]+")" {
		t.Errorf("line 1 = %q", lines[1])
	}
	if got := describeDoomGames(t.TempDir()); len(got) != 0 {
		t.Errorf("empty folder: %q", got)
	}
}

// install.ps1 resolves releases the way snapmap-plus.exe does: through the Releases API, falling
// back to the newest beta (releases/latest/download never serves one), verifying the downloaded
// installer against the published digest and stopping when an installer exits with an error.
func TestBootstrapScriptResolvesAndVerifiesReleases(t *testing.T) {
	script, err := os.ReadFile("install.ps1")
	if err != nil {
		t.Fatal(err)
	}
	s := string(script)
	if strings.Contains(s, "releases/latest/download") {
		t.Error("install.ps1 downloads from releases/latest/download, which 404s while only betas are published")
	}
	for _, want := range []string{"Get-FileHash", "$asset.digest", "$asset.size", "SNAPMAP_PLUS_API_BASE", "-not $_.prerelease"} {
		if !strings.Contains(s, want) {
			t.Errorf("install.ps1 lacks %q", want)
		}
	}
	if n := strings.Count(s, "if ($LASTEXITCODE -ne 0)"); n != 2 {
		t.Errorf("install.ps1 checks %d installer exit codes, want 2 (the 2018 and the current path)", n)
	}
}
