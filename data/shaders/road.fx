//////////////////////////////////////////////////////////////////////////////
//
// Workfile: road.fx
// Created by: Vano
//
// simple diffuse shader for roads (alpha-blended)
//
// $Id: road.fx,v 1.7 2005/10/05 08:18:14 goryh Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse texture
texture 		DiffMap0 : DIFFUSE_MAP_0;
texture			LightMap0: LIGHT_MAP_0;


// scale to calculate lightmap texcoords
const float2 lightmapScale: USER_FLOAT3_PARAM;


// Diffuse color
const float4 g_Ambient : LIGHT_AMBIENT = { 0.2f, 0.2f, 0.2f, 1.0f };
const float4 g_Diffuse : LIGHT_DIFFUSE = { 1.0f, 1.0f, 1.0f, 1.0f };
const float2 g_FogTerm : FOG_TERM = { 1.0f, 800.0f };


// transformation
row_major float4x4 	mFinal	: TOTAL_MATRIX;
row_major float4x4 	mWorld	: WORLD_MATRIX;


// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER_CLAMP( DiffSampler, DiffMap0 )

// declare lightmap sampler
DECLARE_DETAIL_SAMPLER( LightmapSampler, LightMap0 )


// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	: POSITION;		// position in object space
	float3 Normal	: NORMAL;		// normal in object space
	float2 Tex0	: TEXCOORD0;		// diffuse texcoords
};


// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos	: POSITION;
	float2 Tex0	: TEXCOORD0;
	float2 Tex1	: TEXCOORD1;
	float  fog	: FOG;
};


/**
	Simple diffuse vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In , uniform float4 ambient, uniform float4 diffuse )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// position ( projected )
	Out.Pos		= mul( float4( In.Pos, 1 ), mFinal );
	
	// texture coordinate
	Out.Tex0	= In.Tex0;

	// lightmap coords
	Out.Tex1 = mul( float4( In.Pos, 1 ), mWorld ).xz * lightmapScale;

	// fog coeff
	Out.fog 	    = VertexFog( Out.Pos.z, g_FogTerm );

	return Out;
}


/**
	Simple diffuse pixel shader for ps_2_0
 */
float4 PS11_DiffusePS( VS11_OUTPUT In ): COLOR
{
	return tex2D( DiffSampler, In.Tex0 ) * tex2D( LightmapSampler, In.Tex1 ) * 2;
}


//
technique Test1
<
	string 	Description = "simple diffuse shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat = "VERTEX_XYZNT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader = compile vs_2_0 PS11_DiffuseVS( g_Ambient, g_Diffuse );
		PixelShader = compile ps_2_0 PS11_DiffusePS();
		
		//FogEnable        = true;
		//CullMode	     = CCW;
		//FillMode	     = Solid;
		/*FillMode	     = Wireframe;*/
		/*ZWriteEnable     = True;*/
		//AlphaBlendEnable = true;
	}
}