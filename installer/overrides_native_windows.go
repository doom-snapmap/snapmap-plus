package main

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"syscall"
	"unsafe"
)

//go:embed native/migration.dll
var migrationLibrary []byte

// The library contains only the shared planner, not the game bootstrap. Load
// our embedded bytes from a private temporary directory with system-only
// dependency lookup, release every allocation through its owning C runtime,
// then unload and remove the temporary files.
func runNativeMigration(request any) ([]byte, error) {
	input, err := json.Marshal(request)
	if err != nil {
		return nil, err
	}
	dir, err := os.MkdirTemp("", "snapmap-migration-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "migration.dll")
	if err := os.WriteFile(path, migrationLibrary, 0600); err != nil {
		return nil, err
	}
	wide, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	load := syscall.NewLazyDLL("kernel32.dll").NewProc("LoadLibraryExW")
	handle, _, callErr := load.Call(uintptr(unsafe.Pointer(wide)), 0, 0x1100)
	if handle == 0 {
		return nil, fmt.Errorf("load shared package converter: %w", callErr)
	}
	defer syscall.FreeLibrary(syscall.Handle(handle))
	run, err := syscall.GetProcAddress(syscall.Handle(handle), "sh_package_migration_run")
	if err != nil {
		return nil, err
	}
	release, err := syscall.GetProcAddress(syscall.Handle(handle), "sh_package_migration_release")
	if err != nil {
		return nil, err
	}
	var length uintptr
	var diagnostic [2048]byte
	result, _, _ := syscall.SyscallN(run, uintptr(unsafe.Pointer(&input[0])), uintptr(len(input)),
		uintptr(unsafe.Pointer(&length)), uintptr(unsafe.Pointer(&diagnostic[0])), uintptr(len(diagnostic)))
	runtime.KeepAlive(input)
	if result == 0 {
		n := bytes.IndexByte(diagnostic[:], 0)
		if n < 0 {
			n = len(diagnostic)
		}
		return nil, rejectOverride("%s", string(diagnostic[:n]))
	}
	defer syscall.SyscallN(release, result)
	if length > uintptr(^uint(0)>>1) {
		return nil, fmt.Errorf("converted package plan is too large")
	}
	out := make([]byte, int(length))
	if len(out) != 0 {
		copyBytes := syscall.NewLazyDLL("kernel32.dll").NewProc("RtlMoveMemory")
		copyBytes.Call(uintptr(unsafe.Pointer(&out[0])), result, length)
		runtime.KeepAlive(out)
	}
	return out, nil
}
