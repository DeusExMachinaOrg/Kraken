#pragma once

namespace m3d
{
	struct RefCountedBase
	{
		virtual ~RefCountedBase();

		int refCount;
	};
}