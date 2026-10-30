package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Snapmap+ supports one DOOM 2016 build: the April 2024 update (Steam build 13954591), whose
// executables import XINPUT1_4.dll. The 2018 build (Steam build 2642713) is served by the final
// release made for it, legacyReleaseTag, installed exactly as that release's installer would.
// The installer identifies the build by the SHA-256 of the installed executables before it
// downloads anything (the same digests as src/backend/host_image.c and installer/install.ps1).

// legacyReleaseTag is the final Snapmap+ release for the 2018 DOOM build. It must stay
// published unchanged: release.yml refuses to rebuild it (see docs/packaging.md).
const legacyReleaseTag = "v0.2.1-beta.19"

// currentProxy is the backend's file name for the supported build.
const currentProxy = "XINPUT1_4.dll"

// currentUIDll is the supported build's frontend, at a path that names the build so it never
// collides with the 2018 release's frontend. It is SH_UI_DLL_PATH in src/backend/ui_bridge.h and
// $uiRel in package.ps1 (a test checks all three).
const currentUIDll = `snapmap-plus\snapmap-plus-ui-13954591.dll`

// legacyBundleFiles are the 2018 release's files, which every release bundle also carries byte for
// byte. Installers without build detection (legacyReleaseTag and earlier) install every file a
// bundle lists, so on the 2018 build their update leaves these in place and the 2024 files are
// inert (the 2018 executables import XINPUT1_3.dll). This installer never installs them for the
// supported build, and removes them when an earlier installer left them there.
var legacyBundleFiles = []string{"XINPUT1_3.dll", `snapmap-plus\snapmap-plus-ui.dll`}

func isLegacyBundleFile(rel string) bool {
	for _, l := range legacyBundleFiles {
		if strings.EqualFold(filepath.Clean(rel), l) {
			return true
		}
	}
	return false
}

// splitBundleForBuild returns the files to install for the supported build and the 2018 files
// the bundle also carries.
func splitBundleForBuild(files []manifestEntry) (install, legacy []manifestEntry) {
	for _, e := range files {
		if isLegacyBundleFile(e.rel) {
			legacy = append(legacy, e)
		} else {
			install = append(install, e)
		}
	}
	return install, legacy
}

const (
	build2018    = "2018"
	build2024    = "13954591"
	label2018    = "2018 build (Steam build 2642713)"
	label2024    = "April 2024 update (Steam build 13954591)"
	supportedMsg = "Snapmap+ supports the April 2024 update (Steam build 13954591); the 2018 build gets the final 2018 release " + legacyReleaseTag
)

// doomBuild describes one recognized executable.
type doomBuild struct {
	build  string // build2018 or build2024
	label  string // for messages
	steam  string // Steam build id
	binary string // the console's Build Information binary name
	vulkan bool   // the Vulkan executable (DOOMx64vk.exe) rather than OpenGL
}

const (
	binary2024 = "20240321-104810-ginger-fuchsia"
	binary2018 = "20180321-154133-liver-goldenrod"
)

// knownDoomBuilds maps an executable's lowercase SHA-256 to its build. Tests add synthetic entries.
var knownDoomBuilds = map[string]doomBuild{
	// April 2024 update, Steam build 13954591 (not SteamStub-wrapped).
	"3d8b4393649f80bdae85c19ae2c985c45ec9ac54f74ab04a18816e30ead63c21": {build2024, "April 2024 Vulkan", build2024, binary2024, true},
	"1bddecaa4e86850412f96e3117978bce1ec7bb701ae1b5a33120c21e4421e51e": {build2024, "April 2024 OpenGL", build2024, binary2024, false},
	// 2018, Steam build 2642713, as Steam ships it (SteamStub-wrapped).
	"139763e94f1a75b5310179f9eeeb8a949a1f53c49acbc722fcfc5dfe7bb6d323": {build2018, "2018 Vulkan", "2642713", binary2018, true},
	"d46ca967824da6ffc40f1ce0b6d7f4afa868fe12b795124d3e31a1bdb646a8e5": {build2018, "2018 OpenGL", "2642713", binary2018, false},
}

// gameIdentity is the game identity every Snapmap+ report carries, formatted exactly as the
// backend does (sh_host_game_identity in src/backend/host_image.c):
// "DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)".
func gameIdentity(b doomBuild, sum string) string {
	renderer := "OpenGL"
	if b.vulkan {
		renderer = "Vulkan"
	}
	hash8 := strings.ToLower(sum)
	if len(hash8) > 8 {
		hash8 = hash8[:8]
	}
	return fmt.Sprintf("DOOM 2016 build %s (%s, %s, %s)", b.steam, b.binary, renderer, hash8)
}

// describeDoomGames returns one line per DOOM executable in dir: its game identity, or why it
// has none (unrecognized executable, unreadable file). Nothing when no executable is present.
func describeDoomGames(dir string) []string {
	var lines []string
	for _, exe := range doomExes {
		path := filepath.Join(dir, exe)
		st, err := os.Stat(path)
		if err != nil || st.IsDir() {
			continue
		}
		sum, err := fileSHA256(path)
		if err != nil {
			lines = append(lines, fmt.Sprintf("%s: unreadable (%v)", exe, err))
			continue
		}
		if b, ok := knownDoomBuilds[strings.ToLower(sum)]; ok {
			lines = append(lines, fmt.Sprintf("%s: %s", exe, gameIdentity(b, sum)))
		} else {
			lines = append(lines, fmt.Sprintf("%s: unrecognized executable (SHA-256 %s)", exe, strings.ToLower(sum)[:8]))
		}
	}
	return lines
}

// detectDoomBuild hashes every DOOM executable present in dir and returns the build they share.
// It refuses an unrecognized executable, and two executables from different builds.
func detectDoomBuild(dir string) (doomBuild, error) {
	var found doomBuild
	var foundExe string
	for _, exe := range doomExes {
		path := filepath.Join(dir, exe)
		st, err := os.Stat(path)
		if err != nil || st.IsDir() {
			continue
		}
		sum, err := fileSHA256(path)
		if err != nil {
			return doomBuild{}, fmt.Errorf("couldn't read %s to identify your DOOM version (%v)", exe, err)
		}
		b, ok := knownDoomBuilds[strings.ToLower(sum)]
		if !ok {
			return doomBuild{}, fmt.Errorf("%s is not a DOOM 2016 executable Snapmap+ recognizes (SHA-256 %s).\n"+
				"%s.\nIf a mod loader or patch changed the executable, restore the original (Steam > DOOM > Properties >\n"+
				"Installed Files > Verify integrity of game files) and run this again. Nothing was changed",
				exe, sum, supportedMsg)
		}
		if foundExe != "" && b.build != found.build {
			return doomBuild{}, fmt.Errorf("%s is the %s build but %s is the %s build -- the two executables must "+
				"come from the same DOOM update (verify the game files in Steam). %s. Nothing was changed",
				foundExe, found.label, exe, b.label, supportedMsg)
		}
		if foundExe == "" {
			found, foundExe = b, exe
		}
	}
	if foundExe == "" {
		return doomBuild{}, fmt.Errorf("no DOOM 2016 executable (%s) in %q", strings.Join(doomExes, " or "), dir)
	}
	return found, nil
}

// routeRelease decides which release an install takes for the detected build. The supported build
// takes whatever the flags ask for (latest by default, --release, or a --local build); the 2018
// build takes legacyReleaseTag and nothing else.
func routeRelease(f flags, b doomBuild) (flags, error) {
	if b.build == build2024 {
		if f.release == legacyReleaseTag {
			return f, fmt.Errorf("%s is the final release for the 2018 build; your DOOM is the %s. Nothing was changed",
				legacyReleaseTag, label2024)
		}
		return f, nil
	}
	if f.local != "" {
		return f, fmt.Errorf("your DOOM is the %s; a local Snapmap+ build targets the %s. Nothing was changed",
			label2018, label2024)
	}
	if f.release != "" && f.release != legacyReleaseTag {
		return f, fmt.Errorf("your DOOM is the %s, which only Snapmap+ %s supports; %s targets the %s. Nothing was changed",
			label2018, legacyReleaseTag, f.release, label2024)
	}
	fmt.Printf("Your DOOM is the %s. Installing Snapmap+ %s, the final release for it\n"+
		"(newer Snapmap+ releases target the %s).\n", label2018, legacyReleaseTag, label2024)
	f.release = legacyReleaseTag
	f.beta = false
	return f, nil
}

// checkBundleForBuild refuses a bundle that cannot serve the detected build: the supported build
// needs the XINPUT1_4.dll backend and its frontend at currentUIDll. The 2018 build is installed by
// the 2018 release's own installer.
func checkBundleForBuild(files []manifestEntry, b doomBuild) error {
	if b.build != build2024 {
		return nil
	}
	for _, need := range []string{currentProxy, currentUIDll} {
		found := false
		for _, e := range files {
			if strings.EqualFold(filepath.Clean(e.rel), need) {
				found = true
			}
		}
		if !found {
			return fmt.Errorf("this Snapmap+ bundle has no %s, so it does not support the %s you have installed. Nothing was changed",
				need, label2024)
		}
	}
	return nil
}
