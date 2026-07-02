//////////////////////////////////////////////////////////////////////////////
//
// Workfile: bump_vc.fx
// Created by: Vano
//
// simple diffuse bump mapping + per-vertex specular hightlight shader + vertex coloring
// final_color = diffuse_color * (bumpmap_normal dot3 light) + vertex_specular * bumpmap_alpha
//
// $Id: bump_vc.fx,v 1.17 2005/09/06 10:56:14 vano Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"

// textures
texture         texDiffuse  :   DIFFUSE_MAP_0;
texture         texBumpGloss    :   BUMP_MAP_0;


// viewer position (object space)
float4          objectViewPos   : VIEW_POS
<
	int  Space    = SPACE_OBJECT;
	bool Editable = false;
>;


// light directions (object space)
float3 DirFromLight: TMP_LIGHT0_DIR
<
    int Space = SPACE_OBJECT;
>;


// transformations
row_major float4x4 mFinal                 : TOTAL_MATRIX; 

shared const float4 g_Ambient		: LIGHT_AMBIENT 	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE 	= { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_Specular		: LIGHT_SPECULAR 	= { 1.0f, 1.0f, 1.0f };
shared const float2 g_FogTerm		: FOG_TERM 			= { 1.0f, 800.0f };
shared const float  g_Transparency	: TRANSPARENCY		= 1.f;


// declare samplers
DECLARE_DIFFUSE_SAMPLER( DiffSampler, texDiffuse )
DECLARE_BUMP_SAMPLER( BumpSampler, texBumpGloss )



// vertex shader input structure
struct VS_INPUT
{
    float3  Pos     : POSITION; // position in object space
    float3 Normal   : NORMAL;   // normal in object space
    float4 Tangent	: TANGENT;	// tangent in object space with a sign of binormal as a w component
    float2 Tex0     : TEXCOORD0;// diffuse/bump texcoords
    float4 VertClr  : COLOR0;   // vertex color
};

// vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
    float4 Pos      : POSITION;
    float2 uv0      : TEXCOORD0;
    float2 uv0Diff  : TEXCOORD1;
    float3 Light    : COLOR1;   
    float3 Half     : TEXCOORD3;
    float  fog      : FOG;
    float4 VertClr  : COLOR0;
};


/**
    Blinn-Phong simple vertex shader for ps_2_0
 */
VS11_OUTPUT PS11_BumpBlinnDiffuseSpecularVS( VS_INPUT v )
{
    VS11_OUTPUT o = (VS11_OUTPUT)0;

    // position (projected)
    o.Pos     = mul( float4( v.Pos, 1) , mFinal );
    // texcoords
    o.uv0     = v.Tex0;
    o.uv0Diff = v.Tex0;
    // fog coeff
    o.fog     = VertexFog( o.Pos.z, g_FogTerm );
    // vertex color
    o.VertClr =  float4( v.VertClr.xyz, g_Transparency );     

    // light direction should come already normalized from the engine...
    float3 objectLight = -DirFromLight;
    
    // calculate binormal
	float3 Binormal = cross( v.Normal, v.Tangent ) * v.Tangent.w; 

    // calculate light vector in texture space
    float3 tangentLight   = toTangentSpace( objectLight, v.Tangent, Binormal, v.Normal );

    // output light vector in tangent space
    o.Light = 0.5f + 0.5f * tangentLight;
    //Out.Light           = tangentLight;

    // calculate view vector in object space
    float3 objectViewDir  = objectViewPos - v.Pos;
                              
    // calculate view vector in tangent space
    float3 tangentViewDir = toTangentSpace( objectViewDir, v.Tangent, Binormal, v.Normal );
    
    // calculate half vector in tangent space
    o.Half              = 0.5f + 0.5f * normalize( tangentViewDir + tangentLight );

    return o;
}


/**
    Blinn-Phong simple pixel shader for ps_2_0

    TODO: renormalize interpolated vectors via normalizing cubemap
    TODO: replace multiplications w/ specular power texture lookup
 */
float4 PS11_BumpBlinnDiffuseSpecularPS( VS11_OUTPUT i, uniform float4 ambient, uniform float4 diffuse ): COLOR
{
    // fetch normal + gloss factor from texture
    float4  texBump      = tex2D( BumpSampler, i.uv0 );

    // fetch diffuse color + alpha factor from texture
    float4 texDiffuse    = tex2D( DiffSampler, i.uv0Diff );

    // normal (unpack from texture)
    float3  Normal       = bx2( texBump );

    // light
    float3 Light         = bx2( i.Light );

    // dot product of light with bump
    float cosA           = saturate( dot( Normal, Light ) );

    // cosine between half and normal
    float cosB           = saturate( dot( bx2(i.Half), Normal ) );      

    //
    //float3 Spec        = pow16( cosB*cosB ) * texBump.a * cosA * g_Specular;
	float3 Spec          = pow16( cosB ) * texBump.a * cosA * g_Specular;	

    //
    return float4( ( ambient + cosA * diffuse ) * texDiffuse + Spec, 1 ) * i.VertClr;
}

technique DefaultTech
<
	string Description			= "simple diffuse bump mapping + per-vertex specular hightlight shader + vertex coloring";
	bool   ComputeTangentSpace	= true;
	string VertexFormat			= "VERTEX_XYZNCT1T";
	bool   Default				= true;
	bool   UseAlpha				= false;
>
{
	pass Single
	{
		VertexShader = compile vs_2_0 PS11_BumpBlinnDiffuseSpecularVS();
		PixelShader = compile ps_2_0 PS11_BumpBlinnDiffuseSpecularPS( g_Ambient, g_Diffuse );

        	//AlphaBlendEnable = False;
        	//AlphaTestEnable = False;

        	//CullMode = CCW;

        	//FillMode = Solid;

        	//ZWriteEnable = True;

        	//FogEnable = true;
    	}
}