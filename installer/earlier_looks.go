package main

import (
	"fmt"
	"math"
	"path"
	"regexp"
	"strconv"
	"strings"
)

var earlierLayerKey = regexp.MustCompile(`(?im)^[ \t]*(?:layerstilingfactor|multiblend(?:layerscale|heightmapscale|softness|albedoscale|normalscale|specularscale|smoothnessscale|emissivescale)|blendheightmapscale|blendlayer[0-4](?:tiling|emissive|specular)|blendlayer0heightmap|multilayerember(?:tiling|noise|hot)?)[ \t]+`)

type earlierLook struct {
	lines, keys, fields []string
	values              map[string]string
	drop                map[string]bool
	first               int
	err                 error
}

// Look keys are the material's outer fields. Nested blocks and comments can contain the same words.
func materialLine(line string, depth *int, block *bool) string {
	var code strings.Builder
	quoted := false
	for i := 0; i < len(line); i++ {
		c := line[i]
		if *block {
			if strings.HasPrefix(line[i:], "*/") {
				*block = false
				i++
			}
			continue
		}
		if !quoted {
			if strings.HasPrefix(line[i:], "//") {
				break
			}
			if strings.HasPrefix(line[i:], "/*") {
				*block = true
				code.WriteByte(' ')
				i++
				continue
			}
			if c == '{' {
				*depth++
			} else if c == '}' {
				*depth--
			}
		}
		if c == '"' {
			quoted = !quoted
		}
		code.WriteByte(c)
	}
	return strings.TrimSpace(code.String())
}

func migrateEarlierLook(text string) ([]byte, error) {
	eol := "\n"
	if strings.Contains(text, "\r\n") {
		eol = "\r\n"
	}
	m := earlierLook{lines: strings.Split(text, eol), values: map[string]string{}, drop: map[string]bool{}, first: -1}
	depth, block := 0, false
	var particle, layered bool
	for i, line := range m.lines {
		outer := depth == 1
		code := materialLine(line, &depth, &block)
		words := strings.Fields(code)
		key := ""
		if outer && len(words) >= 2 && earlierKey.MatchString(words[0]) {
			key = strings.ToLower(words[0])
			m.values[key] = strings.TrimSpace(code[len(words[0]):])
			particle = particle || earlierParticleKey.MatchString(code)
			layered = layered || earlierLayerKey.MatchString(code)
			if key == "stageprogram" {
				m.lines[i] = earlierParticleProgram.ReplaceAllString(line, "${1}snapmapplus/particles/${2}${3}")
			}
			if m.first < 0 {
				m.first = i
			}
		}
		m.keys = append(m.keys, key)
	}
	if m.first >= 0 && m.keys[m.first] == "look" && earlierLookKey.MatchString(m.lines[m.first]) {
		m.lines[m.first] = earlierLookKey.ReplaceAllString(m.lines[m.first], "${1}type${2}")
		m.keys[m.first] = "type"
	}
	if m.first < 0 || m.keys[m.first] == "type" || (!particle && !layered) {
		return []byte(strings.Join(m.lines, eol)), nil
	}
	if particle {
		m.add("type", "particle")
		m.particle()
	} else {
		m.add("type", "layered")
		m.layered()
	}
	if m.err != nil {
		return nil, m.err
	}
	var out []string
	for i, line := range m.lines {
		if i == m.first {
			out = append(out, m.fields...)
		}
		if !m.drop[m.keys[i]] {
			out = append(out, line)
		}
	}
	return []byte(strings.Join(out, eol)), nil
}

func (m *earlierLook) take(key string) string {
	m.drop[key] = true
	return m.values[key]
}

func (m *earlierLook) vector(key string, fallback [4]float64) [4]float64 {
	v := m.take(key)
	if v == "" {
		return fallback
	}
	nums, err := earlierVector(v)
	if err != nil && m.err == nil {
		m.err = fmt.Errorf("%s: %w", key, err)
	}
	return nums
}

func (m *earlierLook) add(key, val string) {
	m.fields = append(m.fields, key+"\t"+val)
}

func (m *earlierLook) numbers(key string, values ...float64) {
	text := make([]string, len(values))
	for i, v := range values {
		if v == 0 {
			v = 0
		}
		text[i] = strconv.FormatFloat(v, 'g', 9, 64)
	}
	if len(text) == 1 {
		m.add(key, text[0])
	} else {
		m.add(key, "{ "+strings.Join(text, ", ")+" }")
	}
}

func (m *earlierLook) image(key, value string) {
	if value != "" {
		m.add(key, strconv.Quote(strings.Trim(value, "\"")))
	}
}

func (m *earlierLook) changed(key string, values, defaults []float64) {
	for i, v := range values {
		if float32(v) != float32(defaults[i]) {
			m.numbers(key, values...)
			return
		}
	}
}

type earlierTexture struct {
	name     string
	constant bool
	rgba     [4]float64
}

func (m *earlierLook) texture(key string) earlierTexture {
	t := earlierTexture{name: strings.Trim(m.take(key), "\"")}
	base := strings.ToLower(path.Base(t.name))
	switch base {
	case "_black":
		t.rgba = [4]float64{0, 0, 0, 1}
	case "_white":
		t.rgba = [4]float64{1, 1, 1, 1}
	case "_flat":
		t.rgba = [4]float64{.5, .5, 1, 1}
	default:
		bits := strings.Split(strings.TrimPrefix(strings.TrimSuffix(base, ".tga"), "const_"), "_")
		if !strings.HasPrefix(base, "const_") || len(bits) != 4 {
			return t
		}
		for i, s := range bits {
			n, err := strconv.ParseUint(s, 16, 16)
			if err != nil || len(s) != 4 {
				return t
			}
			exp, mant := int(n>>10)&31, int(n&1023)
			switch exp {
			case 0:
				t.rgba[i] = math.Ldexp(float64(mant), -24)
			case 31:
				t.rgba[i] = 65504
			default:
				t.rgba[i] = math.Ldexp(float64(1024+mant), exp-25)
			}
			if n&0x8000 != 0 {
				t.rgba[i] = -t.rgba[i]
			}
		}
	}
	t.constant = true
	return t
}

func (t earlierTexture) matches(v [4]float64) bool {
	return t.name == "" || (t.constant && t.rgba == v)
}

func (m *earlierLook) colour(key string, t earlierTexture, n int) {
	if t.constant {
		m.numbers(key, t.rgba[:n]...)
	} else {
		m.image(key, t.name)
	}
}

func (m *earlierLook) particle() {
	ones := [4]float64{1, 1, 1, 1}
	p := m.vector("eternalparticle", [4]float64{})
	fade := m.vector("eternalparticlefade", [4]float64{})
	a, b := m.vector("eternalparticlecolor", ones), m.vector("eternalparticlecolor2", ones)
	for _, v := range []*[4]float64{&p, &fade, &a, &b} {
		for i, x := range v {
			v[i] = math.RoundToEven(x*1e6) / 1e6
		}
	}
	if p[0] == 1 || p[0] == 4 {
		lighting := "lit"
		if p[0] == 4 {
			lighting = "explosion"
		}
		m.add("lighting", lighting)
		m.take("particlelightatlasprogram")
	}
	m.changed("glow", p[1:2], ones[:1])
	m.image("glowmask", m.take("eternalparticleemissivemap"))
	m.changed("glowmaskstrength", p[2:3], []float64{0})
	m.changed("depthoffset", p[3:], []float64{0})
	m.changed("litscale", b[3:], ones[:1])
	if p[0] == 4 {
		m.changed("gradient", []float64{a[0], a[1], a[2], b[0], b[1], b[2]}, []float64{1, 1, 1, 1, 1, 1})
	} else {
		m.changed("glowmaskcolour", a[:3], ones[:3])
	}
	if fade[0] > fade[1] {
		m.numbers("nearfade", fade[:2]...)
	}
	if fade[2] > .001 {
		m.numbers("softdistance", fade[2])
	}
}

func (m *earlierLook) layered() {
	zero := [4]float64{}
	ones := [4]float64{1, 1, 1, 1}
	normal := [4]float64{.5, .5, 1, 1}
	height := [4]float64{.5, .5, .5, 1}
	tiling := [4]float64{1, 1, 0, 0}
	repeat := m.vector("layerstilingfactor", ones)
	coverage := m.vector("multiblendlayerscale", ones)
	heightBlend := m.vector("multiblendheightmapscale", ones)
	softness := m.vector("multiblendsoftness", [4]float64{.25, .25, .25, .25})
	var weights [5][4]float64
	for i, name := range []string{"albedo", "normal", "specular", "smoothness", "emissive"} {
		weights[i] = m.vector("multiblend"+name+"scale", ones)
	}
	if strings.Contains(strings.ToLower(m.take("ambientprogram")), "wounds") {
		m.add("wounds", "yes")
	}
	m.take("layerparms")
	mask := m.texture("layerblendmask")
	if !mask.matches(zero) {
		m.image("layermask", mask.name)
	}
	extra := [2]earlierTexture{m.texture("multilayerextramap0"), m.texture("multilayerextramap1")}
	slot := func(n float64) string {
		i := int(n)
		if i == 1 || i == 2 {
			return extra[i-1].name
		}
		return ""
	}
	for i := 0; i < 4; i++ {
		name := "layer" + strconv.Itoa(i+1)
		old := "blend" + name
		albedo := m.texture(old + "albedomap")
		n := m.texture(old + "normalmap")
		s := m.texture(old + "smoothnessmap")
		h := m.texture(old + "heightmap")
		spec := m.vector(old+"specular", zero)
		glow := m.vector(old+"emissive", zero)
		uv := m.vector(old+"tiling", tiling)
		if coverage[i] == 0 && albedo.matches(ones) && n.matches(normal) && s.matches(ones) && h.matches(height) && spec == zero && glow == zero {
			continue
		}
		if albedo.name == "" {
			m.numbers(name, ones[:3]...)
		} else {
			m.colour(name, albedo, 3)
		}
		if !n.matches(normal) {
			m.image(name+"normal", n.name)
		}
		if s.name == "" {
			m.numbers(name+"smoothness", 1)
		} else if !s.constant || float32(s.rgba[0]) != .5 {
			m.colour(name+"smoothness", s, 1)
		}
		if h.name != "" && (!h.constant || float32(h.rgba[0]) != .5) {
			m.colour(name+"height", h, 1)
		}
		for k, v := range [2][4]float64{spec, glow} {
			key := name + []string{"specular", "glow"}[k]
			if tex := slot(v[3]); tex != "" {
				m.image(key, tex)
			} else {
				m.changed(key, v[:3], zero[:3])
			}
		}
		m.changed(name+"repeat", repeat[i:i+1], ones[:1])
		m.changed(name+"tiling", uv[:2], ones[:2])
		m.changed(name+"scroll", uv[2:], zero[:2])
		m.changed(name+"coverage", coverage[i:i+1], ones[:1])
		m.changed(name+"heightblend", heightBlend[i:i+1], ones[:1])
		m.changed(name+"softness", softness[i:i+1], []float64{.25})
		m.changed(name+"weights", []float64{weights[0][i], weights[1][i], weights[2][i], weights[3][i], weights[4][i]}, []float64{1, 1, 1, 1, 1})
	}
	base := m.texture("blendlayer0heightmap")
	if base.name != "" && (!base.constant || float32(base.rgba[0]) != .5) {
		m.colour("baseheight", base, 1)
	}
	blend := m.vector("blendheightmapscale", [4]float64{1})
	uv := m.vector("blendlayer0tiling", tiling)
	glow := m.vector("blendlayer0emissive", [4]float64{1, 1, 1, 0})
	m.changed("baseheightblend", blend[:1], ones[:1])
	m.changed("basetiling", uv[:2], ones[:2])
	m.changed("basescroll", uv[2:], zero[:2])
	m.changed("baseglow", glow[:3], ones[:3])
	burn := m.vector("multilayerember", zero)
	burnUVDefault := [4]float64{4, 4}
	burnNoiseDefault := [4]float64{1, 4, .7, .4}
	burnHeatDefault := [4]float64{1, .07, 0}
	burnUV := m.vector("multilayerembertiling", burnUVDefault)
	burnNoise := m.vector("multilayerembernoise", burnNoiseDefault)
	burnHeat := m.vector("multilayeremberhot", burnHeatDefault)
	if burn[0] == 1 {
		mat := slot(burn[2])
		if mat == "" {
			mat = extra[0].name
		}
		m.image("burn", mat)
		m.image("burnglow", slot(burn[1]))
		m.image("burnblood", slot(burn[3]))
		m.changed("burntiling", burnUV[:2], burnUVDefault[:2])
		m.changed("burnnoise", burnNoise[:], burnNoiseDefault[:])
		m.changed("burnheat", burnHeat[:3], burnHeatDefault[:3])
	}
}
