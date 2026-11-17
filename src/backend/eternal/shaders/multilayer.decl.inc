		// multilayer.decl.inc -- the multilayer blend of the product's render programs (modprog.h) in the render
		// program language, for the OpenGL renderer, which compiles a program from its decl text. modprog.c inserts this
		// text before the base program's CUSTOM_TEXTURE_INPUTS template and MULTILAYER_BLEND( inputs ) as that template's
		// first statement. The same function, line for line, as ml_blend in multilayer.frag (the Vulkan module): see there
		// for DOOM Eternal's rule and what each input becomes. The engine compiles a program's fragment text inside
		// main(), so the helpers are macros and templates, never functions.
		#define mlSrgbEncode( v ) ( saturate( v ) <= 0.0031308 ? saturate( v ) * 12.92 : 1.055 * pow( saturate( v ), 1.0 / 2.4 ) - 0.055 )
		#define mlSrgbDecode( v ) ( ( v ) <= 0.04045 ? ( v ) / 12.92 : pow( ( ( v ) + 0.055 ) / 1.055, 2.4 ) )
		#template ML_DECODE_NORMAL( dst, v ) {
			float2 mlDn = ( v ) * ( 255.0 / 128.0 ) - 1.0;
			dst = float3( mlDn.x, mlDn.y, sqrt( saturate( 1.0 - mlDn.x * mlDn.x - mlDn.y * mlDn.y ) ) );
		}
		#endtemplate

		#template MULTILAYER_LAYER( L, albedoMap, normalMap, smoothnessMap, heightMap, layerTiling, layerSpecular, layerEmissive ) {
			float m = mlMask.L;
			BRANCH if ( m != 0.0 ) {
				float tl = $layersTilingFactor.L;
				float hi = ( tex2D( heightMap, mlUv * tl, mlDx * tl, mlDy * tl ).x - 0.49803921580314636 ) * $multiblendHeightmapScale.L;
				float a = ( mlHb + 1.0 ) - m;
				float b = ( hi + 0.0 ) + m;
				float t0 = clamp( ( b - a ) / $multiblendSoftness.L, -1.0, 1.0 ) / 2.0 + 0.5;
				float f = saturate( ( abs( 0.5 - m ) - ( 0.5 - 0.1 ) ) / 0.1 );
				float t = t0;
				if ( m < 0.1 && t0 > m ) {
					t = lerp( t0, m, f );
				} else if ( m > 1.0 - 0.1 && t0 < m ) {
					t = lerp( t0, m, f );
				}
				float2 lsc = layerTiling.xy;
				float2 lu = ( mlUv * tl ) * lsc + _float2( mlTime ) * layerTiling.zw;
				float2 ldx = mlDx * tl * lsc;
				float2 ldy = mlDy * tl * lsc;
				BRANCH if ( $multiblendAlbedoScale.L != 0.0 ) {
					mlAlbedo = lerp( mlAlbedo, float4( tex2D( albedoMap, lu, ldx, ldy ).xyz, 1.0 ), _float4( t * $multiblendAlbedoScale.L ) );
				}
				BRANCH if ( $multiblendSpecularScale.L != 0.0 ) {
					float3 ls = layerSpecular.w == 1.0 ? tex2D( $multilayerExtraMap0, lu, ldx, ldy ).xyz :
						( layerSpecular.w == 2.0 ? tex2D( $multilayerExtraMap1, lu, ldx, ldy ).xyz : layerSpecular.xyz );
					mlSpecular = lerp( mlSpecular, float4( ls, 1.0 ), _float4( t * $multiblendSpecularScale.L ) );
				}
				BRANCH if ( $multiblendSmoothnessScale.L != 0.0 ) {
					mlSmooth = lerp( mlSmooth, tex2D( smoothnessMap, lu, ldx, ldy ).x, t * $multiblendSmoothnessScale.L );
				}
				BRANCH if ( $multiblendNormalScale.L != 0.0 ) {
					mlNormal = lerp( mlNormal, float4( tex2D( normalMap, lu, ldx, ldy ).xy, 0.0, 0.0 ), _float4( t * $multiblendNormalScale.L ) );
				}
				BRANCH if ( $multiblendEmissiveScale.L != 0.0 ) {
					float3 le = layerEmissive.w == 1.0 ? tex2D( $multilayerExtraMap0, lu, ldx, ldy ).xyz * layerEmissive.xyz :
						( layerEmissive.w == 2.0 ? tex2D( $multilayerExtraMap1, lu, ldx, ldy ).xyz * layerEmissive.xyz : layerEmissive.xyz );
					mlEmissive = lerp( mlEmissive, float4( le, 0.0 ), _float4( t * $multiblendEmissiveScale.L ) );
				}
			}
		}
		#endtemplate

		#template MULTILAYER_EMBER( mlR ) {
			float2 mlBt = $multilayerEmberTiling.xy;
			float4 mlB = $multilayerEmber.z == 1.0 ? tex2D( $multilayerExtraMap0, mlUv * mlBt, mlDx * mlBt, mlDy * mlBt ) :
				( $multilayerEmber.z == 2.0 ? tex2D( $multilayerExtraMap1, mlUv * mlBt, mlDx * mlBt, mlDy * mlBt ) : float4( 0.5, 0.5, 0.0, 0.0 ) );
			float mlG = $multilayerEmber.w == 1.0 ? tex2D( $multilayerExtraMap0, mlUv, mlDx, mlDy ).y :
				( $multilayerEmber.w == 2.0 ? tex2D( $multilayerExtraMap1, mlUv, mlDx, mlDy ).y : 1.0 );
			float mlIg = 1.0 - mlG;
			float mlBb = mlB.z;
			float mlW = saturate( 1.0 - mlG * mlG );
			float mlW2 = mlW * mlW;
			float mlW3 = mlW2 * mlW;
			float mlM = saturate( mlW3 * mlBb * ( 2.0 - mlW ) - ( 1.0 - mlW3 ) + saturate( 8.0 * mlW2 - 7.0 ) );
			mlM = saturate( 16.0 * mlM );
			float mlM2 = mlM * mlM;
			mlSpecular.xyz = mlSpecular.xyz + ( _float3( 0.02 ) - mlSpecular.xyz ) * mlM2;
			mlSmooth = mlSmooth + ( 0.75 - mlSmooth ) * mlM2;
			float2 mlEn = ( mlR.xy + 1.0 ) * 0.5;
			float2 mlNxy = mlEn + ( mlB.xy - mlEn ) * mlIg;
			float3 mlChar = _float3( 0.01 ) + ( float3( 0.12, 0.11, 0.10 ) - _float3( 0.01 ) ) * mlBb;
			mlAlbedo.xyz = mlAlbedo.xyz + ( mlChar - mlAlbedo.xyz ) * mlIg;
			mlSmooth = mlSmooth + ( ( 0.1 + ( 0.7 - 0.1 ) * mlBb ) - mlSmooth ) * mlIg;
			mlSpecular.xyz = mlSpecular.xyz + ( _float3( 0.048 ) - mlSpecular.xyz ) * mlIg;
			float3 mlHot = ( mlBb * mlBb ) * $multilayerEmberHot.xyz;
			float3 mlE1 = mlHot + ( mlEmissive.xyz - mlHot ) * mlG;
			float mlS = ( mlB.w + ( mlBb - mlB.w ) * mlIg ) * mlIg;
			float3 mlK = ( _float3( mlS ) + ( mlE1 - _float3( mlS ) ) * mlG ) * float3( 1.0, 0.07, 0.0 ) * 25.0 * ( mlNxy.x * mlNxy.y );
			float4 mlNt = $multilayerEmberNoise;
			float2 mlU1 = float2( mlUv.x * mlNt.x, mlUv.y * mlNt.x + mlTime * mlNt.z );
			float2 mlU2 = float2( mlUv.x * mlNt.y, mlUv.y * mlNt.y - mlTime * mlNt.w );
			float mlN1 = tex2D( $magnificationClouds, mlU1, mlDx * mlNt.x, mlDy * mlNt.x ).x;
			float mlN2 = tex2D( $magnificationClouds, mlU2, mlDx * mlNt.y, mlDy * mlNt.y ).x;
			float mlFl = saturate( mlN1 * mlN1 * mlN2 * mlN2 + 0.12 );
			mlEmissive.xyz = mlE1 + mlK * mlFl;
			float2 mlX = mlNxy * 2.0 - 1.0;
			mlR = float3( mlX.x, mlX.y, sqrt( saturate( 1.0 - mlX.x * mlX.x - mlX.y * mlX.y ) ) );
		}
		#endtemplate

		#template MULTILAYER_BLEND( inputs ) {
			float2 mlUv = inputs.texCoords.xy;
			float2 mlDx = ddx( mlUv );
			float2 mlDy = ddy( mlUv );
			float mlTime = $time.x;
			float4 mlAlbedo = float4( inputs.albedo.xyz, 1.0 );
			float4 mlSpecular = float4( inputs.specular.xyz, 1.0 );
			float mlSmooth = mlSrgbEncode( inputs.smoothness );
			float2 mlBaseNormal = inputs.normal.xy;
			float4 mlNormal = float4( 0.5, 0.5, 0.0, 0.0 );
			float mlRaw0 = inputs.lightmap.x;	// decoded from the sRGB plane: its square is layer 0's intensity
			float4 mlEmissive = float4( $blendLayer0Emissive.xyz * ( mlRaw0 * mlRaw0 ), 0.0 );
			float2 mlUv0 = mlUv * $blendLayer0Tiling.xy + _float2( mlTime ) * $blendLayer0Tiling.zw;
			BRANCH if ( $multilayerEmber.y != 0.0 ) {
				float2 mlD0x = mlDx * $blendLayer0Tiling.xy;
				float2 mlD0y = mlDy * $blendLayer0Tiling.xy;
				float3 mlE0 = $multilayerEmber.y == 1.0 ? tex2D( $multilayerExtraMap0, mlUv0, mlD0x, mlD0y ).xyz : tex2D( $multilayerExtraMap1, mlUv0, mlD0x, mlD0y ).xyz;
				mlEmissive = float4( mlE0 * $blendLayer0Emissive.xyz * $blendLayer0Emissive.w, 0.0 );
			}
			float mlH0 = tex2D( $blendLayer0HeightMap, mlUv0, mlDx * $blendLayer0Tiling.xy, mlDy * $blendLayer0Tiling.xy ).x;
			float4 mlMask = min( tex2D( $layerBlendMask, mlUv, mlDx, mlDy ) * $multiblendLayerScale, _float4( 1.0 ) );
			float mlHb = ( mlH0 - 0.49803921580314636 ) * $blendHeightmapScale.x;
			MULTILAYER_LAYER( x, $blendLayer1AlbedoMap, $blendLayer1NormalMap, $blendLayer1SmoothnessMap, $blendLayer1HeightMap, $blendLayer1Tiling, $blendLayer1Specular, $blendLayer1Emissive );
			MULTILAYER_LAYER( y, $blendLayer2AlbedoMap, $blendLayer2NormalMap, $blendLayer2SmoothnessMap, $blendLayer2HeightMap, $blendLayer2Tiling, $blendLayer2Specular, $blendLayer2Emissive );
			MULTILAYER_LAYER( z, $blendLayer3AlbedoMap, $blendLayer3NormalMap, $blendLayer3SmoothnessMap, $blendLayer3HeightMap, $blendLayer3Tiling, $blendLayer3Specular, $blendLayer3Emissive );
			MULTILAYER_LAYER( w, $blendLayer4AlbedoMap, $blendLayer4NormalMap, $blendLayer4SmoothnessMap, $blendLayer4HeightMap, $blendLayer4Tiling, $blendLayer4Specular, $blendLayer4Emissive );
			// reoriented normal mapping onto layer 0's normal
			float3 mlN0;
			ML_DECODE_NORMAL( mlN0, mlBaseNormal );
			float3 mlD;
			ML_DECODE_NORMAL( mlD, mlNormal.xy );
			float3 mlT = mlN0 + float3( 0.0, 0.0, 1.0 );
			float3 mlU = float3( -mlD.x, -mlD.y, mlD.z );
			float3 mlR = normalize( mlT * dot( mlT, mlU ) - mlU * mlT.z );
			BRANCH if ( $multilayerEmber.x != 0.0 ) {
				MULTILAYER_EMBER( mlR );
			}
			inputs.albedo.xyz = mlAlbedo.xyz;
			float mlGrey = dot( float3( mlSrgbEncode( mlSpecular.x ), mlSrgbEncode( mlSpecular.y ), mlSrgbEncode( mlSpecular.z ) ), float3( 0.2126, 0.7152, 0.0722 ) );
			inputs.specular.xyz = _float3( mlSrgbDecode( mlGrey ) );
			inputs.smoothness = mlSrgbDecode( saturate( mlSmooth ) );
			inputs.normal.xy = saturate( ( mlR.xy * 128.0 + 128.0 ) / 255.0 );
			float mlEm = max( mlEmissive.x, max( mlEmissive.y, mlEmissive.z ) );
			// the base program adds the emissive twice (the ambient light the albedo multiplies, and its composite):
			// the mask is divided by 1 + colorscale x color x albedo in the bloom colour's brightest channel
			float3 mlKa = _float3( $colorScale.x ) * $color.xyz * mlAlbedo.xyz;
			float mlKj = ( $bloomColor.x >= max( $bloomColor.y, $bloomColor.z ) ) ? mlKa.x : ( ( $bloomColor.y >= $bloomColor.z ) ? mlKa.y : mlKa.z );
			mlEm = mlEm / ( 1.0 + max( mlKj, 0.0 ) );
			float mlLm = sqrt( saturate( mlEm ) );
			inputs.lightmap.x = mlLm;
			inputs.emissive = _float3( $bloomMaskScale.x * mlLm * mlLm * $colorScale.x ) * $color.xyz * $bloomColor.xyz;
			inputs.thickness = inputs.lightmap.y;
		}
		#endtemplate
