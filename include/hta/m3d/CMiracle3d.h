#pragma once
#include "Application.h"
#include "TruxxUiManager.h"

struct CMiracle3d : m3d::Application
{
	inline static CMiracle3d*& Instance = *(CMiracle3d**)0x00A0A55C;

	char _offset[0x2A8];
	TruxxUiManager* UiManager;

	void PutSplash(int proc, const char* text)
	{
		FUNC(0x004039D0, void, __thiscall, _PutSplash, CMiracle3d*, int, const char*);
		_PutSplash(this, proc, text);
	}
};