//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

func winKnownFolder(id *syscall.GUID) (string, error) {
	var path *uint16
	shell := syscall.NewLazyDLL("shell32.dll")
	hr, _, _ := shell.NewProc("SHGetKnownFolderPath").Call(uintptr(unsafe.Pointer(id)), 0x4000, 0, uintptr(unsafe.Pointer(&path)))
	if int32(hr) < 0 || path == nil {
		return "", fmt.Errorf("Windows could not locate the folder (0x%08x)", uint32(hr))
	}
	defer syscall.NewLazyDLL("ole32.dll").NewProc("CoTaskMemFree").Call(uintptr(unsafe.Pointer(path)))
	length, _, _ := syscall.NewLazyDLL("kernel32.dll").NewProc("lstrlenW").Call(uintptr(unsafe.Pointer(path)))
	return syscall.UTF16ToString(unsafe.Slice(path, length)), nil
}

func nativeSaveRoot() (string, error) {
	profileID := syscall.GUID{Data1: 0x5e6c858f, Data2: 0x0e22, Data3: 0x4760, Data4: [8]byte{0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73}}
	savedGamesID := syscall.GUID{Data1: 0x4c5c32ff, Data2: 0xbb9d, Data3: 0x43b0, Data4: [8]byte{0xb5, 0xb4, 0x2d, 0x72, 0xe5, 0x4e, 0xaa, 0xa4}}
	profile, err := winKnownFolder(&profileID)
	if err != nil {
		return "", err
	}
	var saved string
	if env := os.Getenv("USERPROFILE"); env != "" && !strings.EqualFold(filepath.Clean(env), filepath.Clean(profile)) {
		saved = filepath.Join(env, "Saved Games")
	} else {
		saved, err = winKnownFolder(&savedGamesID)
		if err != nil {
			return "", err
		}
	}
	return filepath.Join(saved, "id Software", "DOOM", "base", "savegame.user"), nil
}

func publishNativeCopy(tmp, dest string) error {
	from, err := syscall.UTF16PtrFromString(tmp)
	if err != nil {
		return err
	}
	to, err := syscall.UTF16PtrFromString(dest)
	if err != nil {
		return err
	}
	return syscall.MoveFile(from, to)
}
