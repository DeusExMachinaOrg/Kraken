//////////////////////////////////////////////////////////////////////////////
//
// Workfile: wheeltrace.fx
// Created by: Vano
//
//  shader for wheeltraces
//
// $Id: wheeltrace.fx,v 1.2 2005/10/14 11:24:43 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Light direction( object space )
float3 LightDir			 : TMP_LIGHT0_DIR
<
	int Space = SPACE_WORLD;
>;

// Diffuse color
shared const float4 g_Ambient : LIGHT_AMBIENT	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse : LIGHT_DIFFUSE	= { 0.5f, 0.5f, 0.5f, 1.0f };
shared const float2 g_FogTerm : FOG_TERM		= { 1.0f, 800.0f };

// transformations
row_major float4x4 mFinal			 : TOTAL_MATRIX;

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos		: POSITION;		// position in object space
	float4 Clr		: COLOR0;		// vertex color
	float2 Tex0		: TEXCOORD0;	// diffuse texcoords
};

// Vertex shader output structure
struct VS11_OUTPUT
{
	float4 Pos		: POSITION;
	float2 Tex0		: TEXCOORD0;
	float4 Clr      : COLOR0;
	float  fog		: FOG;
};

/**
	wheeltrace vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In, uniform float4 ambient, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos		= mul( float4( In.Pos, 1 ) , mFinal );
	// Texture coordinate
	Out.Tex0	= In.Tex0;
	// Light color    
	Out.Clr		= float4( In.Clr.xyz * ( diffuse * saturate( dot( float3( 0.f, 1.f, 0.f ), -LightDir ) ) + ambient ), In.Clr.w );
	// fog terms
	Out.fog		= VertexFog( Out.Pos.z, g_FogTerm ); 

	return Out;
}

technique Test1
<
	string 	Description = "wheeltrace shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat = "VERTEX_XYZCT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 PS11_DiffuseVS( g_Ambient, g_Diffuse );
		PixelShader		= NULL;
	}
}