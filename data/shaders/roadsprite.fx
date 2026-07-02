//////////////////////////////////////////////////////////////////////////////
//
// Workfile: roadSprite.fx
// Created by: Vano
//
// shader for rendering sprites on roads
//
// $Id: roadSprite.fx,v 1.3 2005/11/09 11:33:34 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Light direction( object space )
float3 LightDir						: TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// Weather params
shared const float4 g_Ambient		: LIGHT_AMBIENT	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE	= { 0.5f, 0.5f, 0.5f, 1.0f };
shared const float2 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f };

// transformations
row_major float4x4 mFinal		: TOTAL_MATRIX;

// texture transform martix
row_major float4x4 texTransform		: USER_FLOAT4x4_PARAM;
const float4	   VertColor		: USER_FLOAT4_PARAM;

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	     : POSITION;		// position in object space
};

// Vertex shader output structure
struct VS11_OUTPUT
{
	float4 Pos		     : POSITION;
	float2 Tex0		     : TEXCOORD0;
	float4 Clr			 : COLOR0;
};

// road sprite vertex shader
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In, uniform float4 ambient, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	In.Pos.y       += 0.05f;
	Out.Pos         = mul( float4( In.Pos, 1 ) , mFinal );
	// Texture coordinate
	Out.Tex0	= mul( float4( In.Pos, 1 ), texTransform );
	// Light color    
	Out.Clr		= float4( VertColor.xyz * ( diffuse * saturate( dot( float3( 0.f, 1.f, 0.f ), -LightDir ) ) + ambient ), VertColor.w );       
	// Fog coeff
	Out.Clr	       *= VertexFog( Out.Pos.z, g_FogTerm );

	return Out;
}

float4 SimplePS( VS11_OUTPUT In ) : COLOR
{
	return float4( 1, 0, 0, 1 ); 
}

technique Test1
<
	string 	Description = "road sprite shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat = "VERTEX_XYZNT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 PS11_DiffuseVS( g_Ambient, g_Diffuse );
		PixelShader		= NULL;
		//PixelShader	= compile ps_2_0  SimplePS();
	}
}