//////////////////////////////////////////////////////////////////////////////
//
// Workfile: landscapeSolid_ps11.vs
// Created by: Goryh
//
//
// $Id: landscapeDeep_ps20.vs,v 1.3 2005/10/25 08:49:09 goryh Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// vertex model*view*projection (c0 - c3)
float4x4 mViewProj: register( c0 ); 

float4 g_FogTerm: register( c4 );

float3 initPos: register( c5 );
float3 initTex: register( c6 );

// texture model*view*projection
float4x4 mTexture: register( c10 );

// camera position
float3 viewPos: register( c14 );

float3 waterDye: register( c15 );

// vertex shader output structure
struct VS_OUTPUT
{
	float4 Pos	: POSITION;
	float4 Tex	: TEXCOORD0;
	float4 col      : COLOR0;
};

/// 
VS_OUTPUT LandscapeVS(  float Pos : POSITION, int2 AltPos : TEXCOORD0 )
{
	VS_OUTPUT o = (VS_OUTPUT)0;

	// unpack vertex pos
	float4 posInfo;
	int indX =  AltPos.x / 128;
	int indY =  AltPos.x - indX * 128;
	posInfo.x  = initPos.x + initPos.z * indX;
 	posInfo.z  = initPos.y + initPos.z * indY;
	posInfo.y  = Pos.x;
        posInfo.w  = 1;

     	o.Pos = mul( posInfo, mViewProj );
         // project texcoords
	o.Tex = mul( posInfo, mTexture );

	// water depth, measured vertically: waterLevel (g_FogTerm.w) - terrainHeight (posInfo.y).
	// The original used the full ray path length through water -- length(viewPos - posInfo) scaled
	// by the depth fraction -- which folds in the entire camera distance, so d ran into the
	// thousands and exp(-d * waterDye) crushed the tint to black. The vertical depth is bounded,
	// so the tint stays in range (trade-off: no view-angle dependence).
	float d = max( 0.0f, g_FogTerm.w - posInfo.y * 0.5);
	
	float fog = VertexFog( o.Pos.z, g_FogTerm );

	o.col = float4( ( exp ( -d * waterDye.x )  ),
			( exp ( -d * waterDye.y ) ),
			( exp ( -d * waterDye.z ) ), 1 - saturate( exp( -0.01f * d ) ) );
	return o;
}