//////////////////////////////////////////////////////////////////////////////
//
// Workfile: specular_cubemap.fx
// Created by: Plus
//
// simple per-vertex environment mapping + specular hightlight shader
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"

// textures
texture 		DiffMap0		:	DIFFUSE_MAP_0;
texture			CubeMap0		:	CUBE_MAP_0;

// light vector
float3          LightPos = { 0, -1, 1 };

float4			DiffuseColor = { 1, 1, 1, 1 };
float4			SpecularColor = { 1, 1, 1, 1 };

// viewer position (world coords)
float4			ViewPos: VIEW_POS;

// viewer position (object space)
float4			objectViewPos: VIEW_POS_OBJECT;

float			Time: TIME_LINEAR;


// light props
static	float4	LightDiffuseColor        = { 1.0f, 0.9f, 0.8f, 1.0f };		// diffuse
static	float4	LightSpecularColor       = { 1.0f, 1.0f, 1.0f, 1.0f };		// specular

// material props
static	float4	MaterialSpecularColor    = { 0.2f, 0.2f, 0.2f, 1.0f };		// specular
static	int		MaterialSpecularPower    = 32;								// power


// light directions (world space)
//float3 DirFromLight = { 0, -1, 1 };
float3 DirFromLight	: TMP_LIGHT0_DIR;



// transformations
row_major float4x4 	mFinal		: TOTAL_MATRIX;
float4x4 			mWorld		: WORLD_MATRIX;
float4x4 			mInvWorld	: INV_WORLD_MATRIX;


// declare base diffuse sampler
#include "data/shaders/diffsampler.fx"

// declare envmap sampler
#include "data/shaders/envsampler.fx"


// vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// position in object space
	float3 Normal	: NORMAL;		// normal in object space
	float2 Tex0		: TEXCOORD0;	// diffuse/bump texcoords
};


// vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float4 Diff		: COLOR0;
	float4 Spec		: COLOR1;
	float2 uv0		: TEXCOORD0;
	float3 CubeUV	: TEXCOORD1;
};


/**
	Simple Phong specular vertex shader
	
	TODO: transform light direction to object space before shader
 */
vsOutput DiffuseSpecularVS( vsInput In )
{
	vsOutput Out = (vsOutput)0;

	// position (projected)
	Out.Pos = mul( float4(In.Pos, 1), mFinal );
	// texcoords
	Out.uv0 = In.Tex0;

	// directional light to object space
	float3 objectLight = -normalize( mul(DirFromLight, (float3x3)mInvWorld) );

	// calc diffuse: diffuse_light * N.L
	Out.Diff = DiffuseColor * max( 0, dot( In.Normal, objectLight ) );
	
	// calculate view vector in object space
	float3 objectViewDir = normalize( objectViewPos - In.Pos );
	
	// calculate environment map texcoords
	float3 R = normalize( reflect( objectViewDir, In.Normal ) );
	Out.CubeUV = R;
	
	// calc specular
    Out.Spec = LightSpecularColor * pow( max( 0, dot( R, -objectViewDir ) ), MaterialSpecularPower/4 );
    
    // fuck off finally...
	return Out;
}


/**
	Simple pixel shader
 */
float4 SimplePS( vsOutput In ): COLOR
{
	// fetch diffuse texture
	float4	tDiff = tex2D( DiffSampler, In.uv0 );
	
	// fetch em texture
	float4	tEM = tex3D( EnvSampler, In.CubeUV );
	
	// final diffuse color
	float3	Diff = In.Diff.xyz * tDiff.xyz;
	//Diff = float3(0, 0, 0);
	
	// final color
	return float4( lerp( Diff, tEM.xyz, 0.25 )/* + In.Spec.xyz*/, tDiff.a );
	//return float4( Diff/* + In.Spec.xyz*/, tDiff.a );
	//return float4(1, 0, 0, 1);
}


technique ForPS11FullDetail
{
	pass P1
	{
		VertexShader = compile vs_2_0 DiffuseSpecularVS();
		PixelShader = compile ps_2_0 SimplePS();


		//CullMode = CCW;

		//SpecularEnable = false;
		
		//FillMode = WireFrame;
		//FillMode = Solid;

		//FogEnable = false;
	}
}