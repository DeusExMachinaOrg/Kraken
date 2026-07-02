//////////////////////////////////////////////////////////////////////////////
//
// Workfile: specular_cubemap.fx
// Created by: S-Hunter
//
// per-vertex lighting + diffuse color + texture only
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"
//#include "data/shaders/ScatterLib.fx"

// textures
texture 		DiffMap0		:	DIFFUSE_MAP_0;

float4x4		TextureTransform: USER_FLOAT4x4_PARAM;
float4			Tfactor: USER_FLOAT4_PARAM;
//float4			Tfactor = { 1.0,-.74,1.0,0.0 };
//float3			Tfactor = { 0,0,1.0 };

// World matrix
float4x4 			InvWorldMat : INV_WORLD_MATRIX;
float4x4 			WorldMat : WORLD_MATRIX;

// light directions (world space)
//float3 DirFromLight = { 0, -1, 1 };
//float3 DirFromLight = { 1, 1, 1 };
//float3 DirFromLight	: TMP_LIGHT0_DIR;

float distFactor : USER_FLOAT_PARAM;

// transformations
float4x4 mFinal		: TOTAL_MATRIX;


#include "data/shaders/rtsampler.fx"

// vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// position in object space
//	float4 Diff		: COLOR0;		// diffuse color
};


// vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
//	float4 Diff		: COLOR0;
	float2 uv0		: TEXCOORD0;
};


/**
	Shader special shader.
	
 */
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out = (vsOutput)0;

	// position (projected)
	Out.Pos = mul( float4(In.Pos, 1), mFinal );
	// Generate texcoords
	Out.uv0 = mul( float4(In.Pos, 1), mul( WorldMat, TextureTransform ) );

	//Out.Diff = In.Diff;
	/*if( In.Diff.w != 0.0f ) 
		Out.Diff.x = 0.4;*/
	



	
    // fuck off finally...
	return Out;
}


/**
	We use tfactor as mask for correct dp operation
 */
float4 SimplePS( vsOutput In ): COLOR
{
	// fetch texture
	float4	texColor = tex2D( DiffSampler, In.uv0 );
	//texColor.w = 1.f;
	//float3	tfac = float3( Tfactor.x, Tfactor.y, Tfactor.z );
	//float4	tDiff = ( texColor, 1.f );
	float4 Tfac = Tfactor; Tfac.w = 0.f;
	// Todo: revert signs
	float  finalColor = 1.0 + dot( texColor, Tfac ) * ( distFactor );// + In.Diff;
	//finalColor = 1.f - finalColor;
	return  float4( finalColor, finalColor, finalColor, 1);
	//tDiff = float4(0.5,0.5,0.5,0.5);	
	//tDiff = float4(1.0,1.0,1.0,0.0);
	//return dot( tDiff, Tfactor );
	//return float4( Tfactor, 1 );
	//return tDiff;
	//return float4(1,0,0,1);
}


technique ForPS11LsShadows
{
	pass P1
	{
		VertexShader = compile vs_2_0 DiffuseVS();
		PixelShader = compile ps_2_0 SimplePS();
		//VertexShader = NULL;
		//PixelShader = NULL;

		AlphaBlendEnable = true;
		//AlphaBlendEnable = false;
		SrcBlend = DestColor;
		DestBlend = Zero;
		/*SrcBlend = SrcColor;
		DestBlend = DestColor;*/
		/*SrcBlend = Zero;
		DestBlend = DestColor;*/


		CullMode = CW;

		SpecularEnable = false;
		//FillMode = WireFrame;
		FillMode = Solid;

		FogEnable = false;

		// Set up texture stage 0
        /*	ColorOp[0] = DotProduct3;
        	ColorArg1[0] = Texture;
        	ColorArg2[0] = TFactor;
        	AlphaOp[0] = Disable;
        	// Disable texture stage 1
        	ColorOp[1] = Disable;
        	AlphaOp[1] = Disable;*/
	}
}