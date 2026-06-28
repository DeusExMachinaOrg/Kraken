//////////////////////////////////////////////////////////////////////////////
//
// Workfile: roadLight.fx
// Created by: Vano
//
// shader for rendering point lights upon landscape and road
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// Light position
float3		LightPos		:	USER_FLOAT4_PARAM;
// Light radius
float		LightRadius		:	USER_FLOAT_PARAM;
// Light color
float3		LightColor		:	USER_FLOAT3_PARAM;
// Light fade end
float		FadeEnd			:	USER_FLOAT_PARAM2;
// Total transformation
row_major float4x4 	mFinal	:	TOTAL_MATRIX;
// World matrix
//float4x4 	mWorld			:	 WORLD_MATRIX;
// Viewer position in world space
float4		ViewPos			:	VIEW_POS
<
	//int 	Space		= SPACE_WORLD;
	int 	Space		= SPACE_OBJECT;	
	bool    Editable	= false;
>;
// 
shared const float3  g_BendTerm  : TREE_BEND_TERM;
// Delta normal coeff
float dnc  = 0.00008;		

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
	float2 uv0		: TEXCOORD0;
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
	//float3 worldPos	= mul( float4( In.Pos, 1 ), mWorld );
	// Calculate world normal
	//float3 worldNormal	= mul( float4( In.Normal, 1 ), mWorld ) - mul( float4( 0, 0, 0, 1 ), mWorld );
	// Calculate texccord for diffuse texture
	Out.uv0				= In.uv0;
	// Calculate direction
	float3 dir			= LightPos - In.Pos /*worldPos*/;	
	// Calculate dot product
	float  dp			= dot( /*worldNormal*/ In.Normal, normalize( dir ) );
	if( dp > 0 )
	{
		// Calculate dist factor
		float  lightDist		= length( dir );
		float  lightFadeStart	= LightRadius * 0.5;
		float  df				= saturate( 1.f - ( lightDist - lightFadeStart ) / ( LightRadius - lightFadeStart ) );
		
		// Fade light in far
		float  FadeStart		= FadeEnd * 0.7;
		float  ff				= saturate( 1.f - ( dist - FadeStart ) / ( FadeEnd - FadeStart ) );
		
		// Calculate color according to angle
		float	c				= saturate( ff * df * pow( dp, 0.3 ) ); 
		Out.clr					= LightColor * c;
	}
	else
	{
		Out.clr					= float3( 0, 0, 0 );
	} 

	return Out;
}

technique Test1
<
	string 	Description			= "point light shader";
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