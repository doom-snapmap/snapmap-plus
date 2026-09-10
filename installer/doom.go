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

const doomAppID = "379720"

// Both renderers are installed together. Check both executable names because
// DOOM can relaunch into the other renderer.
var doomExes = []string{"DOOMx64vk.exe", "DOOMx64.exe"}

// resolveDoom returns the DOOM install dir: an explicit --doom (verified), else Steam auto-detect.
func resolveDoom(explicit string) (string, error) {
	if explicit != "" {
		if hasDoomExe(explicit) {
			return explicit, nil
		}
		return "", fmt.Errorf("no DOOM 2016 executable in %q -- point --doom at your DOOM 2016 folder (the one containing %s)", explicit, strings.Join(doomExes, " and "))
	}
	dir, err := detectDoomViaSteam()
	if err != nil {
		return "", fmt.Errorf("couldn't find your DOOM 2016 install automatically -- pass --doom <your DOOM 2016 folder, the one containing %s> (usually ...\\steamapps\\common\\DOOM)", strings.Join(doomExes, " and "))
	}
	return dir, nil
}

// hasDoomExe: either shipped executable is enough to call this a DOOM 2016 folder.
func hasDoomExe(dir string) bool {
	for _, exe := range doomExes {
		if st, err := os.Stat(filepath.Join(dir, exe)); err == nil && !st.IsDir() {
			return true
		}
	}
	return false
}

// doomIsRunning checks both process names to avoid replacing loaded DLLs.
// Detection is best-effort: tasklist failure does not block installation.
func doomIsRunning() bool {
	for _, exe := range doomExes {
		out, err := exec.Command("tasklist", "/FI", "IMAGENAME eq "+exe, "/NH").Output()
		if err != nil {
			continue
		}
		if strings.Contains(string(out), exe) {
			return true
		}
	}
	return false
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
