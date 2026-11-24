#version 450 core
// hair.frag -- modprog_test's stand-in for a hair program's fragment stage (hairprez): the material uniform block, the
// texture, the stage input and the colour output the hair splice reads, written the way the engine's stage text spells
// them. Compiled into tests/modprog_fixture.c by tools/gen_programs.js.
layout( binding = 3 ) uniform freqHigh_fragmentUniforms_ubo {
	vec4 hairparms;
}
freqHigh_fragmentUniforms ;
layout( binding = 4 ) uniform sampler2D samp_sparediffusemap;

layout( location = 2 ) in vec4 vofi_TexCoord0;

layout( location = 0 ) out vec4 out_FragColor0;

void main() {
	{
		float alpha = texture( samp_sparediffusemap, vofi_TexCoord0.xy ).w;
		if ( alpha - freqHigh_fragmentUniforms.hairparms.w < 0.0 ) { discard; }
		out_FragColor0 = vec4( 0.0 );
	}
}
