//////////////////////////////////////////////////////////////////////////////
//
// Workfile: lightmap_detail.fx
// Created by: Vano
//
// diffuse shader with detail and lightmap textures addition
//
// $Id: lightmap_detail.fx,v 1.2 2005/09/06 10:56:14 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"

// Diffuse, detail and lightmap textures
texture DiffMap0 	: DIFFUSE_MAP_0;
texture	DetailMap	: DETAIL_MAP_0;
texture	LightMap	: LIGHT_MAP_0;

// Light direction( world space )
float3 LightDir			 : TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// Diffuse color
shared const float4 g_Ambient		: LIGHT_AMBIENT	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE	= { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float2 g_FogTerm		: FOG_TERM		= { 1.0f, 800.0f };
shared const float  g_Transparency	: TRANSPARENCY	= 1.f;

// transformations
row_major float4x4 mFinal			 : TOTAL_MATRIX;
//float4x4 mWorld			 : WORLD_MATRIX;
//float4x4 mInvWorld		 : INV_WORLD_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )

// declare detail sampler
DECLARE_DETAIL_SAMPLER( DetailSampler, DetailMap )

// declare lightmap sampler
DECLARE_DETAIL_SAMPLER( LightMapSampler, LightMap )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	     : POSITION;		// position in object space
	float3 Normal	 : NORMAL;			// normal in object space
	float2 Tex0	     : TEXCOORD0;		// diffuse texture texcoords
	float2 Tex1	     : TEXCOORD1;		// lightmap texture texcoords
	float2 Tex2	     : TEXCOORD2;		// detail texture texcoords
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		    : POSITION;
	float2 Tex0		    : TEXCOORD0;
	float2 Tex1			: TEXCOORD1;
	float2 Tex2			: TEXCOORD2;
	float4 Clr          : COLOR0;
	float  fog			: FOG;
};

/**
	diffuse + detail + lightmap vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ) , mFinal );
	// Diffuse texture coordinate
	Out.Tex0	    = In.Tex0;
	// Lightmap texture coordinate
	Out.Tex1	    = In.Tex1;
	// Detail texture coordinate
	Out.Tex2	    = In.Tex2;	
	// Light color    
	Out.Clr         = saturate( dot( In.Normal , -LightDir ) ) * diffuse;       
	// Fog coeff
	Out.fog 	    = VertexFog( Out.Pos.z, g_FogTerm );

	return Out;
}

/**
	diffuse + detail + lightmap pixel shader for ps_2_0
 */
float4 PS11_DiffusePS( VS11_OUTPUT In, uniform float4 ambient ) : COLOR
{
	// Base color
	float4 color = float4( ( In.Clr + ambient ).xyz * 4, g_Transparency ) * tex2D( DiffSampler, In.Tex0 );
	//Full color
	return color * tex2D( DetailSampler, In.Tex2 ) * tex2D( LightMapSampler, In.Tex1 );
/*
	// Base color
	float4 color = tex2D( DiffSampler, In.Tex0 ) * ( In.Clr + ambient );
	// Detail color
	color *= tex2D( DetailSampler, In.Tex2 ) * 2;
	//Full color
	return color * tex2D( LightMapSampler, In.Tex1 ) * 2;
*/	
}

technique Test1
<
	string 	Description			= "diffuse + detail + lightmap shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT3";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader	 = compile vs_2_0 PS11_DiffuseVS( g_Diffuse );
		PixelShader		 = compile ps_2_0 PS11_DiffusePS( g_Ambient );
		
		//FogEnable        = true;
		//CullMode	     = CCW;
		//FillMode	     = Solid;
		//ZWriteEnable     = true;
		//AlphaBlendEnable = false;
		//AlphaTestEnable  = true;
		//AlphaFunc		 = GreaterEqual;
		//AlphaRef		 = 100;
	}
}