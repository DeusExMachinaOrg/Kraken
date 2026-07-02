//////////////////////////////////////////////////////////////////////////////
//
// Workfile: lsShadows.fx
// Created by: Vano
//
// shader for rendering shadows upon landscape and road
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"

// Texture
texture 	DiffMap0		:	DIFFUSE_MAP_0; // Shadow texture
// Texcoord generating matrix
float4x4	TextureTransform:	USER_FLOAT4x4_PARAM;
// Tfactor for color making
float4		Tfactor			:	USER_FLOAT4_PARAM		= { 0, 0, 0, 0 };
// Detail origin
float3		DetOrg			:	USER_FLOAT3_PARAM		= { 0, 0, 0 };
// Detail radius
float		DetRad			:	USER_FLOAT_PARAM3		= 25.f;
// Total transformation
row_major float4x4 	mFinal	:	TOTAL_MATRIX;
// Viewer position in world space
float4		ViewPos			: VIEW_POS
<
	int 	Space		= SPACE_WORLD;	
	bool    Editable	= false;
>;
// Delta normal coeff
float dnc  = 0.0006;	

// Diffuse sampler
DECLARE_SHADOW_SAMPLER( DiffSampler, DiffMap0 )

// Vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// Position in object space
	float3 Normal	: NORMAL;		// Normal In Object space
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float2 uv0		: TEXCOORD0;
	float3  df		: TEXCOORD1;
};

//	Ls shadow vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out		= ( vsOutput )0;
	// Calculate distance to viewer
	//float dist			= distance( ViewPos, In.Pos );
	// Calculate delta normal
	//float deltaNormal	= dist * dnc;
	//clamp( deltaNormal, 0.0001, 0.1 );
	// Pos with normal addition in object space
	//Out.Pos				= float4( In.Pos + In.Normal * deltaNormal, 1 );
	Out.Pos				= float4( In.Pos, 1 );
	// Position ( projected )
	Out.Pos				= mul( Out.Pos, mFinal );
	// Generate texcoords
	Out.uv0				= mul( float4( In.Pos, 1 ), TextureTransform );
	// Calculate detalisation radius and detalization dist
	Out.df.y			= DetRad / 50.f;
	Out.df.z			= distance( DetOrg.xz, In.Pos.xz ) / 50.f;
	
	return Out;
}

//	Ls shadow pixel shader
float4 SimplePS( vsOutput In ): COLOR
{
	// Get texture color
	float3	texColor = tex2D( DiffSampler, In.uv0 );

	// Calculate final color
	float	finalColor = 1.0 - dot( texColor, Tfactor );
	
	// Return final color
	return  float4( finalColor, finalColor, finalColor, In.df.y < In.df.z  );	
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