/*
** Standard library header.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LUALIB_H
#define _LUALIB_H

#include "lua.h"

#define LUA_FILEHANDLE	"FILE*"

#define LUA_COLIBNAME	"coroutine"
#define LUA_MATHLIBNAME	"math"
#define LUA_STRLIBNAME	"string"
#define LUA_TABLIBNAME	"table"
#define LUA_IOLIBNAME	"io"
#define LUA_OSLIBNAME	"os"
#define LUA_LOADLIBNAME	"package"
#define LUA_DBLIBNAME	"debug"
#define LUA_BITLIBNAME	"bit"
#define LUA_JITLIBNAME	"jit"
#define LUA_FFILIBNAME	"ffi"
#define LUA_UTF8LIBNAME	"utf8"
#define LUA_THRLIBNAME	"thread"

LUALIB_API int luaopen_base(lua_State *L);
LUALIB_API int luaopen_math(lua_State *L);
LUALIB_API int luaopen_string(lua_State *L);
LUALIB_API int luaopen_table(lua_State *L);
LUALIB_API int luaopen_io(lua_State *L);
LUALIB_API int luaopen_os(lua_State *L);
LUALIB_API int luaopen_package(lua_State *L);
LUALIB_API int luaopen_debug(lua_State *L);
LUALIB_API int luaopen_bit(lua_State *L);
LUALIB_API int luaopen_jit(lua_State *L);
LUALIB_API int luaopen_ffi(lua_State *L);
LUALIB_API int luaopen_utf8(lua_State *L);
LUALIB_API int luaopen_string_buffer(lua_State *L);

/* Library selection bits for luaL_openselectedlibs(). The bit order
** follows Lua 5.5 (LUA_GLIBK is the base library).
*/
#define LUA_GLIBK	1
#define LUA_LOADLIBK	(LUA_GLIBK << 1)
#define LUA_COLIBK	(LUA_LOADLIBK << 1)
#define LUA_DBLIBK	(LUA_COLIBK << 1)
#define LUA_IOLIBK	(LUA_DBLIBK << 1)
#define LUA_MATHLIBK	(LUA_IOLIBK << 1)
#define LUA_OSLIBK	(LUA_MATHLIBK << 1)
#define LUA_STRLIBK	(LUA_OSLIBK << 1)
#define LUA_TABLIBK	(LUA_STRLIBK << 1)
#define LUA_UTF8LIBK	(LUA_TABLIBK << 1)
/* LuaJIT extensions. */
#define LUA_BITLIBK	(LUA_UTF8LIBK << 1)
#define LUA_JITLIBK	(LUA_BITLIBK << 1)
#define LUA_FFILIBK	(LUA_JITLIBK << 1)

LUALIB_API void (luaL_openselectedlibs) (lua_State *L, int load, int preload);

/* Open all standard libraries. The FFI is preloaded, but not opened. */
#define luaL_openlibs(L)	luaL_openselectedlibs(L, ~LUA_FFILIBK, LUA_FFILIBK)

/* Binary compatibility alias of the macro above. */
LUALIB_API void (luaL_openlibs) (lua_State *L);

#ifndef lua_assert
#define lua_assert(x)	((void)0)
#endif

#endif
