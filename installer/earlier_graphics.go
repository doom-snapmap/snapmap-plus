package main

import (
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
)

var earlierParticleProgram = regexp.MustCompile(`(?im)^([ \t]*stageprogram[ \t]+"?)snapmapplus/eternal/(uberparticles)([ \t\r]*$|"|[ \t]+//)`)
var earlierParticleKey = regexp.MustCompile(`(?im)^[ \t]*eternalparticle(?:fade|color2?|emissivemap)?[ \t]+`)

func earlierVector(text string) ([4]float64, error) {
	var out [4]float64
	fields := strings.FieldsFunc(text, func(r rune) bool { return strings.ContainsRune("{}, \t", r) })
	if len(fields) < 1 || len(fields) > 4 {
		return out, fmt.Errorf("expected a vector of up to four numbers")
	}
	for i, s := range fields {
		n, err := strconv.ParseFloat(s, 64)
		if err != nil || math.IsInf(n, 0) || math.IsNaN(n) {
			return out, fmt.Errorf("invalid component %q", s)
		}
		out[i] = n
	}
	if len(fields) == 1 && !strings.HasPrefix(text, "{") {
		out = [4]float64{out[0], out[0], out[0], out[0]}
	}
	return out, nil
}

// Turn retired material parameters into the same author look as a current emit.
func upgradeEarlierMaterial(original []byte) ([]byte, error) {
	if !earlierLookKey.Match(original) && !earlierCompactLook.Match(original) && !earlierParticleProgram.Match(original) && !earlierParticleKey.Match(original) && !earlierLayerKey.Match(original) {
		return original, nil
	}
	return migrateEarlierLook(string(original))
}
