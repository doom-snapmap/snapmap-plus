//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var moveFileEx = syscall.NewLazyDLL("kernel32.dll").NewProc("MoveFileExW")

// Extended-length form for absolute paths, as package os uses internally.
func extendedPath(path string) string {
	path = filepath.Clean(path)
	if strings.HasPrefix(path, `\\?\`) || !filepath.IsAbs(path) {
		return path
	}
	if strings.HasPrefix(path, `\\`) {
		return `\\?\UNC\` + path[2:]
	}
	return `\\?\` + path
}

// renameNoReplace moves a file or folder on the same volume and fails if the
// destination exists. os.Rename would replace an existing file on Windows.
func renameNoReplace(from, to string) error {
	source, err := syscall.UTF16PtrFromString(extendedPath(from))
	if err != nil {
		return err
	}
	target, err := syscall.UTF16PtrFromString(extendedPath(to))
	if err != nil {
		return err
	}
	if ok, _, err := moveFileEx.Call(uintptr(unsafe.Pointer(source)), uintptr(unsafe.Pointer(target)), 0); ok == 0 {
		return &os.LinkError{Op: "rename", Old: from, New: to, Err: err}
	}
	return nil
}
