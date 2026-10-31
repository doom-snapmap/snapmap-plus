// snapmap-plus-sim is the installer simulation harness's helper (installer/sim/run.ps1 drives it). It
// never ships: it serves a fake GitHub Releases API on a loopback port, packs release assets from a
// dist/ folder, lays out the DOOM and data folders earlier releases left, and checks the layout
// contract after every step.
//
//	snapmap-plus-sim release --dist <dist> --exe <snapmap-plus.exe> --out <root>\<tag>
//	snapmap-plus-sim serve   --root <root> --url-file <file>
//	snapmap-plus-sim seed    <snaphak|beta19|beta20> --doom <dir> --data <dir> --profile <dir> [--fixtures <dir>]
//	snapmap-plus-sim check   --stage <name> --doom <dir> --data <dir> --phase <installed|uninstalled>
//	                         [--expect <rel>]... [--keep <rel>]...
package main

import (
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: snapmap-plus-sim <release|serve|seed|check> ...")
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "release":
		err = cmdRelease(os.Args[2:])
	case "serve":
		err = cmdServe(os.Args[2:])
	case "seed":
		err = cmdSeed(os.Args[2:])
	case "check":
		var ok bool
		ok, err = cmdCheck(os.Args[2:], os.Stdout)
		if err == nil && !ok {
			os.Exit(1)
		}
	default:
		err = fmt.Errorf("unknown command %q", os.Args[1])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "snapmap-plus-sim:", err)
		os.Exit(2)
	}
}

// opts is a tiny "--key value" parser; repeated keys accumulate.
type opts map[string][]string

func parseOpts(args []string) (opts, []string) {
	o := opts{}
	var rest []string
	for i := 0; i < len(args); i++ {
		if len(args[i]) > 2 && args[i][:2] == "--" && i+1 < len(args) {
			o[args[i][2:]] = append(o[args[i][2:]], args[i+1])
			i++
			continue
		}
		rest = append(rest, args[i])
	}
	return o, rest
}

func (o opts) one(key string) string {
	if v := o[key]; len(v) > 0 {
		return v[len(v)-1]
	}
	return ""
}

func (o opts) need(keys ...string) error {
	for _, k := range keys {
		if o.one(k) == "" {
			return fmt.Errorf("--%s is required", k)
		}
	}
	return nil
}
