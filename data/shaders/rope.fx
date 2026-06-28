//////////////////////////////////////////////////////////////////////////////
//
// Workfile: rope.fx
// Created by: Vano
//
// shader for rendering thin ropes
//
// $Id: rope.fx,v 1.1 2005/11/15 12:29:14 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse texture
texture DiffMap0 : DIFFUSE_MAP_0;

// Light direction( object space )
float3 LightDir	 : TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// Diffuse color
shared const float4 g_Ambient			: LIGHT_AMBIENT		 = { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse			: LIGHT_DIFFUSE		 = { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float2 g_FogTerm			: FOG_TERM			 = { 1.0f, 800.0f };
shared const float  g_TransStartDist	: TRANS_START_DIST	 = 10000.f;
shared const float  g_TransObjWidth		: TRANS_OBJECT_WIDTH = 0.f;

// transformation
row_major float4x4 mFinal				: TOTAL_MATRIX;
row_major float4x4 mView				: VIEW_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	    : POSITION;		// position in object space
	float3 Normal	: NORMAL;		// normal in object space
	float2 Tex0	    : TEXCOORD0;	// diffuse texcoords
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		: POSITION;
	float2 Tex0		: TEXCOORD0;
	float4 Clr      : COLOR0;
	float  fog		: FOG;
};

/**
	vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Distance to viewer
	float dist		= length( mul( float4( In.Pos, 1 ), mView ) );
	// Calculate interpolation value
	float k			= saturate( ( dist - g_TransStartDist ) / ( g_TransStartDist * 10.f ) );
	// Calculate object thickening coeff
	float othc		= g_TransObjWidth * k * 1.2f;	
	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos + In.Normal * othc, 1 ) , mFinal );
	// Texture coordinate
	Out.Tex0	    = In.Tex0;
	// Light color    
	Out.Clr.xyz     = saturate( dot( In.Normal , -LightDir ) ) * diffuse.xyz;       	
	// Transparency
	Out.Clr.w		= 1.f - k;
	// Fog coeff
	Out.fog 	    = VertexFog( Out.Pos.z, g_FogTerm );

	return Out;
}

/**
	pixel shader for ps_2_0
 */
float4 PS11_DiffusePS( VS11_OUTPUT In, uniform float4 ambient, uniform float4 diffuse ): COLOR
{		
	return tex2D( DiffSampler, In.Tex0 ) * float4( ( In.Clr + ambient ).xyz, In.Clr.w );
}

technique Test1
<
	string 	Description			= "rope shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader = compile vs_2_0 PS11_DiffuseVS( g_Diffuse );
		PixelShader = compile ps_2_0 PS11_DiffusePS( g_Ambient, g_Diffuse );
	}
}