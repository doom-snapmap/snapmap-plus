package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

var prototypeBinding = regexp.MustCompile(`^smpbind\.([0-9]+)\.([0-9]+)\.([0-9a-f]{16})$`)
var prototypeReference = regexp.MustCompile(`^smpref\.delivery\.[0-9a-f]{64}\.([0-9a-f]{16})$`)

type unavailablePrototype string

func (why unavailablePrototype) Error() string { return string(why) }

func prototypeBody(encoded, digest string) []byte {
	body, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return nil
	}
	sum := sha256.Sum256(body)
	if hex.EncodeToString(sum[:8]) != digest {
		return nil
	}
	return body
}

// The prototype saved external package references and private placement aliases.
// Neither establishes a current mod pin. Refuse the dependent map before staging any files.
func earlierPrototypeDependency(src []byte, root earlierObject) error {
	raw := root["variables"]
	if !bytes.Contains(raw, []byte("smpref.")) && !bytes.Contains(raw, []byte("smpbind.")) {
		return nil
	}
	var vars struct{ String []json.RawMessage }
	if json.Unmarshal(raw, &vars) != nil {
		return nil
	}
	var parts []string
	var digest string
	hasBinding := false
	validSet := true
	for _, raw := range vars.String {
		var row earlierVariable
		if json.Unmarshal(raw, &row) != nil || row.Info.Type != "snapVarInfo_t" {
			continue
		}
		if ref := prototypeReference.FindStringSubmatch(row.Info.Name); ref != nil {
			var spec struct {
				ID, Name, SHA256 string
				Packed           int64
			}
			if json.Unmarshal(prototypeBody(row.InitialValue, ref[1]), &spec) == nil && earlierID.MatchString(spec.ID) &&
				spec.Name != "" && spec.Packed > 0 && pinHash(spec.SHA256) {
				return unavailablePrototype(fmt.Sprintf("the earlier external package %s (%s) has no supported build pin", spec.ID, spec.SHA256))
			}
		}
		bind := prototypeBinding.FindStringSubmatch(row.Info.Name)
		if bind == nil {
			continue
		}
		hasBinding = true
		i, ix := strconv.Atoi(bind[1])
		n, nx := strconv.Atoi(bind[2])
		if ix != nil || nx != nil || n > len(vars.String) || i >= n || row.InitialValue == "" {
			validSet = false
			continue
		}
		if parts == nil {
			parts, digest = make([]string, n), bind[3]
		}
		if n != len(parts) || digest != bind[3] || parts[i] != "" {
			validSet = false
			continue
		}
		parts[i] = row.InitialValue
	}
	if !hasBinding {
		return nil
	}
	var body []byte
	if validSet {
		for _, part := range parts {
			if part == "" {
				validSet = false
				break
			}
		}
	}
	if validSet {
		body = prototypeBody(strings.Join(parts, ""), digest)
	}
	_, names := earlierEntityPlacements(src, nil, true)
	for _, name := range names {
		hash, private := strings.CutPrefix(name, "smp/")
		if !private || !pinHash(hash) {
			continue
		}
		field := binary.LittleEndian.AppendUint32(nil, uint32(len(name)))
		if len(body) >= 8 && bytes.HasPrefix(body, []byte("SMPBIND")) && body[7] <= 2 && bytes.Contains(body, append(field, name...)) {
			return unavailablePrototype(fmt.Sprintf("the placed entity %s uses an unsupported earlier package binding", name))
		}
		return unavailablePrototype(fmt.Sprintf("cannot establish the earlier package binding for the placed entity %s", name))
	}
	return nil
}
