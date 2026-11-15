		// radialblur.decl.inc -- Snapmap+'s radial blur in the game's post process (modprog.h) in the render program
		// language, for the OpenGL renderer, which compiles a program from its decl text. modprog.c puts this text in
		// front of the post process template and reads each clamped scene sample through RADIAL_BLUR_SAMPLE. The
		// same function, line for line, as rb_sample in radialblur.frag (the Vulkan module): see there for the rule.
		// The engine compiles a program's text inside main(), so the helpers are templates, never functions.
		#template RB_SETUP( rbI, rbA, rbQ, rbE ) {
			float4 rbP = float4( rbA.xyz, 1.0 );
			float rbCx = dot( $viewProjectionMatrixX, rbP );
			float rbCy = dot( $viewProjectionMatrixY, rbP );
			float rbW = dot( $viewProjectionMatrixW, rbP );
			float rbNear = 0.0;
			if ( $projectionMatrixZ.z != 0.0 ) {
				rbNear = $projectionMatrixZ.w / $projectionMatrixZ.z;
			}
			rbS[ rbI ] = 0.0;
			rbC[ rbI ] = float2( 0.5, 0.5 );
			rbR[ rbI ] = 1.0;
			rbF[ rbI ] = rbQ.y;
			rbRing[ rbI ] = rbQ.z;
			if ( rbA.w > 0.0 && rbW >= rbNear && rbQ.x > 0.0 ) {
				float2 rbCen = float2( rbCx / rbW * 0.5 + 0.5, $radialBlur0.x * -rbCy / rbW * 0.5 + 0.5 );
				float rbRad = length( $viewProjectionMatrixY.xyz ) * rbQ.x / rbW * 0.5;
				float2 rbHalf = float2( ( 1.0 + rbQ.z ) * rbRad / rbAspect, ( 1.0 + rbQ.z ) * rbRad );
				if ( rbCen.x + rbHalf.x >= 0.0 && rbCen.x - rbHalf.x <= 1.0 && rbCen.y + rbHalf.y >= 0.0 && rbCen.y - rbHalf.y <= 1.0 ) {
					float rbStr = rbA.w * min( 1.0, rbRad / rbE.y ) * saturate( ( rbW - rbNear ) / rbE.z );
					rbStr = min( rbStr, rbE.x );
					float rbZone = rbQ.z * rbRad;
					float rbFall = rbQ.y;
					if ( min( rbZone / rbFall, rbZone ) > rbE.w ) {
						if ( rbFall < 1.0 ) {
							rbStr *= rbFall;
						}
						rbFall = rbZone / rbE.w;
					}
					float2 rbLo = max( float2( 0.0, 0.0 ), rbCen - rbHalf );
					float2 rbHi = min( float2( 1.0, 1.0 ), rbCen + rbHalf );
					float rbSeen = 0.0;
					for ( int rbK = 0; rbK < 8; rbK++ ) {
						float2 rbT = float2( 0.1667, 0.1667 );
						if ( rbK == 1 ) { rbT = float2( 0.5, 0.1667 ); }
						if ( rbK == 2 ) { rbT = float2( 0.8333, 0.1667 ); }
						if ( rbK == 3 ) { rbT = float2( 0.1667, 0.5 ); }
						if ( rbK == 4 ) { rbT = float2( 0.8333, 0.5 ); }
						if ( rbK == 5 ) { rbT = float2( 0.1667, 0.8333 ); }
						if ( rbK == 6 ) { rbT = float2( 0.5, 0.8333 ); }
						if ( rbK == 7 ) { rbT = float2( 0.8333, 0.8333 ); }
						float2 rbUv = rbLo + rbT * ( rbHi - rbLo );
						float rbZs = $projectionMatrixZ.w / ( tex2Dlod( $viewDepthMap, float4( rbUv * $resolutionScale.xy, 0.0, 0.0 ) ).x + $projectionMatrixZ.z );
						if ( rbW < rbZs || !( rbZs > 0.0 ) ) {
							rbSeen += 1.0;
						}
					}
					rbS[ rbI ] = rbStr * min( 1.0, rbSeen * 0.125 );
					rbC[ rbI ] = rbCen;
					rbR[ rbI ] = rbRad;
					rbF[ rbI ] = rbFall;
				}
			}
		}
		#endtemplate

		#template RB_VECTOR( rbV, rbAt ) {
			float2 rbSum = float2( 0.0, 0.0 );
			float rbWsum = 1e-6;
			for ( int rbB = 0; rbB < 7; rbB++ ) {
				if ( rbS[ rbB ] > 0.0 ) {
					float2 rbD = rbC[ rbB ] - ( rbAt );
					rbD.x *= rbAspect;
					float rbL = max( length( rbD ), 1e-6 );
					float rbKk = saturate( rbF[ rbB ] * ( 1.0 - abs( rbL - rbR[ rbB ] ) / ( rbRing[ rbB ] * rbR[ rbB ] ) ) ) * rbS[ rbB ];
					rbSum += rbD / rbL * rbKk * rbKk;
					rbWsum += rbKk;
				}
			}
			float2 rbVv = rbSum / rbWsum;
			rbVv = ( floor( clamp( rbVv, float2( -1.0, -1.0 ), float2( 1.0, 1.0 ) ) * 127.0 + 128.0 + 0.5 ) - 128.0 ) / 127.0;
			rbV = rbVv * $radialBlur0.z;
		}
		#endtemplate

		#template RADIAL_BLUR_SAMPLE( rbOut, rbTc ) {
			rbOut = tex2Dlod( $tex0, float4( rbTc, 0.0, 0.0 ) );
			if ( $radialBlur0.y > 0.0 ) {
				float rbAspect = $renderViewSize.x / $renderViewSize.y;
				float rbS[ 7 ];
				float2 rbC[ 7 ];
				float rbR[ 7 ];
				float rbF[ 7 ];
				float rbRing[ 7 ];
				{ RB_SETUP( 0, $radialBlur1, $radialBlur2, float4( $radialBlur2.w, $radialBlur15.xyz ) ); }
				{ RB_SETUP( 1, $radialBlur3, $radialBlur4, float4( $radialBlur4.w, $radialBlur15.xyz ) ); }
				{ RB_SETUP( 2, $radialBlur5, $radialBlur6, float4( $radialBlur6.w, $radialBlur15.xyz ) ); }
				{ RB_SETUP( 3, $radialBlur7, $radialBlur8, float4( $radialBlur8.w, $radialBlur15.xyz ) ); }
				{ RB_SETUP( 4, $radialBlur9, $radialBlur10, float4( $radialBlur10.w, $radialBlur15.xyz ) ); }
				{ RB_SETUP( 5, $radialBlur11, $radialBlur12, float4( $radialBlur12.w, $radialBlur15.xyz ) ); }
				{ RB_SETUP( 6, $radialBlur13, $radialBlur14, float4( $radialBlur14.w, $radialBlur15.xyz ) ); }
				if ( rbS[ 0 ] > 0.0 || rbS[ 1 ] > 0.0 || rbS[ 2 ] > 0.0 || rbS[ 3 ] > 0.0 || rbS[ 4 ] > 0.0 || rbS[ 5 ] > 0.0 || rbS[ 6 ] > 0.0 ) {
					float2 rbUv0 = ( rbTc ) / $resolutionScale.xy;
					float2 rbV0;
					RB_VECTOR( rbV0, rbUv0 );
					if ( dot( rbV0, rbV0 ) > 0.0 ) {
						float2 rbCoarse = rbV0 / 7.0;
						rbCoarse.x /= rbAspect;
						float2 rbLim = $resolutionScale.xy - $renderPositionToViewTexture.zw * 0.5;
						float4 rbAcc = float4( 0.0, 0.0, 0.0, 0.0 );
						for ( int rbJ = 0; rbJ < 7; rbJ++ ) {
							float2 rbQq = rbUv0 + float( rbJ ) * rbCoarse;
							float2 rbFine;
							RB_VECTOR( rbFine, rbQq );
							rbFine = rbFine / 49.0;
							rbFine.x /= rbAspect;
							for ( int rbI2 = 0; rbI2 < 7; rbI2++ ) {
								rbAcc += tex2Dlod( $tex0, float4( clamp( ( rbQq + float( rbI2 ) * rbFine ) * $resolutionScale.xy, float2( 0.0, 0.0 ), rbLim ), 0.0, 0.0 ) );
							}
						}
						rbOut = rbAcc / 49.0;
					}
				}
			}
		}
		#endtemplate
