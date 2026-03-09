#include <windows.h>

__declspec(noinline)
far HMODULE __stdcall HTA_LoadLibrary(LPCSTR libname){
    return NULL;
};

__declspec(noinline)
far bool __stdcall HTA_FreeLibrary(HMODULE module){
    return NULL;
};

__declspec(noinline)
far HMODULE __stdcall HTA_GetProcAddress(HMODULE hModule, LPCSTR procname){
    return NULL;
};

__declspec(noinline)
far void *__cdecl HTA_Malloc(size_t size){
    return NULL;
};

__declspec(noinline)
far void __cdecl HTA_Free(void *ptr){};

__declspec(noinline)
far void __cdecl HTA_Close(int code){};

typedef void (__cdecl* KRKN_Create) (HMODULE module);

void __cdecl Kraken_Create(void) {
    HMODULE kraken = HTA_LoadLibrary("kraken.dll");
    if (!kraken)
        HTA_Close(-1);
    KRKN_Create create = (KRKN_Create)HTA_GetProcAddress(kraken, (LPSTR)1);
    if (!create)
        HTA_Close(-1);
    create(kraken);
};