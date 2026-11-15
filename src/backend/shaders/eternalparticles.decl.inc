		// eternalparticles.decl.inc -- DOOM Eternal's particle shading of the product's particle program (modprog.h) in
		// the render program language, for the OpenGL renderer, which compiles a program from its decl text. modprog.c
		// inserts this text after the base fragment stage's includes and ETERNAL_PARTICLE( result ) after its
		// PREMULTIPLY_ALPHA_BLENDED( result ). The same function, line for line, as ep_shade in eternalparticles.frag
		// (the Vulkan module): see there for DOOM Eternal's rule. The engine compiles a program's fragment text inside
		// main(), so the helpers are macros and templates, never functions.
		#define epLin( x ) ( ( x ) * ( ( x ) * ( ( x ) * 0.305306011 + 0.682171111 ) + 0.012522878 ) )
		#define epSrgb( x ) ( ( x ) <= 0.0031308 ? ( x ) * 12.92 : 1.055 * ( pow( ( x ), 1.0 / 2.4 ) - 0.0521327 ) )
		#define EP_MIN_ALPHA ( 16.0 / 255.0 )
		#define epW0( a ) ( ( 1.0 / 6.0 ) * ( ( a ) * ( ( a ) * ( -( a ) + 3.0 ) - 3.0 ) + 1.0 ) )
		#define epW1( a ) ( ( 1.0 / 6.0 ) * ( ( a ) * ( a ) * ( 3.0 * ( a ) - 6.0 ) + 4.0 ) )
		#define epW2( a ) ( ( 1.0 / 6.0 ) * ( ( a ) * ( ( a ) * ( -3.0 * ( a ) + 3.0 ) + 3.0 ) + 1.0 ) )
		#define epW3( a ) ( ( 1.0 / 6.0 ) * ( ( a ) * ( a ) * ( a ) ) )
		#template EP_BICUBIC( dst, image, uv0, res ) {
			float2 epUv = ( uv0 ) * ( res ) + 0.5;
			float2 epIuv = floor( epUv );
			float2 epFuv = frac( epUv );
			float epG0x = epW0( epFuv.x ) + epW1( epFuv.x );
			float epG1x = epW2( epFuv.x ) + epW3( epFuv.x );
			float epG0y = epW0( epFuv.y ) + epW1( epFuv.y );
			float epG1y = epW2( epFuv.y ) + epW3( epFuv.y );
			float epH0x = -1.0 + epW1( epFuv.x ) / epG0x;
			float epH1x = 1.0 + epW3( epFuv.x ) / epG1x;
			float epH0y = -1.0 + epW1( epFuv.y ) / epG0y;
			float epH1y = 1.0 + epW3( epFuv.y ) / epG1y;
			float3 epT0 = tex2Dlod( image, float4( ( float2( epIuv.x + epH0x, epIuv.y + epH0y ) - 0.5 ) / ( res ), 0.0, 0.0 ) ).xyz;
			float3 epT1 = tex2Dlod( image, float4( ( float2( epIuv.x + epH1x, epIuv.y + epH0y ) - 0.5 ) / ( res ), 0.0, 0.0 ) ).xyz;
			float3 epT2 = tex2Dlod( image, float4( ( float2( epIuv.x + epH0x, epIuv.y + epH1y ) - 0.5 ) / ( res ), 0.0, 0.0 ) ).xyz;
			float3 epT3 = tex2Dlod( image, float4( ( float2( epIuv.x + epH1x, epIuv.y + epH1y ) - 0.5 ) / ( res ), 0.0, 0.0 ) ).xyz;
			dst = epG0y * ( epG0x * epT0 + epG1x * epT1 ) + epG1y * ( epG0x * epT2 + epG1x * epT3 );
		}
		#endtemplate

		#template ETERNAL_PARTICLE( epResult ) {
			float4 epCur = tex2D( $transMap, fragment.texcoord0.xy );
			float4 epNxt = tex2D( $transMap, fragment.texcoord0.zw );
			float epFb = fragment.texcoord1.z;
			float epR = lerp( epLin( epCur.x ), epLin( epNxt.x ), epFb );
			float epG = lerp( epLin( epCur.y ), epLin( epNxt.y ), epFb );
			clip( epG - 1e-6 );
			float epMode = $eternalParticle.x;
			float epGp0 = ( fragment.texcoord1.x - 1.0 ) / 10.0;
			float epGp1 = ( fragment.texcoord4.z - 1.0 ) / 10.0;
			float epGp3 = fragment.texcoord1.w;
			float3 epC = float3( epSrgb( fragment.color.x ), epSrgb( fragment.color.y ), epSrgb( fragment.color.z ) );

			float epCa = epSrgb( fragment.color.w ) * ( 1.0 - EP_MIN_ALPHA ) + EP_MIN_ALPHA;
			if ( $eternalParticleFade.x > $eternalParticleFade.y ) {
				float epD = length( fragment.texcoord2.xyz - $globalViewOrigin.xyz );
				epCa *= saturate( ( epD - $eternalParticleFade.y ) / ( $eternalParticleFade.x - $eternalParticleFade.y ) );
			}
			float2 epTc = fragment.position.xy * $renderPositionToViewTexture.zw + $renderPositionToViewTexture.xy;
			float epZs = $projectionMatrixZ.w / ( tex2Dlod( $viewDepthMap, float4( epTc, 0.0, 0.0 ) ).x + $projectionMatrixZ.z );
			float epZf = $projectionMatrixZ.w / ( fragment.position.z + $projectionMatrixZ.z );
			float epSoft = saturate( ( epZs - epZf - saturate( 1.0 - epG ) * $eternalParticle.w ) / max( $eternalParticleFade.z, 1e-3 ) );

			float epM = 1.0;
			if ( $eternalParticleFade.w != 0.0 ) {
				epM = lerp( tex2D( $eternalParticleEmissiveMap, fragment.texcoord0.xy ).x, tex2D( $eternalParticleEmissiveMap, fragment.texcoord0.zw ).x, epFb );
			}

			float3 epRgb;
			float epA;
			float epCurve = lerp( sqrt( epG ), epG, epGp3 );
			if ( epMode == 1.0 ) {
				float3 epL;
				EP_BICUBIC( epL, $particlesLightAtlasMap, fragment.texcoord4.xy, $particleAtlasParms.xy );
				epRgb = epR * epC * ( epL + 200.0 * epGp0 ) * $eternalParticleColor2.w;
				if ( $eternalParticle.z != 0.0 ) {
					epRgb += $eternalParticleColor.xyz * $eternalParticle.z * epM * epM;
				}
				epA = epCurve * epCa * ( 1.0 + 10.0 * epGp1 );
			} else if ( epMode == 4.0 ) {
				float3 epL;
				EP_BICUBIC( epL, $particlesLightAtlasMap, fragment.texcoord4.xy, $particleAtlasParms.xy );
				epRgb = lerp( epL * epR * epC, lerp( $eternalParticleColor.xyz, $eternalParticleColor2.xyz, epM ) * $eternalParticle.y, epM );
				float epO = epCurve * epCa;
				epA = lerp( sqrt( epO ), epO, epGp3 ) * epCa;
			} else {
				epRgb = epR * epC * ( 1.0 + 200.0 * epGp0 ) * $eternalParticle.y * epCa * epCa;
				epA = epCurve * epCa * ( 1.0 + 10.0 * epGp1 );
			}
			epA = saturate( epA * epSoft );

			float3 epHeightFog = lerp( $fogLightInscatterColor.xyz * $fogLightInscatterIntensity.x, $fogLightOppositeColor.xyz, fragment.texcoord6.z );
			float epFogAmount = saturate( fragment.texcoord6.y + fragment.texcoord6.x );
			float epFogSafe = max( epFogAmount, 0.0001 );
			float3 epFogColor = lerp( $fogColor.xyz, epHeightFog, max( 0.0, ( epFogSafe - fragment.texcoord6.x ) / epFogSafe ) );
			epRgb = lerp( epFogColor * $transparencyLightMult.x, epRgb, 1.0 - epFogAmount );

			epResult.color = float4( epRgb * epA, epA );
		}
		#endtemplate
