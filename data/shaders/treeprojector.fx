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
// Total transformation
row_major float4x4 	mFinal	:	TOTAL_MATRIX;
// World matrix
//float4x4 	mWorld		:	 WORLD_MATRIX;

float		Intensity	:	USER_FLOAT_PARAM3;
// Viewer position in world space
float4		ViewPos		:	VIEW_POS
<
	//int 	Space		= SPACE_WORLD;
	int 	Space		= SPACE_OBJECT;	
	bool    Editable	= false;
>;
// Time 
shared const float3  g_BendTerm  : TREE_BEND_TERM;
// Delta normal coeff
float dnc  = 0.00008;		

// Projector sampler
//DECLARE_PROJECTOR_SAMPLER( ProjectorSampler, ProjectorMap )

// Vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// Position in object space
	float3 Normal	: NORMAL;		// Normal In Object space
	float2 uv0		: TEXCOORD0;	// Diffuse texture texcoord
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float4 uv0		: TEXCOORD0;
	float2 uv1		: TEXCOORD1;
	float3 clr		: COLOR0;
};

// projector vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out		= ( vsOutput )0;
	// Shake a little
	if( In.Pos.y >= 0.0f )
	{
		In.Pos.xz += g_BendTerm.xy * In.Pos.y;
	}
	// Calculate distance to viewer
	float dist			= distance( ViewPos, In.Pos );
	// Calculate delta normal
	//float deltaNormal	= dist * dnc;
	//clamp( deltaNormal, 0.0001, 0.1 );
	// Pos with normal addition in object space
	//Out.Pos				= float4( In.Pos + In.Normal * deltaNormal, 1 );
	Out.Pos				= float4( In.Pos, 1 );
	// Position ( projected )
	Out.Pos				= mul( Out.Pos, mFinal );
	// Calculate world position
	//float3 worldPos		= mul( float4( In.Pos, 1 ), mWorld );
	// Calculate world normal
	//float3 worldNormal	= mul( float4( In.Normal, 1 ), mWorld ) - mul( float4( 0, 0, 0, 1 ), mWorld );
	// Generate texcoords
	Out.uv0				= mul( float4( /*worldPos*/ In.Pos, 1 ), TextureTransform );
	// Calculate texccord for diffuse texture
	Out.uv1				= In.uv0;
	// Calculate vertex color
	if( Out.uv0.z <= 0 )
	{
		Out.clr			= float3( 0, 0, 0 );
	} 
	else
	{
		// Calculate dot product
		float dp		= dot( /*worldNormal*/ In.Normal, ProjectorDir );
	
		if( dp > 0 )
		{
			// Calculate dist factor
			float  projDist		 = distance( ProjectorPos, /*worldPos*/ In.Pos );
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