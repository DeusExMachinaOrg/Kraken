//////////////////////////////////////////////////////////////////////////////
//
// Workfile: roadProjector.fx
// Created by: Vano
//
// shader for rendering projectors upon landscape and road
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Texture
//texture 	ProjectorMap	:	DIFFUSE_MAP_0; // Projector texture
// Texcoord generating matrix
float4x4	TextureTransform:	USER_FLOAT4x4_PARAM;
// Projector position
float3		ProjectorPos	:	USER_FLOAT4_PARAM;
// Projector direction
float3		ProjectorDir	:	USER_FLOAT3_PARAM;
// Projector radius
float		ProjectorRadius :	USER_FLOAT_PARAM;
// Projector fade end
float		FadeEnd		:	USER_FLOAT_PARAM2;

float		Intensity	:	USER_FLOAT_PARAM3;
// Total transformation
float4x4 	mFinal		:	TOTAL_MATRIX;
// Viewer position in world space
float4		ViewPos		:	VIEW_POS
<
	//int 	Space		= SPACE_WORLD;
	int 	Space		= SPACE_OBJECT;	
	bool    Editable	= false;
>;

float3		initPos			:	USER_FLOAT3_PARAM2		= { 0, 0, 0 };

// Delta normal coeff
float dnc  = 0.001;

// Projector sampler
//DECLARE_PROJECTOR_SAMPLER( ProjectorSampler, ProjectorMap )

// Vertex shader input structure
struct vsInput
{
	float Pos	: POSITION;
	int4 AltPos 	: TEXCOORD0;
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float4 uv0		: TEXCOORD0;
	float3 clr		: COLOR0;
};

// projector vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out		= ( vsOutput )0;
	// Calculate distance to viewer
	float4 pos;
	int indX =  In.AltPos.x / 128;
	int indY =  In.AltPos.x - indX * 128;
	pos.x  = initPos.x + initPos.z * indX;
 	pos.z  = initPos.y + initPos.z * indY;
	pos.y  = In.Pos.x;
        pos.w  = 1;

	float3 normal;

	normal = float3( In.AltPos.y, In.AltPos.z, In.AltPos.w ) / 1024;

	// Calculate distance to viewer
	float dist		= distance( ViewPos, pos );

	// Position ( projected )
	Out.Pos				= mul( pos, mFinal );
	// Generate texcoords
	Out.uv0				= mul( pos, TextureTransform );
	// Calculate vertex color
	if( Out.uv0.z <= 0 )
	{
		Out.clr			= float3( 0, 0, 0 );
	} 
	else
	{
		// Calculate dot product
		float   dp		= dot( normal, ProjectorDir );
	
		if( dp > 0 )
		{
			// Calculate dist factor
			float  projDist		 = distance( ProjectorPos, pos );
			float  projFadeStart 	= ProjectorRadius * 0.5;
			float  df		= saturate( 1.f - ( projDist - projFadeStart ) / ( ProjectorRadius - projFadeStart ) );
		
			// Fade projector in far
			float  FadeStart	= FadeEnd * 0.7;
			float  ff		= saturate( 1.f - ( dist - FadeStart ) / ( FadeEnd - FadeStart ) );
		
			// Calculate color according to angle
			float	c		= saturate( ff * df * /*sqrt( dp )*/ pow( dp, 0.3 /* 0.4*/ ) ) * Intensity; 
			Out.clr			= float3( c, c, c );
		}
		else
		{
			Out.clr			= float3( 0, 0, 0 );
		}
	} 
	
	return Out;
}

technique Test1
<
	string 	Description			= "projector shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat		= "VERTEX_XYZNT1";
	bool	Default				= true;
>
{
	pass P1
	{
		VertexShader				= compile vs_2_0 DiffuseVS();
		PixelShader					= NULL;	
	}
}