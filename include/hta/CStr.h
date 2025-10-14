#pragma once
#include "Func.h"
#include <string>
#include <unordered_set>

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

	// custom method
	bool In(std::initializer_list<const char*> strings) const
	{
	    if (!this->allocSz) return false;
	    
	    for (const char* str : strings)
	    {
	        if (!strcmp(this->charPtr, str))
	            return true;
	    }
	    return false;
	}

	// custom method
	bool In(const std::unordered_set<std::string_view>& strings) const
	{
	    if (!this->allocSz) return false;
	    return strings.find(std::string_view(this->charPtr)) != strings.end();
	}

};

inline bool operator<(const CStr& lhs, const CStr& rhs)
{
	FUNC(0x00425D80, bool, __fastcall, _operator, const CStr*, const CStr*);
	return _operator(&lhs, &rhs);
}