//////////////////////////////////////////////////////////////////////////////
//
// Workfile: ps_nofog_light.fx
// Created by: Vano
//
//  shader for light particles
//
// $Id: $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse texture
texture 		DiffMap0 : DIFFUSE_MAP_0;

// transformations
row_major float4x4 mFinal		: TOTAL_MATRIX;
shared const float2 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f };
// declare base diffuse sampler
DECLARE_SHADOW_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos		: POSITION;		// position in object space
	float4 Clr		: COLOR0;			// vertex color
	float2 Tex0		: TEXCOORD0;		// diffuse texcoords
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		: POSITION;
	float2 Tex0		: TEXCOORD0;
	float4 Clr		: COLOR0;
};

/**
	ps light vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos		= mul( float4( In.Pos, 1 ) , mFinal );
	// Texture coordinate
	Out.Tex0		= In.Tex0;
	// Light color    
	Out.Clr		= In.Clr;       

	return Out;
}

/**
	ps light pixel shader for ps_2_0
 */
float4 PS11_DiffusePS( VS11_OUTPUT In ): COLOR
{
	return tex2D( DiffSampler, In.Tex0 ) * In.Clr;
}

technique Test1
<
	string 	Description = "ps light shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat = "VERTEX_XYZCT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 PS11_DiffuseVS();
		//PixelShader		= compile ps_2_0 PS11_DiffusePS();
		PixelShader		= NULL;
	}
}