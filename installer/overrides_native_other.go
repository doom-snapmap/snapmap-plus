//go:build !windows

package main

import "errors"

func runNativeMigration(request any) ([]byte, error) {
	return nil, errors.New("package conversion requires the Windows installer")
}
