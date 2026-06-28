//////////////////////////////////////////////////////////////////////////////
//
// Workfile: specular_cubemap.fx
// Created by: S-Hunter
//
// per-vertex lighting + diffuse color + texture only
//
// $Id: ls_color.fx,v 1.6 2004/04/21 11:21:45 plus Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"
#include "data/shaders/ScatterLib.fx"

// textures
texture 		DiffMap0		:	DIFFUSE_MAP_0;

// light vector
static float3          LightPos = { .5, .3, .21 };
static float3		   ModelEyePos = { 0, 0, 0 };


// viewer position (world coords)
float4			ViewPos: VIEW_POS;

// light directions (world space)
//float3 DirFromLight = { 0, -1, 1 };
//float3 DirFromLight = { 1, 1, 1 };
//float3 DirFromLight	: TMP_LIGHT0_DIR;

static float fFogStart = 650.f;
static float fFogEnd = 1000.f;
static float fFogDensity = 0.02;




// transformations
float4x4 mFinal		: TOTAL_MATRIX;


// declare base diffuse sampler
DECLARE_DIFFUSE_SAMPLER( DiffSampler, DiffMap0 )


// vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// position in object space
	float3 Normal	: NORMAL;		// normal in object space
	float4 Diff		: COLOR0;		// diffuse color
	float2 Tex0		: TEXCOORD0;	// alpha texcoords
	float2 Tex1		: TEXCOORD1;	// diffuse texcoords
};


// vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float4 Diff		: COLOR0;
	float2 uv0		: TEXCOORD0;
    float3 F_ex 	: TEXCOORD1;            // light outscatter coefficient
    float3 L_in 	: TEXCOORD2;            // light inscatter coefficient
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
	Out.uv0 = In.Tex1;

	// directional light to object space
	// calc diffuse: diffuse_light * N.L
	Out.Diff = 2 * In.Diff * dot( In.Normal, -DirFromLight );
	//Out.Diff = float4 ( 1,1,1,1);

    //float eyeDist = distance( ViewPos, In.Pos);

	Out.Diff.a = CalcFog( Out.Pos.z );


    vsAthmoFog( In.Pos, ViewPos, -DirFromLight, Out.L_in, Out.F_ex );
    Out.F_ex *= saturate(lerp(SunColor, 1.0, 0.3));

	
    // fuck off finally...
	return Out;
}


/**
	Simple pixel shader
 */
float4 SimplePS( vsOutput In ): COLOR
{
	//return float4(1, 0, 0, 1);
	//return float4(1, 0, 0, 0);

	// fetch diffuse texture
	float4	tDiff = tex2D( DiffSampler, In.uv0 );
	//tDiff = float4( 1, 0,0,0);
	// float alpha = 1.0f;
	// float4  lightcomp = float4(1, 1, 1, 0) * dot( In.Normal, LightPos );	
	// final color

	//float d = length( In.FakePos );
	/*float d = In.FakePos.z;
	float alpha = saturate((fFogEnd - d) / ( fFogEnd -  fFogStart));*/
	//float alpha = ((fFogEnd - d) / ( fFogEnd -  fFogStart));

	//return float4( tDiff.xyz * In.Diff.xyz, alpha );
	tDiff = tDiff * In.Diff * In.Diff.a;
	//return tDiff;	
	return psAthmoFog( In.L_in, In.F_ex, tDiff );

	//return tDiff * In.Diff;
	//return float4(1, 0, 0, 1);
}


technique ForPS11FullDetail
{
	pass P1
	{
		VertexShader = compile vs_2_0 DiffuseVS();
		//VertexShader = NULL;
		PixelShader = compile ps_2_0 SimplePS();
		//PixelShader = NULL;

		AlphaBlendEnable = False;
		/*SrcBlend = SrcAlpha;
		DestBlend = InvSrcAlpha;*/


		CullMode = CW;

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