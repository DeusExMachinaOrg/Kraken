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
row_major float4x4 	mFinal	:	TOTAL_MATRIX;
// Viewer position in world space
float4		ViewPos			:	VIEW_POS
<
	//int 	Space		= SPACE_WORLD;
	int 	Space		= SPACE_OBJECT;	
	bool    Editable	= false;
>;
// Delta normal coeff
float dnc  = 0.001;		

// Projector sampler
//DECLARE_PROJECTOR_SAMPLER( ProjectorSampler, ProjectorMap )

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
	float4 uv0		: TEXCOORD0;
	float3 clr		: COLOR0;
};

// projector vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out		= ( vsOutput )0;
	// Calculate distance to viewer
	float dist			= distance( ViewPos, In.Pos );
	// Calculate delta normal
	//float deltaNormal	= dist * dnc;
	//clamp( deltaNormal, 0.001, 0.1 );
	// Pos with normal addition in object space
	//Out.Pos				= float4( In.Pos + In.Normal * deltaNormal, 1 );
	Out.Pos				= float4( In.Pos, 1 );
	// Position ( projected )
	Out.Pos				= mul( Out.Pos, mFinal );
	// Generate texcoords
	Out.uv0				= mul( float4( In.Pos, 1 ), TextureTransform );
	// Calculate vertex color
	if( Out.uv0.z <= 0 )
	{
		Out.clr			= float3( 0, 0, 0 );
	} 
	else
	{
		// Calculate dot product
		float   dp		= dot( In.Normal, ProjectorDir );
	
		if( dp > 0 )
		{
			// Calculate dist factor
			float  projDist		 = distance( ProjectorPos, In.Pos );
			float  projFadeStart = ProjectorRadius * 0.5;
			float  df			 = saturate( 1.f - ( projDist - projFadeStart ) / ( ProjectorRadius - projFadeStart ) );
		
			// Fade projector in far
			float  FadeStart	 = FadeEnd * 0.7;
			float  ff			 = saturate( 1.f - ( dist - FadeStart ) / ( FadeEnd - FadeStart ) );
		
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

/*
//	projector pixel shader
float4 SimplePS( vsOutput In ): COLOR
{
	// Get texture color
	float4	texColor = tex2Dproj( DiffSampler, In.uv0 );
	// Return final color
	return  texColor;
}
*/

float4 SimplePS( vsOutput In ): COLOR
{
	return  float4( 1, 0, 0, 1 );
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
		//PixelShader					= compile ps_2_0  SimplePS();	
	}
}