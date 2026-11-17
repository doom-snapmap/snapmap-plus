#version 450 core
// lit.frag -- modprog_test's stand-in for a lit COMPUTE_SHADING fragment program: the uniform blocks, textures,
// buffers and lighting inputs the multilayer splice reads, written the way the engine's stage text spells them.
// Compiled into tests/modprog_fixture.c by tools/gen_programs.js.
layout( binding = 5 ) uniform freqHigh_fragmentUniforms_ubo {
	vec4 usevirtualmapping;
	vec4 bloommaskscale;
	vec4 colorscale;
	vec4 color;
	vec4 bloomcolor;
}
freqHigh_fragmentUniforms ;
layout( binding = 6 ) uniform freqLow_fragmentUniforms_ubo {
	vec4 globalvieworigin;
	vec4 time;
}
freqLow_fragmentUniforms ;
layout( binding = 7 ) uniform sampler2D samp_vmtrphysicalpagesmaparray0;
layout( binding = 8 ) uniform sampler2D samp_woundtexture;
layout( binding = 9 ) uniform sampler2D samp_magnificationclouds;
struct clusternumlights_t {
	int offset ;
	int numItems ;
};
layout( binding = 10, std430 ) buffer clusternumlights_b {
	restrict readonly clusternumlights_t clusternumlights[ ];
};
layout( location = 2 ) in vec4 vofi_TexCoord0;
layout( location = 0 ) out vec4 out_FragColor0;
struct lightingInput_t {
	vec3 albedo;
	vec3 specular;
	float smoothness;
	float thickness;
	vec3 normal;
	vec3 lightmap;
	vec3 emissive;
	vec4 texCoords;
};
void main() {
	{
		lightingInput_t inputs = lightingInput_t( vec3(0), vec3(0), 0, 0, vec3(0), vec3(0), vec3(0), vec4(0) );
		inputs.texCoords = vofi_TexCoord0;
		vec4 sampleNormal = texture( samp_vmtrphysicalpagesmaparray0, vofi_TexCoord0.xy );
		vec4 sampleLightmap = texture( samp_vmtrphysicalpagesmaparray0, vofi_TexCoord0.zw );
		inputs.albedo = sampleNormal.xyz;
		inputs.specular = sampleLightmap.xyz;
		inputs.lightmap = sampleLightmap.xyz;
		inputs.smoothness = sampleLightmap.w;
		inputs.normal = sampleNormal.wyz;
		{
			vec4 woundMap = texture( samp_woundtexture, vofi_TexCoord0.xy );
			inputs.albedo *= woundMap.xyz * texture( samp_magnificationclouds, vofi_TexCoord0.xy ).x;
		}
		int n = clusternumlights[ 0 ].numItems;
		out_FragColor0 = vec4( inputs.albedo + inputs.specular + inputs.normal + inputs.emissive + inputs.lightmap,
		                       inputs.smoothness + inputs.thickness + float( n ) ) * freqHigh_fragmentUniforms.color +
		                 freqLow_fragmentUniforms.time;
	}
}
