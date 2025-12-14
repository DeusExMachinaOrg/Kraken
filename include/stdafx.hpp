#ifndef __KRAKEN_STDAFX_HPP__
#define __KRAKEN_STDAFX_HPP__

#define NOMINMAX

#include <memory>
#include <stdexcept>
#include <stdint.h>
#include <assert.h>

#include <windows.h>

#define API __declspec(dllexport)

void* operator new[](size_t size);
void* operator new(size_t size);

void operator delete[](void* Block);
void operator delete(void* Block);

#endif