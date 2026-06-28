//////////////////////////////////////////////////////////////////////////////
//
// Workfile: contour.fx
// Created by: Vano
//
// shader for rendering contours of objects
//
// $Id: contour.fx,v 1.2 2005/07/20 07:14:49 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// transformations
row_major float4x4	mFinal	: TOTAL_MATRIX;
// ContourWidth
float	contourWidth		: USER_FLOAT_PARAM = 0.005f;

// Vertex shader input structure
struct VS_INPUT
{
	float3 Pos	     : POSITION;		// position in object space
	float3 Normal	 : NORMAL;			// normal in object space
//	float2 Tex0	     : TEXCOORD0;		// diffuse texcoords
};

// Vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos		     : POSITION;
//	float2 Tex0		     : TEXCOORD0;
};

/**
	Simple diffuse vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_DiffuseVS( VS_INPUT In )
{
	VS11_OUTPUT Out = ( VS11_OUTPUT )0;
// Contour width doesn't depend on distance	
/*
	// Calculate bear-out direction
	float3 dirOut	= normalize( In.Pos );
	float3 n		= normalize( dirOut * 4.f + In.Normal );
	if( dot( dirOut, In.Normal ) <= 0.f )
		n = In.Normal;
	// Add bear-out
	In.Pos += n * contourWidth;
	// Position ( projected )
	Out.Pos         = mul( float4( In.Pos, 1 ) , mFinal );
	// Texture coordinate
	//Out.Tex0	    = In.Tex0;
*/

// Contour width depends on distance	
	// Calculate bear-out direction
	float3 dirOut	= normalize( In.Pos );
	float3 n		= normalize( dirOut * 4.f + In.Normal );
	if( dot( dirOut, In.Normal ) <= 0.f )
		n = In.Normal;
	// Calculate shifted position
	float4  shiftedPos = mul( float4( In.Pos + n, 1 ) , mFinal );	
	// Calculate not shifted position
	float4 pos = mul( float4( In.Pos, 1 ) , mFinal );
	// Calculate direction in projected space
	float2 dir = normalize( shiftedPos - pos ) * pos.z * contourWidth;
	// Position ( projected )
	Out.Pos = pos + float4( dir, 0, 0 );
	
	return Out;
}

technique Test1
<
	string 	Description = "contour shader";
	bool   	ComputeTangentSpace = false;
	string 	VertexFormat = "VERTEX_XYZNT1";
	bool	Default = true;
>
{
	pass P1
	{
		VertexShader	= compile vs_2_0 PS11_DiffuseVS();
		PixelShader		= NULL;
	}
}