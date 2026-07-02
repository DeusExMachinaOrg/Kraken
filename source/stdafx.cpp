#include "stdafx.hpp"

static auto _malloc = (void*(__cdecl*)(size_t))(0x00965162);
void* operator new(size_t size) {
    return _malloc(size);
};

void* operator new[](size_t size) {
    return _malloc(size);
};

static auto _free = (void(__cdecl*)(void*))(0x00965168);
void operator delete(void* Block) {
    return _free(Block);
};

void operator delete[](void* Block) {
    return _free(Block);
};