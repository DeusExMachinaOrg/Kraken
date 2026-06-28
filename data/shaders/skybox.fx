//////////////////////////////////////////////////////////////////////////////
//
// Workfile: skybox.fx
// Created by: S-Hunter
//
// per-vertex lighting + diffuse color + texture only
// Note: Diffuse color not works
//
// $Id: skybox.fx,v 1.8 2004/04/27 12:08:53 plus Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"
#include "data/shaders/ScatterLib.fx"

// textures
texture 		DiffMap0		:	DIFFUSE_MAP_0;

// viewer position (world coords)
float4			ViewPos: VIEW_POS;

// light directions (world space)
//float3 DirFromLight	: TMP_LIGHT0_DIR;

static float fFogStart = 650.f;
static float fFogEnd = 1000.f;
static float fFogDensity = 0.02;




// transformations
row_major float4x4 mFinal		: TOTAL_MATRIX;


// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )


// vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// position in object space
	float3 Normal	: NORMAL;		// normal in object space
	float4 Diff		: COLOR0;		// diffuse color
	float2 Tex0		: TEXCOORD0;	// diffuse texcoords
};


// vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float4 Diff		: COLOR0;
	float2 uv0		: TEXCOORD0;
};


/**
	Simple Phong specular vertex shader
	
	TODO: transform light direction to object space before shader
 */
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out = (vsOutput)0;

	// position (projected)
	Out.Pos = mul( float4(In.Pos, 1), mFinal );
	// texcoords
	Out.uv0 = In.Tex0;

	// directional light to object space
	// calc diffuse: diffuse_light * N.L
	//Out.Diff = 2.0 * In.Diff * dot( In.Normal, DirFromLight );
	//float3 ModelLightPos = mul( LightPos , mFinal );
	//float3 ModelLightPos = LightPos;
	//float3 ModelLightPos = DirFromLight;
	/*float3 ModelLightPos;
	ModelLightPos.x = -DirFromLight.x;
	ModelLightPos.y = -DirFromLight.y;
	ModelLightPos.z = DirFromLight.z;*/
	//float3 ModelLightPos = mul( -DirFromLight, InvWorldMat );

	//float3 worldPos = In.Pos;

    //float eyeDist = distance( ViewPos, worldPos);
	//Out.fog.x = CalcFog( /*eyeDist*/ Out.Pos.z );

	vsSky( In.Pos, -DirFromLight /*ModelLightPos*/, Out.Diff );

	return Out;
}


/**
	Simple pixel shader
 */
float4 SimplePS( vsOutput In ): COLOR
{
	float4	tDiff = tex2D( DiffSampler, In.uv0 );

    float4	color = In.Diff;
	//return tDiff;
    return float4( lerp( tDiff, color, color.a ));
}


technique ForPS11FullDetail
<
	string 	VertexFormat = "VERTEX_XYZCT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader = compile vs_2_0 DiffuseVS();
		PixelShader = compile ps_2_0 SimplePS();
		//PixelShader = NULL;
		//VertexShader = NULL;

		AlphaBlendEnable = false;
		/*SrcBlend = SrcAlpha;
		DestBlend = InvSrcAlpha;*/


		CullMode = NONE;

		SpecularEnable = false;
		
		//FillMode = WireFrame;
		FillMode = Solid;

		FogEnable = false;

		// Set up texture stage 0
        	/*ColorOp[0] = Modulate;
        	ColorArg1[0] = Texture;
        	ColorArg2[0] = Diffuse;
        	AlphaOp[0] = Modulate;
        	AlphaArg1[0] = Texture;
        	AlphaArg2[0] = Diffuse;
        
        	// Disable texture stage 1
        	ColorOp[1] = Disable;
        	AlphaOp[1] = Disable;*/
	}
}