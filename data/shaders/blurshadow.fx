//////////////////////////////////////////////////////////////////////////////
//
// Workfile: blurShadow.fx
// Created by: Vano
//
// shader for bluring shadow
//
//////////////////////////////////////////////////////////////////////////////

#include "data/shaders/lib.fx"

// Texture
texture 	ShadowMap0		:	DIFFUSE_MAP_0; // Shadow texture
// TexCoord scale
float		TexCoordScale	:	USER_FLOAT_PARAM = 0.001f;

// Shadow sampler
DECLARE_SHADOW_SAMPLER( ShadowSampler, ShadowMap0 )

// Vertex shader input structure
struct vsInput
{
	float3 Pos		: POSITION;		// Position in object space
	float2 uv0		: TexCOORD0;    // Texture coord
};

// Vertex shader output structure
struct vsOutput
{
	float4 Pos		: POSITION;
	float2 Uvs[ 4 ]	: TEXCOORD0;
};

//	Blur shadow vertex shader
vsOutput DiffuseVS( vsInput In )
{
	vsOutput Out	= ( vsOutput )0;
	// Pos with normal addition in object space
	Out.Pos			= float4( In.Pos, 1 );
	// Set texcoords with neighbours
	Out.Uvs[ 0 ]	= In.uv0 + float2(  0,  1 ) * TexCoordScale;
	Out.Uvs[ 1 ]	= In.uv0 + float2(  1,  0 ) * TexCoordScale;
	Out.Uvs[ 2 ]	= In.uv0 + float2(  0, -1 ) * TexCoordScale;
	Out.Uvs[ 3 ]	= In.uv0 + float2( -1,  0 ) * TexCoordScale;
	
	return Out;
}

//	Road shadow pixel shader
float4 SimplePS( vsOutput In ): COLOR
{
	float4	texColor	 = tex2D( ShadowSampler, In.Uvs[ 0 ] ) * 0.25;
	texColor			+= tex2D( ShadowSampler, In.Uvs[ 1 ] ) * 0.25;
	texColor			+= tex2D( ShadowSampler, In.Uvs[ 2 ] ) * 0.25;
	texColor			+= tex2D( ShadowSampler, In.Uvs[ 3 ] ) * 0.25;
	
	return texColor;  
}

technique Test1
<
	string 	Description			= "blur shadow shader";
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