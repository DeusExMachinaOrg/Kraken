//////////////////////////////////////////////////////////////////////////////
//
// Workfile: shadow.fx
// Created by: Vano
//
// shader for rendering shadows to texture
//
// $Id: shadow.fx,v 1.1 2005/03/17 12:34:33 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"

// Diffuse texture
texture 			DiffMap0	: DIFFUSE_MAP_0;
// transformations
row_major float4x4	mFinal		: TOTAL_MATRIX;
// Color mask
float3				colorMask	: USER_FLOAT3_PARAM	= { 0, 0, 0 };

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	     : POSITION;		// position in object space
	float2 Tex0	     : TEXCOORD0;		// diffuse texcoords
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		     : POSITION;
	float2 Tex0		     : TEXCOORD0;
};

/**
	Simple shadow vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ) , mFinal );
	// Texture coordinate
	Out.Tex0	    = In.Tex0;

	return Out;
}

/**
	Simple shadow pixel shader for ps_2_0
 */
float4 PS11_DiffusePS( VS11_OUTPUT In ): COLOR
{
	//float4 texClr = tex2D( DiffSampler, In.Tex0 ); 
	return float4( colorMask, tex2D( DiffSampler, In.Tex0 ).a );
}


technique Test1
<
	string 	Description = "simple shadow shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat = "VERTEX_XYZNT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 PS11_DiffuseVS();
		PixelShader		= compile ps_2_0 PS11_DiffusePS();
	}
}