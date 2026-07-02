//////////////////////////////////////////////////////////////////////////////
//
// Workfile: diffuse_vc.fx
// Created by: Vano
//
// simple diffuse shader with vertex coloring
//
// $Id: diffuse_vc.fx,v 1.18 2005/09/06 10:56:14 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"

// Diffuse texture
texture DiffMap0	: DIFFUSE_MAP_0;

// Light direction( world space )
float3	LightDir	: TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// Diffuse color
shared const float4 g_Ambient		: LIGHT_AMBIENT = { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE = { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float2 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f };
shared const float  g_Transparency	: TRANSPARENCY	= 1.f;

// transformations
row_major float4x4 mFinal		: TOTAL_MATRIX;
//float4x4 mWorld		: WORLD_MATRIX;
//float4x4 mInvWorld	: INV_WORLD_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos		     : POSITION;		// position in object space
	float3 Normal	     : NORMAL;			// normal in object space
	float2 Tex0		     : TEXCOORD0;		// diffuse texcoords
	float4 VertColor	 : COLOR0;			// vertex color
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		     : POSITION;
	float2 Tex0		     : TEXCOORD0;
	float4 Clr			 : COLOR1;		// Diffuse color
	float4 VertColor	 : COLOR0;		// Vertex color
	float  fog			 : FOG;
};

/**
	Simple diffuse + vertex color vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_Diffuse_VC_VS( VS_INPUT In, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ) , mFinal );
	// Texture coordinate
	Out.Tex0	    = In.Tex0;
	// Diffuse light color     
	Out.Clr         = saturate( dot( In.Normal, -LightDir ) ) * diffuse;
	// Vertex color 
	Out.VertColor   = In.VertColor;
	// Fog coeff
	Out.fog         = VertexFog( Out.Pos.z, g_FogTerm );   

	return Out;
}

/**
	Simple diffuse + vertex color pixel shader for ps_2_0
 */
float4 PS11_Diffuse_VC_PS( VS11_OUTPUT In, uniform float4 ambient ): COLOR
{
	return tex2D( DiffSampler, In.Tex0 ) * float4( ( In.Clr + ambient ).xyz, g_Transparency ) * In.VertColor;
}

technique Test1
<
	string 	Description = "simple diffuse shader + vertex colors";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat = "VERTEX_XYZNCT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader = compile vs_2_0 PS11_Diffuse_VC_VS( g_Diffuse );
		PixelShader  = compile ps_2_0 PS11_Diffuse_VC_PS( g_Ambient );

		//AlphaBlendEnable = false;
		//AlphaTestEnable  = true;
		//FogEnable        = false;
		//CullMode         = CCW;
		//FillMode         = Solid;
		//ZWriteEnable     = True;
		//AlphaTestEnable  = true;
		//AlphaFunc = GreaterEqual;
		//AlphaRef = 100;
	}
}
