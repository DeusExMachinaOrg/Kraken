
// Texture samplers 


#ifdef MIN_MAG_FILTER
#ifdef MIP_FILTER
#ifdef MAX_ANISOTROPY


#define DECLARE_DIFFUSE_SAMPLER( samplerName, textureName )	\
		sampler samplerName = sampler_state		\
		{										\
			Texture = <textureName>;			\
			AddressU  = Wrap;					\
			AddressV  = Wrap;					\
			MinFilter = MIN_MAG_FILTER;			\
			MagFilter = MIN_MAG_FILTER;			\
			MipFilter = MIP_FILTER;				\
			MaxAnisotropy =  MAX_ANISOTROPY;	\
		};



#define DECLARE_DIFFUSE_SAMPLER_CLAMP( samplerName, textureName )	\
		sampler samplerName = sampler_state		\
		{										\
			Texture = <textureName>;			\
			AddressU  = Clamp;					\
			AddressV  = Clamp;					\
			MinFilter = MIN_MAG_FILTER;			\
			MagFilter = MIN_MAG_FILTER;			\
			MipFilter = MIP_FILTER;				\
			MaxAnisotropy =  MAX_ANISOTROPY;	\
		};


#define DECLARE_DETAIL_SAMPLER( samplerName, textureName )	\
		sampler samplerName = sampler_state		\
		{										\
			Texture = <textureName>;			\
			AddressU  = Wrap;					\
			AddressV  = Wrap;					\
			MinFilter = MIN_MAG_FILTER;			\
			MagFilter = MIN_MAG_FILTER;			\
			MipFilter = MIP_FILTER;				\
			MaxAnisotropy =  MAX_ANISOTROPY;	\
		};

#define FILTERS_DEFINED

#endif // MAX_ANISOTROPY
#endif // MIP_FILTER
#endif // MIN_MAG_FILTER



#ifndef FILTERS_DEFINED

#define DECLARE_DIFFUSE_SAMPLER( samplerName, textureName )	\
		sampler samplerName = sampler_state		\
		{										\
			Texture = <textureName>;			\
			AddressU  = Wrap;					\
			AddressV  = Wrap;					\
			MinFilter = Linear;					\
			MagFilter = Linear;					\
			MipFilter = Point;					\
		};



#define DECLARE_DIFFUSE_SAMPLER_CLAMP( samplerName, textureName )	\
		sampler samplerName = sampler_state		\
		{										\
			Texture = <textureName>;			\
			AddressU  = Clamp;					\
			AddressV  = Clamp;					\
			MinFilter = Linear;					\
			MagFilter = Linear;					\
			MipFilter = Point;					\
		};


#define DECLARE_DETAIL_SAMPLER( samplerName, textureName )	\
		sampler samplerName = sampler_state		\
		{						\
			Texture = <textureName>;		\
			AddressU  = Wrap;			\
			AddressV  = Wrap;			\
			MinFilter = Linear;			\
			MagFilter = Linear;			\
			MipFilter = Point;			\
		};

#endif // FILTERS_DEFINED



#define DECLARE_BUMP_SAMPLER( samplerName, textureName )	\
		sampler	samplerName = sampler_state		\
		{                                               \
			Texture	  = (textureName);		\
			AddressU  = Wrap;			\
			AddressV  = Wrap;                       \
			MipFilter = Linear;			\
			MinFilter = Linear;			\
			MagFilter = Linear;			\
			MipMapLodBias = -1.f;		\
		};


#define DECLARE_CUBEMAP_SAMPLER( samplerName, textureName )	\
		samplerCUBE samplerName = sampler_state		\
		{						\
			Texture = <textureName>;		\
			AddressU = Clamp;			\
			AddressV = Clamp;			\
			AddressW = Clamp;			\
			MinFilter = Linear;			\
			MagFilter = Linear;			\
			MipFilter = Linear;			\
		};

				
#define DECLARE_SHADOW_SAMPLER( samplerName, textureName )	\
		sampler samplerName = sampler_state					\
		{													\
			Texture		= <textureName>;					\
			AddressU	= Border;							\
			AddressV	= Border;							\
			AddressW	= Border;							\
			MinFilter	= Linear;							\
			MagFilter	= Linear;							\
			MipFilter	= None;								\
		};


#define DECLARE_PROJECTOR_SAMPLER( samplerName, textureName )	\
		sampler samplerName = sampler_state					\
		{													\
			Texture		= <textureName>;					\
			AddressU	= Clamp;							\
			AddressV	= Clamp;							\
			AddressW	= Clamp;							\
			MinFilter	= Linear;							\
			MagFilter	= Linear;							\
			MipFilter	= Linear;							\
		};


#define DECLARE_CONTOUR_SAMPLER( samplerName, textureName )	\
		sampler samplerName = sampler_state					\
		{													\
			Texture		= <textureName>;					\
			AddresssU	= Clamp;							\
			AddressV	= Clamp;							\
			AddressW	= Clamp;							\
			MinFilter	= Linear;							\
			MagFilter	= Linear;							\
			MipFilter	= None;								\
		};
				
    