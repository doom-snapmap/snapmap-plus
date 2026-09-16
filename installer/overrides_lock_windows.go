//go:build windows

package main

import (
	"crypto/sha256"
	"fmt"
	"strings"
	"syscall"
	"unsafe"
)

func lockOverrideMigration(root string) (func(), error) {
	sum := sha256.Sum256([]byte(strings.ToLower(root)))
	name, err := syscall.UTF16PtrFromString(fmt.Sprintf(`Local\SnapmapPlusOverrideMigration-%x`, sum))
	if err != nil {
		return nil, err
	}
	kernel := syscall.NewLazyDLL("kernel32.dll")
	handle, _, err := kernel.NewProc("CreateMutexW").Call(0, 1, uintptr(unsafe.Pointer(name)))
	if handle == 0 {
		return nil, fmt.Errorf("create migration lock: %w", err)
	}
	close := func() { kernel.NewProc("ReleaseMutex").Call(handle); syscall.CloseHandle(syscall.Handle(handle)) }
	if err == syscall.ERROR_ALREADY_EXISTS {
		syscall.CloseHandle(syscall.Handle(handle))
		return nil, fmt.Errorf("another override conversion is running")
	}
	return close, nil
}
