#pragma once
#include "Func.h"
#include <string>
#include <unordered_set>

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

	// custom method
	bool In(std::initializer_list<const char*> strings) const
	{
	    if (!this->allocSz) return false;
	    
	    for (const char* str : strings)
	    {
	        if (!strcmp(this->m_charPtr, str))
	            return true;
	    }
	    return false;
	}

	// custom method
	bool In(const std::unordered_set<std::string_view>& strings) const
	{
	    if (!this->allocSz) return false;
	    return strings.find(std::string_view(this->m_charPtr)) != strings.end();
	}

	// cast to string
    operator std::string() const
    {
        return (m_charPtr && allocSz) ? std::string(m_charPtr) : std::string();
    }

};

inline bool operator<(const CStr& lhs, const CStr& rhs)
{
	FUNC(0x00425D80, bool, __fastcall, _operator, const CStr*, const CStr*);
	return _operator(&lhs, &rhs);
}

inline bool operator==(const CStr& lhs, const CStr& rhs)
{
	if (!lhs.allocSz && !rhs.allocSz) return true;
	if (!lhs.allocSz || !rhs.allocSz) return false;
	return strcmp(lhs.m_charPtr, rhs.m_charPtr) == 0;
}

namespace std {
	template<>
	struct hash<CStr> {
		size_t operator()(const CStr& str) const {
			if (!str.allocSz || !str.m_charPtr) {
				return 0;
			}
			return hash<string_view>{}(string_view(str.m_charPtr));
		}
	};
}
