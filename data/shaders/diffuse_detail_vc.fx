//////////////////////////////////////////////////////////////////////////////
//
// Workfile: diffuse_detail_vc.fx
// Created by: Vano
//
// diffuse shader with detail texture and vertex color addition
//
// $Id: diffuse_detail_vc.fx,v 1.7 2005/09/06 10:56:14 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"

// Diffuse and detail texture
texture 		DiffMap0	: DIFFUSE_MAP_0;
texture			DetailMap	: DETAIL_MAP_0;

// Light direction( world space )
float3 LightDir				: TMP_LIGHT0_DIR
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

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	     : POSITION;		// position in object space
	float3 Normal	 : NORMAL;			// normal in object space
	float2 Tex0	     : TEXCOORD0;		// diffuse texture texcoords
	float2 Tex1	     : TEXCOORD1;		// detail texture texcoords
	float4 VertColor : COLOR0;			// vertex color
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		    : POSITION;
	float2 Tex0		    : TEXCOORD0;
	float2 Tex1			: TEXCOORD1;
	float4 Clr			: COLOR1;		// Diffuse color
	float4 VertColor	: COLOR0;		// Vertex color
	float  fog			: FOG;
};

/**
	diffuse + detail + vc vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ) , mFinal );
	// Diffuse texture coordinate
	Out.Tex0	    = In.Tex0;
	// Detail texture coordinate
	Out.Tex1	    = In.Tex1;
	// Diffuse light color    
	Out.Clr         = saturate( dot( In.Normal , -LightDir ) ) * diffuse;
	// Vertex color
	Out.VertColor   = In.VertColor;       
	// Fog coeff
	Out.fog 	    = VertexFog( Out.Pos.z, g_FogTerm );

	return Out;
}

/**
	diffuse + detail + vc vertex shader for ps_2_0
 */
REAL4 PS11_DiffusePS( VS11_OUTPUT In, uniform float4 ambient ) : COLOR
{

	// Base color
	float4 color = float4( ( In.Clr + ambient ).xyz * 2, g_Transparency ) * tex2D( DiffSampler, In.Tex0 ) * In.VertColor;
	// Full color
	return color * tex2D( DetailSampler, In.Tex1 );

/*
	// Base color
	float4 color = tex2D( DiffSampler, In.Tex0 ) * ( In.Clr + ambient ) * In.VertColor;
	// Full color
	return color * tex2D( DetailSampler, In.Tex1 ) * 2;
*/
/*
	// Detail color
	REAL4 detail = tex2D( DetailSampler, In.Tex1 );
	// Full color
	return tex2D( DiffSampler, In.Tex0 ) * ( In.Clr + ambient ) * In.VertColor * detail * 2;
*/
}

technique Test1
<
	string 	Description			= "diffuse + detail + vc shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNCT2";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 PS11_DiffuseVS( g_Diffuse );
		PixelShader	= compile ps_2_0 PS11_DiffusePS( g_Ambient );
		
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