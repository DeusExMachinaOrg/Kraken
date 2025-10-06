#ifndef __KRAKEN_HTA_POINTERS_HPP__
#define __KRAKEN_HTA_POINTERS_HPP__

namespace kraken::hta {
#pragma region: CRT
    typedef void* (__cdecl* CRT_Malloc)(size_t size);
    const CRT_Malloc _Malloc = (CRT_Malloc) 0x00965162;

    typedef void* (__cdecl* CRT_Realloc)(void* ptr, size_t size);
    const CRT_Realloc _Realloc = (CRT_Realloc) 0x0096519E;

    typedef void (__cdecl* CRT_Free)(void* ptr);
    const CRT_Free _Free = (CRT_Free) 0x00965168;
#pragma endregion: CRT

#pragma region: LUA
    typedef struct _lua_State  lua_State;
    typedef struct _lua_Debug  lua_Debug;
    typedef struct _luaL_reg   luaL_reg;
    typedef struct _lua_Buffer luaL_Buffer;

    typedef void (__fastcall *TPLUA_CALL)(lua_State *L, int nargs, int nresults);
    const TPLUA_CALL lua_call = (TPLUA_CALL) 0x00895660;

    typedef int (__fastcall *TPLUA_CHECKSTACK)(lua_State *L, int size);
    const TPLUA_CHECKSTACK lua_checkstack = (TPLUA_CHECKSTACK) 0x00894950;

    typedef void (__fastcall *TPLUA_CLOSE)(lua_State *L);
    const TPLUA_CLOSE lua_close = (TPLUA_CLOSE) 0x008A0530;

    typedef void (__fastcall *TPLUA_CONCAT)(lua_State *L, int n);
    const TPLUA_CONCAT lua_concat = (TPLUA_CONCAT) 0x008958F0;

    typedef int (__fastcall *TPLUA_CPCALL)(lua_State *L, int (__fastcall *func)(lua_State *), void *ud);
    const TPLUA_CPCALL lua_cpcall = (TPLUA_CPCALL) 0x00895780;

    typedef int (__fastcall *TPLUA_DOBUFFER)(lua_State *L, const char *buff, unsigned int size, const char *name);
    const TPLUA_DOBUFFER lua_dobuffer = (TPLUA_DOBUFFER) 0x00896B60;

    typedef int (__fastcall *TPLUA_DOFILE)(lua_State *L, const char *filename);
    const TPLUA_DOFILE lua_dofile = (TPLUA_DOFILE) 0x00896B30;

    typedef int (__fastcall *TPLUA_DOSTRING)(lua_State *L, const char *str);
    const TPLUA_DOSTRING lua_dostring = (TPLUA_DOSTRING) 0x00896BB0;

    typedef int (__fastcall *TPLUA_DUMP)(lua_State *L, int (__fastcall *writer)(lua_State *, const void *, unsigned int, void *), void *data);
    const TPLUA_DUMP lua_dump = (TPLUA_DUMP) 0x00895800;

    typedef int (__fastcall *TPLUA_EQUAL)(lua_State *L, int index1, int index2);
    const TPLUA_EQUAL lua_equal = (TPLUA_EQUAL) 0x00894D80;

    typedef int (__fastcall *TPLUA_ERROR)(lua_State *L);
    const TPLUA_ERROR lua_error = (TPLUA_ERROR) 0x008958A0;

    typedef void (__fastcall *TPLUA_GETFENV)(lua_State *L, int idx);
    const TPLUA_GETFENV lua_getfenv = (TPLUA_GETFENV) 0x00895470;

    typedef int (__fastcall *TPLUA_GETGCCOUNT)(lua_State *L);
    const TPLUA_GETGCCOUNT lua_getgccount = (TPLUA_GETGCCOUNT) 0x00895850;

    typedef int (__fastcall *TPLUA_GETGCTHRESHOLD)(lua_State *L);
    const TPLUA_GETGCTHRESHOLD lua_getgcthreshold = (TPLUA_GETGCTHRESHOLD) 0x00895840;

    typedef int (__fastcall *TPLUA_GETHOOKCOUNT)(lua_State *L);
    const TPLUA_GETHOOKCOUNT lua_gethookcount = (TPLUA_GETHOOKCOUNT) 0x00897020;

    typedef int (__fastcall *TPLUA_GETHOOKMASK)(lua_State *L);
    const TPLUA_GETHOOKMASK lua_gethookmask = (TPLUA_GETHOOKMASK) 0x00897010;

    typedef int (__fastcall *TPLUA_GETINFO)(lua_State *L, const char *what, lua_Debug *ar);
    const TPLUA_GETINFO lua_getinfo = (TPLUA_GETINFO) 0x00897C10;

    typedef const char* (__fastcall *TPLUA_GETLOCAL)(lua_State *L, const lua_Debug *ar, int n);
    const TPLUA_GETLOCAL lua_getlocal = (TPLUA_GETLOCAL) 0x008970D0;

    typedef int (__fastcall *TPLUA_GETMETATABLE)(lua_State *L, int objindex);
    const TPLUA_GETMETATABLE lua_getmetatable = (TPLUA_GETMETATABLE) 0x00895400;

    typedef int (__fastcall *TPLUA_GETSTACK)(lua_State *L, int level, lua_Debug *ar);
    const TPLUA_GETSTACK lua_getstack = (TPLUA_GETSTACK) 0x00897030;

    typedef void (__fastcall *TPLUA_GETTABLE)(lua_State *L, int idx);
    const TPLUA_GETTABLE lua_gettable = (TPLUA_GETTABLE) 0x008952D0;

    typedef int (__fastcall *TPLUA_GETTOP) (lua_State *L);
    const TPLUA_GETTOP lua_gettop = (TPLUA_GETTOP) 0x00894A30;

    typedef const char* (__fastcall *TPLUA_GETUPVALUE) (lua_State *L, int funcindex, int n);
    const TPLUA_GETUPVALUE lua_getupvalue = (TPLUA_GETUPVALUE) 0x00895A60;

    typedef void (__fastcall *TPLUA_INSERT) (lua_State *L, int idx);
    const TPLUA_INSERT lua_insert = (TPLUA_INSERT) 0x00894AE0;

    typedef int (__fastcall *TPLUA_ISCFUNCTION)(lua_State *L, int idx);
    const TPLUA_ISCFUNCTION lua_iscfunction = (TPLUA_ISCFUNCTION) 0x00894C00;

    typedef int (__fastcall *TPLUA_ISNUMBER)(lua_State *L, int idx);
    const TPLUA_ISNUMBER lua_isnumber = (TPLUA_ISNUMBER) 0x00894C40;

    typedef int (__fastcall *TPLUA_ISSTRING)(lua_State *L, int idx);
    const TPLUA_ISSTRING lua_isstring = (TPLUA_ISSTRING) 0x00894C90;

    typedef int (__fastcall *TPLUA_ISUSERDATA)(lua_State *L, int idx);
    const TPLUA_ISUSERDATA lua_isuserdata = (TPLUA_ISUSERDATA) 0x00894CD0;

    typedef int (__fastcall *TPLUA_LESSTHAN)(lua_State *L, int index1, int index2);
    const TPLUA_LESSTHAN lua_lessthan = (TPLUA_LESSTHAN) 0x00894E00;

    typedef int (__fastcall *TPLUA_LOAD)(lua_State *L, const char *(__fastcall *reader)(lua_State *, void *, unsigned int *), void *data, const char *chunkname);
    const TPLUA_LOAD lua_load = (TPLUA_LOAD) 0x008957B0;

    typedef void (__fastcall *TPLUA_NEWTABLE)(lua_State *L);
    const TPLUA_NEWTABLE lua_newtable = (TPLUA_NEWTABLE) 0x008953C0;

    typedef lua_State* (__fastcall *TPLUA_NEWTHREAD)(lua_State *L);
    const TPLUA_NEWTHREAD lua_newthread = (TPLUA_NEWTHREAD) 0x00894A00;

    typedef void* (__fastcall *TPLUA_NEWUSERDATA)(lua_State *L, unsigned int size);
    const TPLUA_NEWUSERDATA lua_newuserdata = (TPLUA_NEWUSERDATA) 0x00895960;

    typedef int (__fastcall *TPLUA_NEXT) (lua_State *L, int idx);
    const TPLUA_NEXT lua_next = (TPLUA_NEXT) 0x008958B0;

    typedef lua_State* (__fastcall *TPLUA_OPEN) ();
    const TPLUA_OPEN lua_open = (TPLUA_OPEN) 0x008A04A0;

    typedef int (__fastcall *TPLUA_PCALL)(lua_State *L, int nargs, int nresults, int errfunc);
    const TPLUA_PCALL lua_pcall = (TPLUA_PCALL) 0x00895690;

    typedef void (__fastcall *TPLUA_PUSHBOOLEAN)(lua_State *L, int b);
    const TPLUA_PUSHBOOLEAN lua_pushboolean = (TPLUA_PUSHBOOLEAN) 0x00895290;

    typedef void (__fastcall *TPLUA_PUSHCCLOSURE)(lua_State *L, int (__fastcall *fn)(lua_State *), int n);
    const TPLUA_PUSHCCLOSURE lua_pushcclosure = (TPLUA_PUSHCCLOSURE) 0x00895210;

    typedef void (__fastcall *TPLUA_PUSHLSTRING)(lua_State *L, const char *s, unsigned int len);
    const TPLUA_PUSHLSTRING lua_pushlstring = (TPLUA_PUSHLSTRING) 0x00895110;

    typedef void (__fastcall *TPLUA_PUSHNIL)(lua_State *L);
    const TPLUA_PUSHNIL lua_pushnil = (TPLUA_PUSHNIL) 0x008950E0;

    typedef void (__fastcall *TPLUA_PUSHNUMBER)(lua_State *L, long double n);
    const TPLUA_PUSHNUMBER lua_pushnumber = (TPLUA_PUSHNUMBER) 0x008950F0;

    typedef void (__fastcall *TPLUA_PUSHSTRING)(lua_State *L, const char *s);
    const TPLUA_PUSHSTRING lua_pushstring = (TPLUA_PUSHSTRING) 0x00895150;

    typedef int (__fastcall *TPLUA_PUSHUPVALUES)(lua_State *L);
    const TPLUA_PUSHUPVALUES lua_pushupvalues = (TPLUA_PUSHUPVALUES) 0x008959A0;

    typedef void (__fastcall *TPLUA_PUSHVALUE)(lua_State *L, int idx);
    const TPLUA_PUSHVALUE lua_pushvalue = (TPLUA_PUSHVALUE) 0x00894B70;

    typedef const char* (__fastcall *TPLUA_PUSHVFSTRING)(lua_State *L, const char *fmt, char *argp);
    const TPLUA_PUSHVFSTRING lua_pushvfstring = (TPLUA_PUSHVFSTRING) 0x008951B0;

    typedef int (__fastcall *TPLUA_RAWEQUAL)(lua_State *L, int index1, int index2);
    const TPLUA_RAWEQUAL lua_rawequal = (TPLUA_RAWEQUAL) 0x00894D10;

    typedef void (__fastcall *TPLUA_RAWGET)(lua_State *L, int idx);
    const TPLUA_RAWGET lua_rawget = (TPLUA_RAWGET) 0x00895320;

    typedef void (__fastcall *TPLUA_RAWGETI)(lua_State *L, int idx, int n);
    const TPLUA_RAWGETI lua_rawgeti = (TPLUA_RAWGETI) 0x00895370;

    typedef void (__fastcall *TPLUA_RAWSET)(lua_State *L, int idx);
    const TPLUA_RAWSET lua_rawset = (TPLUA_RAWSET) 0x00895500;

    typedef void (__fastcall *TPLUA_RAWSETI)(lua_State *L, int idx, int n);
    const TPLUA_RAWSETI lua_rawseti = (TPLUA_RAWSETI) 0x00895550;

    typedef void (__fastcall *TPLUA_REMOVE)(lua_State *L, int idx);
    const TPLUA_REMOVE lua_remove = (TPLUA_REMOVE) 0x00894A90;

    typedef void (__fastcall *TPLUA_REPLACE)(lua_State *L, int idx);
    const TPLUA_REPLACE lua_replace = (TPLUA_REPLACE) 0x00894B30;

    typedef int (__fastcall *TPLUA_RESUME)(lua_State *L, int nargs);
    const TPLUA_RESUME lua_resume = (TPLUA_RESUME) 0x009414F0;

    typedef int (__fastcall *TPLUA_SETFENV)(lua_State *L, int idx);
    const TPLUA_SETFENV lua_setfenv = (TPLUA_SETFENV) 0x00895600;

    typedef void (__fastcall *TPLUA_SETGCTHRESHOLD)(lua_State *L, int newthreshold);
    const TPLUA_SETGCTHRESHOLD lua_setgcthreshold = (TPLUA_SETGCTHRESHOLD) 0x00895860;

    typedef int (__fastcall *TPLUA_SETHOOK)(lua_State *L, void (__fastcall *func)(lua_State *, lua_Debug *), int mask, int count);
    const TPLUA_SETHOOK lua_sethook = (TPLUA_SETHOOK) 0x00896FD0;

    typedef const char* (__fastcall *TPLUA_SETLOCAL)(lua_State *L, const lua_Debug *ar, int n);
    const TPLUA_SETLOCAL lua_setlocal = (TPLUA_SETLOCAL) 0x00897150;

    typedef int (__fastcall *TPLUA_SETMETATABLE)(lua_State *L, int objindex);
    const TPLUA_SETMETATABLE lua_setmetatable = (TPLUA_SETMETATABLE) 0x008955A0;

    typedef void (__fastcall *TPLUA_SETTABLE)(lua_State *L, int idx);
    const TPLUA_SETTABLE lua_settable = (TPLUA_SETTABLE) 0x008954C0;

    typedef void (__fastcall *TPLUA_SETTOP)(lua_State *L, int idx);
    const TPLUA_SETTOP lua_settop = (TPLUA_SETTOP) 0x00894A40;

    typedef const char* (__fastcall *TPLUA_SETUPVALUE)(lua_State *L, int funcindex, int n);
    const TPLUA_SETUPVALUE lua_setupvalue = (TPLUA_SETUPVALUE) 0x00895AA0;

    typedef unsigned int (__fastcall *TPLUA_STRLEN)(lua_State *L, int idx);
    const TPLUA_STRLEN lua_strlen = (TPLUA_STRLEN) 0x00894F70;

    typedef int (__fastcall *TPLUA_TOBOOLEAN)(lua_State *L, int idx);
    const TPLUA_TOBOOLEAN lua_toboolean = (TPLUA_TOBOOLEAN) 0x00894EC0;

    typedef long double (__fastcall *TPLUA_TONUMBER)(lua_State *L, int idx);
    const TPLUA_TONUMBER lua_tonumber = (TPLUA_TONUMBER) 0x00894E70;

    typedef const void* (__fastcall *TPLUA_TOPOINTER)(lua_State *L, int idx);
    const TPLUA_TOPOINTER lua_topointer = (TPLUA_TOPOINTER) 0x00895070;

    typedef const char* (__fastcall *TPLUA_TOSTRING)(lua_State *L, int idx);
    const TPLUA_TOSTRING lua_tostring = (TPLUA_TOSTRING) 0x00894F00;

    typedef lua_State* (__fastcall *TPLUA_TOTHREAD)(lua_State *L, int idx);
    const TPLUA_TOTHREAD lua_tothread = (TPLUA_TOTHREAD) 0x00895040;

    typedef void* (__fastcall *TPLUA_TOUSERDATA)(lua_State *L, int idx);
    const TPLUA_TOUSERDATA lua_touserdata = (TPLUA_TOUSERDATA) 0x00895000;

    typedef int (__fastcall *TPLUA_TYPE)(lua_State *L, int idx);
    const TPLUA_TYPE lua_type = (TPLUA_TYPE) 0x00894BB0;

    typedef const char* (__fastcall *TPLUA_TYPENAME)(lua_State *L, int t);
    const TPLUA_TYPENAME lua_typename = (TPLUA_TYPENAME) 0x00894BE0;

    typedef const char* (__fastcall *TPLUA_VERSION)();
    const TPLUA_VERSION lua_version = (TPLUA_VERSION) 0x00895890;

    typedef void (__fastcall *TPLUA_XMOVE)(lua_State *from, lua_State *to, int n);
    const TPLUA_XMOVE lua_xmove = (TPLUA_XMOVE) 0x008949A0;

    typedef int (__fastcall *TPLUA_YIELD)(lua_State *L, int nresults);
    const TPLUA_YIELD lua_yield = (TPLUA_YIELD) 0x009412F0;

    typedef void (__fastcall* TPLUAL_ADDLSTRING)(luaL_Buffer *B, const char *s, unsigned int l);
    const TPLUAL_ADDLSTRING luaL_addlstring = (TPLUAL_ADDLSTRING) 0x008965A0;

    typedef void (__fastcall* TPLUAL_ADDSTRING)(luaL_Buffer *B, const char *s);
    const TPLUAL_ADDSTRING luaL_addstring = (TPLUAL_ADDSTRING) 0x00896600;

    typedef void (__fastcall* TPLUAL_ADDVALUE)(luaL_Buffer *B);
    const TPLUAL_ADDVALUE luaL_addvalue = (TPLUAL_ADDVALUE) 0x00896660;

    typedef int (__fastcall* TPLUAL_ARGERROR)(lua_State *L, int narg, const char *extramsg);
    const TPLUAL_ARGERROR luaL_argerror = (TPLUAL_ARGERROR) 0x00896C10;

    typedef void (__fastcall* TPLUAL_BUFFINIT)(lua_State *L, luaL_Buffer *B);
    const TPLUAL_BUFFINIT luaL_buffinit = (TPLUAL_BUFFINIT) 0x00896700;

    typedef int (__fastcall* TPLUAL_CALLMETA)(lua_State *L, int obj, const char *event);
    const TPLUAL_CALLMETA luaL_callmeta = (TPLUAL_CALLMETA) 0x00896110;

    typedef void (__fastcall* TPLUAL_CHECKANY)(lua_State *L, int narg);
    const TPLUAL_CHECKANY luaL_checkany = (TPLUAL_CHECKANY) 0x00896D80;

    typedef const char* (__fastcall* TPLUAL_CHECKLSTRING)(lua_State *L, int narg, unsigned int *len);
    const TPLUAL_CHECKLSTRING luaL_checklstring = (TPLUAL_CHECKLSTRING) 0x00896DB0;

    typedef long double (__fastcall* TPLUAL_CHECKNUMBER)(lua_State *L, int narg);
    const TPLUAL_CHECKNUMBER luaL_checknumber = (TPLUAL_CHECKNUMBER) 0x00896E80;

    typedef void (__fastcall* TPLUAL_CHECKSTACK)(lua_State *L, int space, const char *mes);
    const TPLUAL_CHECKSTACK luaL_checkstack = (TPLUAL_CHECKSTACK) 0x00896080;

    typedef void (__fastcall* TPLUAL_CHECKTYPE)(lua_State *L, int narg, int t);
    const TPLUAL_CHECKTYPE luaL_checktype = (TPLUAL_CHECKTYPE) 0x00896D30;

    typedef void* (__fastcall* TPLUAL_CHECKUDATA)(lua_State *L, int ud, const char *tname);
    const TPLUAL_CHECKUDATA luaL_checkudata = (TPLUAL_CHECKUDATA) 0x00895FF0;

    typedef int (__fastcall* TPLUAL_FINDSTRING)(const char *name, const char *const *list);
    const TPLUAL_FINDSTRING luaL_findstring = (TPLUAL_FINDSTRING) 0x00895EE0;

    typedef int (__fastcall* TPLUAL_GETMETAFIELD)(lua_State *L, int obj, const char *event);
    const TPLUAL_GETMETAFIELD luaL_getmetafield = (TPLUAL_GETMETAFIELD) 0x008960B0;

    typedef void (__fastcall* TPLUAL_GETMETATABLE)(lua_State *L, const char *tname);
    const TPLUAL_GETMETATABLE luaL_getmetatable = (TPLUAL_GETMETATABLE)   0x00895FD0;

    typedef int (__fastcall* TPLUAL_GETN)(lua_State *L, int t);
    const TPLUAL_GETN luaL_getn = (TPLUAL_GETN) 0x00896400;

    typedef int (__fastcall* TPLUAL_LOADBUFFER)(lua_State *L, const char *buff, unsigned int size, const char *name);
    const TPLUAL_LOADBUFFER luaL_loadbuffer = (TPLUAL_LOADBUFFER) 0x00896A50;

    typedef int (__fastcall* TPLUAL_LOADFILE)(lua_State *L, const char *filename);
    const TPLUAL_LOADFILE luaL_loadfile = (TPLUAL_LOADFILE) 0x008968D0;

    typedef int (__fastcall* TPLUAL_NEWMETATABLE)(lua_State *L, const char *tname);
    const TPLUAL_NEWMETATABLE luaL_newmetatable = (TPLUAL_NEWMETATABLE) 0x00895F40;

    typedef void (__fastcall* TPLUAL_OPENLIB) (lua_State *L, const char *libname, const luaL_reg *l, int nup);
    const TPLUAL_OPENLIB luaL_openlib = (TPLUAL_OPENLIB) 0x00896170;

    typedef const char* (__fastcall* TPLUAL_OPTLSTRING)(lua_State *L, int narg, const char *def, unsigned int *len);
    const TPLUAL_OPTLSTRING luaL_optlstring = (TPLUAL_OPTLSTRING) 0x00896E20;

    typedef long double (__fastcall* TPLUAL_OPTNUMBER)(lua_State *L, int narg, long double def);
    const TPLUAL_OPTNUMBER luaL_optnumber = (TPLUAL_OPTNUMBER) 0x00896F00;

    typedef char* (__fastcall* TPLUAL_PREPBUFFER)(luaL_Buffer *B);
    const TPLUAL_PREPBUFFER luaL_prepbuffer = (TPLUAL_PREPBUFFER) 0x00896570;

    typedef void (__fastcall* TPLUAL_PUSHRESULT)(luaL_Buffer *B);
    const TPLUAL_PUSHRESULT luaL_pushresult = (TPLUAL_PUSHRESULT) 0x00896620;

    typedef int (__fastcall* TPLUAL_REF)(lua_State *L, int t);
    const TPLUAL_REF luaL_ref = (TPLUAL_REF) 0x00896710;

    typedef void (__fastcall* TPLUAL_SETN)(lua_State *L, int t, int n);
    const TPLUAL_SETN luaL_setn = (TPLUAL_SETN) 0x00896350;

    typedef int (__fastcall* TPLUAL_TYPERROR) (lua_State *L, int narg, const char *tname);
    const TPLUAL_TYPERROR luaL_typerror = (TPLUAL_TYPERROR) 0x00896CB0;
    
    typedef void (__fastcall* TPLUAL_UNREF)(lua_State *L, int t, int ref);
    const TPLUAL_UNREF luaL_unref = (TPLUAL_UNREF) 0x008967F0;
    
    typedef void (__fastcall* TPLUAL_WHERE)(lua_State *L, int level);
    const TPLUAL_WHERE luaL_where = (TPLUAL_WHERE) 0x00895E40;
#pragma endregion: LUA
};
#endif