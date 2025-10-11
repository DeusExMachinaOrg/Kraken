#pragma once
#include <stdafx.hpp>
#include "GameImpulse.h"
#include "IRenderer.h"

namespace m3d
{
	struct Application
	{
		inline static Application*& Instance = *(Application**)0x00A0A55C;

		BYTE _offset[0x2FC];
		m3d::rend::IRenderer* Renderer;
		BYTE _offset1[0x8AF40];
		IImpulse* Impulse;

		static void __fastcall PutSplashCallBack(int proc, const char** data)
        {
            if (!Instance || !data || !*data)
                return;
            Instance->PutSplash(proc, *data);
        }

		void PutSplash(int proc, const char* data)
		{
			FUNC(0x006A5A70, void, __thiscall, _PutSplash, Application*, int, const char**);
			_PutSplash(this, proc, &data);
		}

		int OneFrame()
		{
			FUNC(0x005A3AD0, int, __thiscall, _OneFrame, Application*);
			return _OneFrame(this);
		}
	};
}