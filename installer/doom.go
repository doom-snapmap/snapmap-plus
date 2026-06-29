package main

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
)

const (
	doomAppID = "379720"
	doomExe   = "DOOMx64vk.exe"
)

// resolveDoom returns the DOOM install dir: an explicit --doom (verified), else Steam auto-detect.
func resolveDoom(explicit string) (string, error) {
	if explicit != "" {
		if hasDoomExe(explicit) {
			return explicit, nil
		}
		return "", fmt.Errorf("no %s in %q -- point --doom at your DOOM 2016 folder (the one containing %s)", doomExe, explicit, doomExe)
	}
	dir, err := detectDoomViaSteam()
	if err != nil {
		return "", fmt.Errorf("couldn't find your DOOM 2016 install automatically -- pass --doom <the folder containing %s> (usually ...\\steamapps\\common\\DOOM)", doomExe)
	}
	return dir, nil
}

func hasDoomExe(dir string) bool {
	st, err := os.Stat(filepath.Join(dir, doomExe))
	return err == nil && !st.IsDir()
}

// doomIsRunning reports whether DOOM 2016 (DOOMx64vk.exe) is currently running, so we can say "close DOOM
// first" instead of surfacing a raw file-lock error -- Windows won't let us replace a DLL the running game
// has loaded. Best-effort: if we can't tell (e.g. tasklist unavailable), we don't block.
func doomIsRunning() bool {
	out, err := exec.Command("tasklist", "/FI", "IMAGENAME eq "+doomExe, "/NH").Output()
	if err != nil {
		return false
	}
	return strings.Contains(string(out), doomExe)
}

// detectDoomViaSteam: SteamPath (registry) -> every library in libraryfolders.vdf -> the one holding appid 379720.
func detectDoomViaSteam() (string, error) {
	steam, err := steamPath()
	if err != nil {
		return "", err
	}
	for _, lib := range steamLibraries(steam) {
		// prefer the library whose appmanifest declares DOOM, but fall back to a present common\DOOM
		manifest := filepath.Join(lib, "steamapps", "appmanifest_"+doomAppID+".acf")
		cand := filepath.Join(lib, "steamapps", "common", "DOOM")
		if _, err := os.Stat(manifest); err == nil && hasDoomExe(cand) {
			return cand, nil
		}
		if hasDoomExe(cand) {
			return cand, nil
		}
	}
	return "", fmt.Errorf("DOOM (appid %s) not found in any Steam library", doomAppID)
}

// steamPath reads HKCU\Software\Valve\Steam\SteamPath via `reg query` (avoids an x/sys dependency).
func steamPath() (string, error) {
	out, err := exec.Command("reg", "query", `HKCU\Software\Valve\Steam`, "/v", "SteamPath").Output()
	if err != nil {
		return "", fmt.Errorf("Steam registry key not readable: %w", err)
	}
	// a value line looks like:  "    SteamPath    REG_SZ    c:/program files (x86)/steam"
	for _, line := range strings.Split(string(out), "\n") {
		if !strings.Contains(line, "SteamPath") {
			continue
		}
		if i := strings.Index(line, "REG_SZ"); i >= 0 {
			p := strings.TrimSpace(line[i+len("REG_SZ"):])
			if p != "" {
				return filepath.FromSlash(p), nil
			}
		}
	}
	return "", fmt.Errorf("SteamPath value not present")
}

// steamLibraries returns the Steam install root plus every extra library from libraryfolders.vdf.
func steamLibraries(steam string) []string {
	libs := []string{steam}
	f, err := os.Open(filepath.Join(steam, "steamapps", "libraryfolders.vdf"))
	if err != nil {
		return libs
	}
	defer f.Close()
	// lines like:   "path"   "D:\\SteamLibrary"
	re := regexp.MustCompile(`"path"\s+"([^"]+)"`)
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if m := re.FindStringSubmatch(sc.Text()); m != nil {
			p := strings.ReplaceAll(m[1], `\\`, `\`) // VDF escapes backslashes
			libs = append(libs, filepath.Clean(filepath.FromSlash(p)))
		}
	}
	return libs
}
