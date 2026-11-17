#version 450 core
// particle.frag -- modprog_test's stand-in for the particle program's fragment stage: the uniform blocks, textures,
// stage inputs and colour output the particle splice reads, written the way the engine's stage text spells them.
// Compiled into tests/modprog_fixture.c by tools/gen_programs.js.
layout( binding = 2 ) uniform freqHigh_fragmentUniforms_ubo {
	vec4 particleuberparms1;
	vec4 renderpositiontoviewtexture;
	vec4 emissive;
}
freqHigh_fragmentUniforms ;
layout( binding = 3 ) uniform freqLow_fragmentUniforms_ubo {
	vec4 particlemult;
	vec4 particleatlasparms;
	vec4 foglightinscattercolor;
	vec4 foglightinscatterintensity;
	vec4 foglightoppositecolor;
	vec4 fogcolor;
	vec4 transparencylightmult;
}
freqLow_fragmentUniforms ;
layout( binding = 4 ) uniform sampler2D samp_transmap;
layout( binding = 5 ) uniform sampler2D samp_viewdepthmap;
layout( binding = 6 ) uniform sampler2D samp_particleslightatlasmap;

layout( location = 2 ) in vec4 vofi_TexCoord0;
layout( location = 3 ) in vec4 vofi_TexCoord1;
layout( location = 0 ) in vec4 vofi_Color;
layout( location = 4 ) in vec4 vofi_TexCoord2;
layout( location = 6 ) in vec4 vofi_TexCoord4;
layout( location = 8 ) in vec4 vofi_TexCoord6;
in vec4 gl_FragCoord;

layout( location = 0 ) out vec4 out_FragColor0;

void main() {
	{
		vec4 t = texture( samp_transmap, vofi_TexCoord0.xy );
		if ( t.g < 1e-6 ) { discard; }
		float d = textureLod( samp_viewdepthmap, gl_FragCoord.xy * freqHigh_fragmentUniforms.renderpositiontoviewtexture.zw, 0.0 ).x;
		vec3 l = texture( samp_particleslightatlasmap, vofi_TexCoord4.xy ).xyz * freqLow_fragmentUniforms.particleatlasparms.x;
		vec3 c = t.rrr * vofi_Color.rgb * vofi_TexCoord1.x * freqHigh_fragmentUniforms.emissive.x + l * d;
		c = mix( freqLow_fragmentUniforms.fogcolor.xyz * freqLow_fragmentUniforms.transparencylightmult.x, c, vofi_TexCoord6.x );
		c += freqLow_fragmentUniforms.foglightinscattercolor.xyz * freqLow_fragmentUniforms.foglightinscatterintensity.x +
		     freqLow_fragmentUniforms.foglightoppositecolor.xyz + vofi_TexCoord2.xyz * freqLow_fragmentUniforms.particlemult.x;
		out_FragColor0.rgb = c * freqHigh_fragmentUniforms.particleuberparms1.x;
		out_FragColor0.a = t.g * vofi_Color.w;
		out_FragColor0.rgb *= out_FragColor0.a;
	}
}
