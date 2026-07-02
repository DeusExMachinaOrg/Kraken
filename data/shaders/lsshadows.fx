//////////////////////////////////////////////////////////////////////////////
//
// Workfile: lsShadows.fx
// Created by: Vano
//
// shader for rendering shadows upon landscape and road
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Texture
texture 	DiffMap0		:	DIFFUSE_MAP_0; // Shadow texture
// Texcoord generating matrix
float4x4	TextureTransform	:	USER_FLOAT4x4_PARAM;
// Tfactor for color making
float4		Tfactor			:	USER_FLOAT4_PARAM		= { 0, 0, 0, 0 };
// Fade start
float		FadeStart		:	USER_FLOAT_PARAM		= { 0 };
// Fade end
float		FadeEnd			:	USER_FLOAT_PARAM2		= { 300 };
// Detail origin
float3		DetOrg			:	USER_FLOAT3_PARAM		= { 0, 0, 0 };
// Detail radius
float		DetRad			:	USER_FLOAT_PARAM3		= 25.f;
// Total transformation
float4x4	mFinal			:  	TOTAL_MATRIX;

// Viewer position in world space
float4		ViewPos			: 	VIEW_POS
<
	int 	Space		= SPACE_WORLD;	
	bool    Editable	= false;
>;

float3		initPos			:	USER_FLOAT3_PARAM2		= { 0, 0, 0 };

// Delta normal coeff
float dnc		= 0.0006;

// Diffuse sampler
DECLARE_SHADOW_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct vsInput
{
	float Pos	: POSITION;
	float2 AltPos : TEXCOORD0;
	//float3 Pos		: POSITION;		// Position in object space
	//float3 Normal	: NORMAL;		// Normal In Object space
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float2 uv0		: TEXCOORD0;
	float3 df		: TEXCOORD1;
};

//	Ls shadow vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out		= ( vsOutput )0;
	// Calculate distance to viewer
	float4 posInfo;
	int indX =  In.AltPos.x / 128;
	int indY =  In.AltPos.x - indX * 128;
	posInfo.x  = initPos.x + initPos.z * indX;
 	posInfo.z  = initPos.y + initPos.z * indY;
	posInfo.y  = In.Pos.x;
        posInfo.w  = 1;

	float dist			= distance( ViewPos,posInfo );

	// Position ( projected )
	Out.Pos				= mul( posInfo, mFinal );
	// Generate texcoords
	Out.uv0				= mul( posInfo, TextureTransform );
	// Calculate dist factor
	Out.df.x			= saturate( 1.f - ( dist - FadeStart ) / ( FadeEnd - FadeStart ) );
	// Calculate detalisation radius and detalization dist
	Out.df.y			= DetRad / 50.f;
	Out.df.z			= distance( DetOrg.xz, posInfo.xz ) / 50.f;
	
	return Out;
}

//	Ls shadow pixel shader
float4 SimplePS( vsOutput In ): COLOR
{
	// Get texture color
	float3	texColor	= tex2D( DiffSampler, In.uv0 );

	// Calculate final color
	float	finalColor = 1.0 - In.df.x * dot( texColor, Tfactor );
	
	// Return final color
	return  float4( finalColor, finalColor, finalColor, In.df.y >= In.df.z );
}

technique Test1
<
	string 	Description			= "road shadows shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 DiffuseVS();
		PixelShader		= compile ps_2_0 SimplePS();
	}
}