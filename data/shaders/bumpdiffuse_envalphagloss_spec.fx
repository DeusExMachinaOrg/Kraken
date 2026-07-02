//////////////////////////////////////////////////////////////////////////////
//
// Workfile: BumpDiffuse_EnvAlphaGloss_Spec.fx
// Created by: Plus
//
// Diffuse bump mapping (one directional light) + per-pixel env mapping.
// final_color = lerp( diffuse_color, envmap_color, diffuse_color_alpha ).
//
// $Id: BumpDiffuse_EnvAlphaGloss_Spec.fx,v 1.11 2005/11/14 11:56:48 plus Exp $
//
//////////////////////////////////////////////////////////////////////////////

#include "lib.fx"


// textures
texture 		texDiffuse	:	DIFFUSE_MAP_0;
texture			texBumpGloss	:	BUMP_MAP_0;
texture			texEnvMap 	:	CUBE_MAP_0;

// declare samplers
DECLARE_DIFFUSE_SAMPLER( DiffSampler, texDiffuse )
DECLARE_BUMP_SAMPLER( BumpSampler, texBumpGloss )
DECLARE_CUBEMAP_SAMPLER( EnvSampler, texEnvMap )


// viewer position (object space)
float4	objectViewPos	: VIEW_POS
<
	int Space = SPACE_OBJECT;
>;


// light directions (object space)
float3 DirFromLight: TMP_LIGHT0_DIR 
<
	int Space = SPACE_OBJECT;
>;


// transformations
row_major float4x4 mFinal: TOTAL_MATRIX; 
row_major float4x4 mWorld: WORLD_MATRIX;


shared const float4 g_Ambient		: LIGHT_AMBIENT 	= { 0.2f, 0.2f, 0.2f, 1.0f };
shared const float4 g_Diffuse		: LIGHT_DIFFUSE 	= { 1.0f, 1.0f, 1.0f, 1.0f };
shared const float3 g_Specular		: LIGHT_SPECULAR 	= { 1.0f, 1.0f, 1.0f };
shared const float2 g_FogTerm		: FOG_TERM 			= { 1.0f, 800.0f };
shared const float  g_Transparency	: TRANSPARENCY		= 1.f;


// vertex shader input structure
struct VS_INPUT
{
	float3	Pos		: POSITION;	 // position in object space
	float3	Normal	: NORMAL;	 // normal in object space
	float4  Tangent	: TANGENT;   // tangent in object space with a sign of binormal as a w component
	float2	Tex0	: TEXCOORD0; // diffuse/bump texcoords
};


// vertex shader output structure (for ps_2_0)
struct VS11_OUTPUT
{
	float4 Pos	: POSITION;
	float2 uv0	: TEXCOORD0;
	float2 uv0Diff	: TEXCOORD1;
	float3 Light	: COLOR0;
	float3 Half	: COLOR1;
	float3 EnvCoords: TEXCOORD2;
	float  fog	: FOG;
};


//
VS11_OUTPUT PS11_BumpDiffuseVS( VS_INPUT v )
{
	VS11_OUTPUT o = (VS11_OUTPUT)0;

	// position (projected)
	o.Pos               = mul( float4( v.Pos, 1 ), mFinal );
	// texcoords
	o.uv0 = v.Tex0;
	o.uv0Diff = v.Tex0;
	// fog term
	o.fog = VertexFog( o.Pos.z, g_FogTerm );	

	// light direction should come already normalized from the engine...
	float3 objectLight = -DirFromLight;
	
	// calculate binormal
	float3 Binormal = cross( v.Normal, v.Tangent ) * v.Tangent.w; 

	// calculate light vector in texture space
	float3 tangentLight   = toTangentSpace( objectLight, v.Tangent, Binormal, v.Normal );

	// output light vector in tangent space
	o.Light.xyz = 0.5f + 0.5f * tangentLight;
	//Out.Light           = tangentLight;

	// calculate view vector in object space
	float3 objectViewDir  = objectViewPos - v.Pos;
                              
	// calculate view vector in tangent space
	float3 tangentViewDir = toTangentSpace( objectViewDir, v.Tangent, Binormal, v.Normal );
	
	// calculate environment map texcoords
	o.EnvCoords = normalize( mul( reflect( -objectViewDir, v.Normal ), mWorld ) );

	return o;
}


//
float4 PS11_BumpDiffusePS( VS11_OUTPUT i, uniform float4 ambient, uniform float4 diffuse ): COLOR
{
	// fetch normal + gloss factor from texture
	float4	texBump      = tex2D( BumpSampler, i.uv0 );

	// fetch diffuse color + alpha factor from texture
	float4 texDiffuse    = tex2D( DiffSampler, i.uv0Diff );

	// normal (unpack from texture)
	float3  Normal       = bx2( texBump );

	// light
	float3 Light         = bx2( i.Light );

	// dot product of light	with bump
	float cosA           = saturate( dot( Normal, Light ) );

	// fetch em texture
	float3	tEM = texCUBE( EnvSampler, i.EnvCoords.xyz ) * ( cosA + ambient.xyz );

	//
	return float4( lerp( (ambient + cosA * diffuse) * texDiffuse, tEM, texDiffuse.a ), g_Transparency );
}


//
VS11_OUTPUT PS11_BumpSpecularVS( VS_INPUT v )
{
	VS11_OUTPUT o = (VS11_OUTPUT)0;

	// position (projected)
	o.Pos               = mul( float4( v.Pos, 1 ), mFinal );
	// texcoords
	o.uv0 = v.Tex0;

	// light direction should come already normalized from the engine...
	float3 objectLight = -DirFromLight;
	
	// calculate binormal
	float3 Binormal = cross( v.Normal, v.Tangent ) * v.Tangent.w; 

	// calculate light vector in texture space
	float3 tangentLight   = toTangentSpace( objectLight, v.Tangent, Binormal, v.Normal );

	// calculate view vector in object space
	float3 objectViewDir  = objectViewPos - v.Pos;
                              
	// calculate view vector in tangent space
	float3 tangentViewDir = toTangentSpace( objectViewDir, v.Tangent, Binormal, v.Normal );
	
	// calculate half vector in tangent space
	o.Half              = 0.5f + 0.5f * normalize( tangentLight + tangentViewDir );

	return o;
}


//
float4 PS11_BumpSpecularPS( VS11_OUTPUT i ): COLOR
{
	// fetch normal + gloss factor from texture
	float4	texBump      = tex2D( BumpSampler, i.uv0 );

	// normal (unpack from texture)
	float3  Normal       = bx2( texBump );

	// cosine between half and normal
	float cosB           = saturate( dot( bx2( i.Half ), Normal ) );

	// specular term
	float3 Spec          = Normal.z > 0 ? pow16( cosB*cosB ) * texBump.a * g_Specular : 0;

	//
	return float4( Spec, 1 );
}


//
technique PS11_two_passes
<
	string Description = "diffuse bump mapping + per-vertex specular/environment mapping";
	bool   ComputeTangentSpace = true;
	string VertexFormat = "VERTEX_XYZNT1T";
	bool   Default = true;
	bool   UseAlpha	= false;
>
{
	pass Diffuse
	{
		VertexShader = compile vs_2_0 PS11_BumpDiffuseVS();
		PixelShader = compile ps_2_0 PS11_BumpDiffusePS( g_Ambient, g_Diffuse );

		//AlphaBlendEnable = False;
		//AlphaTestEnable = False;
		//FogEnable = true;

		//CullMode = CCW;
	}

	/*pass Specular
	{
		VertexShader = compile vs_2_0 PS11_BumpSpecularVS();
		PixelShader = compile ps_2_0 PS11_BumpSpecularPS();

		AlphaBlendEnable = True;
		SrcBlend = One;
		DestBlend = One;
		ZWriteEnable = False;
	}*/
}


// vertex shader output structure (for ps_2_0)
struct VS20_OUTPUT
{
	float4 Pos	: POSITION;
	float2 uv0	: TEXCOORD0;
	float3 Light	: TEXCOORD1;
	float3 EnvCoords: TEXCOORD2;
	float3 EyeTS	: TEXCOORD3;
	float3 EyeOS	: TEXCOORD4;

	float3 Tangent	: TEXCOORD5;
	float3 Binormal	: TEXCOORD6;
	float3 Normal	: TEXCOORD7;

	float  fog	: FOG;
};


//
VS20_OUTPUT PS20_BumpDiffuseSpecularVS( VS_INPUT v )
{
	VS20_OUTPUT o = (VS20_OUTPUT)0;

	// position (projected)
	o.Pos               = mul( float4( v.Pos, 1 ), mFinal );
	// texcoords
	o.uv0 = v.Tex0;
	// fog term
	o.fog = VertexFog( o.Pos.z, g_FogTerm );	

	// light direction should come already normalized from the engine...
	float3 objectLight = -DirFromLight;
	
	// calculate binormal
	float3 Binormal = cross( v.Normal, v.Tangent ) * v.Tangent.w; 

	// calculate light vector in texture space
	float3 tangentLight   = toTangentSpace( objectLight, v.Tangent, Binormal, v.Normal );

	// output light vector in tangent space
	o.Light.xyz = tangentLight;

	// calculate view vector in object space
	float3 objectViewDir  = objectViewPos - v.Pos;
                              
	// calculate view vector in tangent space
	float3 tangentViewDir = toTangentSpace( objectViewDir, v.Tangent, Binormal, v.Normal );
	
	// calculate environment map texcoords (per-vertex)
	//o.EnvCoords = normalize( -reflect( objectViewDir, v.Normal ) );

	o.Tangent = v.Tangent;
	o.Binormal = Binormal;
	o.Normal = v.Normal;

	// eye vector
	o.EyeTS              = tangentViewDir;
	o.EyeOS = normalize( objectViewPos - v.Pos );

	return o;
}


//
float4 PS20_BumpDiffuseSpecularPS( VS20_OUTPUT i, uniform float4 ambient, uniform float4 diffuse ): COLOR
{
	// fetch normal + gloss factor from texture
	float4	texBump      = tex2D( BumpSampler, i.uv0 );

	// fetch diffuse color + alpha factor from texture
	float4 texDiffuse    = tex2D( DiffSampler, i.uv0 );

	// normal (unpack from texture)
	float3  Normal       = bx2( texBump );

	// light
	float3 Light         = normalize( i.Light );

	// dot product of light	with bump
	float cosA           = saturate( dot( Normal, Light ) );

	// cosine between reflected light and eye vector
	float3 reflectedLightVec = 2 * cosA * Normal - Light;
	float cosB = saturate( dot( normalize( i.EyeTS ), reflectedLightVec ) );		

	//
	float3 Spec          = pow( cosB, 4 ) * texBump.a * cosA * g_Specular;

	// calculate environment map texcoords per pixel
	// compute world space reflection vector
	float3 objectNormal = mul( Normal, float3x3( i.Tangent, i.Binormal, i.Normal ) );
	float3 objectEnvCoords = reflect( -i.EyeOS, objectNormal );
	float3 worldEnvCoords = mul( objectEnvCoords, mWorld );
	float3 tEM = texCUBE( EnvSampler, worldEnvCoords.xyz ) * ( cosA + ambient.xyz );

	//
	return float4( lerp( (ambient + cosA * diffuse) * texDiffuse, tEM, texDiffuse.a ) + Spec, g_Transparency );
}


//
technique PS20_single_pass
<
	string Description = "diffuse bump mapping + per-pixel specular/environment mapping (PS20)";
	bool   ComputeTangentSpace = true;
	string VertexFormat = "VERTEX_XYZNT1T";
	bool   Default = true;
	bool   IsPs20 = true;
	bool   UseAlpha	= false;
>
{
	pass p0
	{
		VertexShader = compile vs_2_0 PS20_BumpDiffuseSpecularVS();
		PixelShader = compile ps_2_0 PS20_BumpDiffuseSpecularPS( g_Ambient, g_Diffuse );
	}
}