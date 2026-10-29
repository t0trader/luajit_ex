/*
** Library initialization.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major parts taken verbatim from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lib_init_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arch.h"

/* Standard libraries. The masks follow the order of Lua 5.5.
** In LuaJIT the coroutine library is part of the base library.
*/
static const struct {
  const char *name;
  lua_CFunction func;
  int mask;
} lj_lib_load[] = {
  { "",			luaopen_base,	LUA_GLIBK|LUA_COLIBK },
  { LUA_LOADLIBNAME,	luaopen_package, LUA_LOADLIBK },
  { LUA_TABLIBNAME,	luaopen_table,	LUA_TABLIBK },
  { LUA_IOLIBNAME,	luaopen_io,	LUA_IOLIBK },
  { LUA_OSLIBNAME,	luaopen_os,	LUA_OSLIBK },
  { LUA_STRLIBNAME,	luaopen_string,	LUA_STRLIBK },
  { LUA_MATHLIBNAME,	luaopen_math,	LUA_MATHLIBK },
  { LUA_DBLIBNAME,	luaopen_debug,	LUA_DBLIBK },
  { LUA_BITLIBNAME,	luaopen_bit,	LUA_BITLIBK },
  { LUA_JITLIBNAME,	luaopen_jit,	LUA_JITLIBK },
  { LUA_UTF8LIBNAME,	luaopen_utf8,	LUA_UTF8LIBK },
#if LJ_HASFFI
  { LUA_FFILIBNAME,	luaopen_ffi,	LUA_FFILIBK },
#endif
  { NULL,		NULL,		0 }
};

/* Open and/or preload selected standard libraries. */
LUALIB_API void (luaL_openselectedlibs)(lua_State *L, int load, int preload)
{
  int i, n = (int)(sizeof(lj_lib_load)/sizeof(lj_lib_load[0]) - 1);
  luaL_findtable(L, LUA_REGISTRYINDEX, "_PRELOAD", n);
  for (i = 0; i < n; i++) {
    const char *name = lj_lib_load[i].name;
    lua_CFunction func = lj_lib_load[i].func;
    if (load & lj_lib_load[i].mask) {  /* Open the library. */
      lua_pushcfunction(L, func);
      lua_pushstring(L, name);
      lua_call(L, 1, 0);
    } else if (preload & lj_lib_load[i].mask) {  /* Preload only. */
      lua_pushcfunction(L, func);
      lua_setfield(L, -2, name);
    }
  }
  lua_pop(L, 1);  /* Remove _PRELOAD table. */
}

/* Open all standard libraries (binary compatible alias of the macro). */
LUALIB_API void (luaL_openlibs)(lua_State *L)
{
  luaL_openselectedlibs(L, ~LUA_FFILIBK, LUA_FFILIBK);
}
