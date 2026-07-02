//////////////////////////////////////////////////////////////////////////////
//
// Workfile: road_diffuse_detail.fx
// Created by: Vano
//
// diffuse shader with detail texture addition for roads
//
// $Id: 
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Diffuse and detail texture
texture 		DiffMap0 : DIFFUSE_MAP_0;
texture			DetailMap: DETAIL_MAP_0;
texture			LightMap0: LIGHT_MAP_0;
shared const float4 g_Ambient		: LIGHT_AMBIENT	= { 0.2f, 0.2f, 0.2f, 1.0f };

// scale to calculate lightmap texcoords
const float2 lightmapScale: USER_FLOAT3_PARAM;

// Light direction( world space )
float3 LightDir			 : TMP_LIGHT0_DIR
<
	int Space = SPACE_OBJECT;
>;

// Diffuse color
shared const float2 g_FogTerm		: FOG_TERM	= { 1.0f, 800.0f };

// transformations
row_major float4x4 mFinal			 : TOTAL_MATRIX;
row_major float4x4 mWorld			 : WORLD_MATRIX;

// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER_CLAMP( DiffSampler, DiffMap0 )

// declare detail sampler
DECLARE_DETAIL_SAMPLER( DetailSampler, DetailMap )

// declare lightmap sampler
DECLARE_DETAIL_SAMPLER( LightmapSampler, LightMap0 )

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos		: POSITION;		// position in object space
	float3 Normal		: NORMAL;		// normal in object space
	float2 Tex0		: TEXCOORD0;		// diffuse texture texcoords
	float2 Tex1		: TEXCOORD1;		// detail texture texcoords
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		: POSITION;
	float2 Tex0		: TEXCOORD0;
	float2 Tex1		: TEXCOORD1;
	float2 Tex2		: TEXCOORD2;
	float  fog		: FOG;
};

/**
	diffuse + detail vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;

	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ) , mFinal );
	// Diffuse texture coordinate
	Out.Tex0	    = In.Tex0;
	// Detail texture coordinate
	Out.Tex1	    = In.Tex1;
	// Fog coeff
	Out.fog 	    = VertexFog( Out.Pos.z, g_FogTerm );
	// lightmap coords
	Out.Tex2 = mul( float4( In.Pos, 1 ), mWorld ).xz * lightmapScale;

	return Out;
}

/**
	diffuse + detail vertex shader for ps_2_0
 */
float4 PS11_DiffusePS( VS11_OUTPUT In ) : COLOR
{
	float4 color = tex2D( LightmapSampler, In.Tex2 ) * tex2D( DiffSampler, In.Tex0 ) * 2;
	// Full color
	return color * tex2D( DetailSampler, In.Tex1 ) * 2;
}

technique Test1
<
	string 	Description			= "diffuse + detail shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT2";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 PS11_DiffuseVS( );
		PixelShader		= compile ps_2_0 PS11_DiffusePS( );
	/*	asm 	
	{
	    ps_2_0
	    tex t0
	    tex t1
	    tex t2
	    add r0.xyz, t2, t2
	    mul r1, t1, t0
	    mul r0.xyz, r0, r1
	  + mov r0.w, r1.w
	};*/
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