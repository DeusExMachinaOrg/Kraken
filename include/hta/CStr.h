#pragma once
#include "Func.h"
#include <string>

struct CStr
{
	char* charPtr;
	int allocSz;
	char zeroChar;

	CStr()
	{

	}

	CStr(const char* str)
	{
		allocSz = strlen(str) + 1;
		charPtr = new char[allocSz];
		strcpy(charPtr, str);
	}

	bool Equal(char* str) const
	{
		return this->allocSz && !strcmp(this->charPtr, str);
	}

	bool Equal(const char* str) const
	{
		return this->allocSz && !strcmp(this->charPtr, str);
	}
};

inline bool operator<(const CStr& lhs, const CStr& rhs)
{
	FUNC(0x00425D80, bool, __fastcall, _operator, const CStr*, const CStr*);
	return _operator(&lhs, &rhs);
}