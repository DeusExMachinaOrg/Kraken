#pragma once
#include "Func.h"
#include <string>

struct CStr
{
	char* m_charPtr;
	int allocSz;
	char zeroChar;

	CStr()
	{

	}

	CStr(const char* str)
	{
		allocSz = strlen(str) + 1;
		m_charPtr = new char[allocSz];
		strcpy(m_charPtr, str);
	}

	bool Equal(char* str) const
	{
		return this->allocSz && !strcmp(this->m_charPtr, str);
	}

	bool Equal(const char* str) const
	{
		return this->allocSz && !strcmp(this->m_charPtr, str);
	}
};

inline bool operator<(const CStr& lhs, const CStr& rhs)
{
	FUNC(0x00425D80, bool, __fastcall, _operator, const CStr*, const CStr*);
	return _operator(&lhs, &rhs);
}