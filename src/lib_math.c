/*
** Math library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <math.h>
#include <stdio.h>

#define lib_math_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_lib.h"
#include "lj_vm.h"
#include "lj_prng.h"
#include "lj_strscan.h"

/* ------------------------------------------------------------------------ */

#define LJLIB_MODULE_math

LJLIB_ASM(math_abs)		LJLIB_REC(.)
{
  lj_lib_checknumber(L, 1);
  return FFH_RETRY;
}
LJLIB_ASM_(math_floor)		LJLIB_REC(math_round IRFPM_FLOOR)
LJLIB_ASM_(math_ceil)		LJLIB_REC(math_round IRFPM_CEIL)

LJLIB_ASM(math_sqrt)		LJLIB_REC(math_unary IRFPM_SQRT)
{
  lj_lib_checknum(L, 1);
  return FFH_RETRY;
}
LJLIB_ASM_(math_log10)		LJLIB_REC(math_call IRCALL_log10)
LJLIB_ASM_(math_exp)		LJLIB_REC(math_call IRCALL_exp)
LJLIB_ASM_(math_sin)		LJLIB_REC(math_call IRCALL_sin)
LJLIB_ASM_(math_cos)		LJLIB_REC(math_call IRCALL_cos)
LJLIB_ASM_(math_tan)		LJLIB_REC(math_call IRCALL_tan)
LJLIB_ASM_(math_asin)		LJLIB_REC(math_call IRCALL_asin)
LJLIB_ASM_(math_acos)		LJLIB_REC(math_call IRCALL_acos)
LJLIB_ASM_(math_atan)		LJLIB_REC(math_call IRCALL_atan)
LJLIB_ASM_(math_sinh)		LJLIB_REC(math_call IRCALL_sinh)
LJLIB_ASM_(math_cosh)		LJLIB_REC(math_call IRCALL_cosh)
LJLIB_ASM_(math_tanh)		LJLIB_REC(math_call IRCALL_tanh)
LJLIB_ASM_(math_frexp)
LJLIB_ASM_(math_modf)

LJLIB_ASM(math_log)		LJLIB_REC(math_log)
{
  double x = lj_lib_checknum(L, 1);
  if (L->base+1 < L->top) {
    double y = lj_lib_checknum(L, 2);
#ifdef LUAJIT_NO_LOG2
    x = log(x); y = 1.0 / log(y);
#else
    x = lj_vm_log2(x); y = 1.0 / lj_vm_log2(y);
#endif
    setnumV(L->base-1-LJ_FR2, x*y);  /* Do NOT join the expression to x / y. */
    return FFH_RES(1);
  }
  return FFH_RETRY;
}

LJLIB_LUA(math_deg) /* function(x) return x * 57.29577951308232 end */
LJLIB_LUA(math_rad) /* function(x) return x * 0.017453292519943295 end */

LJLIB_ASM(math_atan2)		LJLIB_REC(.)
{
  lj_lib_checknum(L, 1);
  lj_lib_checknum(L, 2);
  return FFH_RETRY;
}
LJLIB_ASM_(math_pow)		LJLIB_REC(.)
LJLIB_ASM_(math_fmod)

LJLIB_ASM(math_ldexp)		LJLIB_REC(.)
{
  lj_lib_checknum(L, 1);
#if LJ_DUALNUM && !LJ_TARGET_X86ORX64
  lj_lib_checkint(L, 2);
#else
  lj_lib_checknum(L, 2);
#endif
  return FFH_RETRY;
}

LJLIB_ASM(math_min)		LJLIB_REC(math_minmax IR_MIN)
{
  int i = 0;
  do { lj_lib_checknumber(L, ++i); } while (L->base+i < L->top);
  return FFH_RETRY;
}
LJLIB_ASM_(math_max)		LJLIB_REC(math_minmax IR_MAX)

/* Check for an integral value in the range of a 64 bit integer. */
static uint64_t math_checkuint64(lua_State *L, int narg)
{
  double d = lj_lib_checknum(L, narg);
  if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0) ||
      d != floor(d))
    luaL_argerror(L, narg, "number has no integer representation");
  return (uint64_t)(int64_t)d;
}

/* Unsigned comparison of two integers (Lua 5.5). */
LJLIB_CF(math_ult)		LJLIB_REC(.)
{
  uint64_t a = math_checkuint64(L, 1);
  uint64_t b = math_checkuint64(L, 2);
  setboolV(L->top++, a < b);
  return 1;
}

/* Return "integer"/"float" for numbers and nil for other values (Lua 5.5).
** This is representation-based: in a build without integer values (the
** default on x64), every number is a float.
*/
LJLIB_CF(math_type)		LJLIB_REC(.)
{
  TValue *o;
  luaL_checkany(L, 1);  /* Error if no argument. */
  o = L->base;
  if (tvisint(o)) {
    lua_pushliteral(L, "integer");
  } else if (tvisnum(o)) {
    lua_pushliteral(L, "float");
  } else {
    setnilV(L->top++);
  }
  return 1;
}

/* Return the integral value of a number or numeric string, or nil (Lua 5.5).
** Numbers are doubles here, so the value is returned unchanged if it is an
** exact integer in the range of a 64 bit signed integer.
*/
LJLIB_CF(math_tointeger)	LJLIB_REC(.)
{
  TValue *o, tmp;
  double d;
  luaL_checkany(L, 1);  /* Error if no argument. */
  o = L->base;
  if (tvisint(o)) {  /* Dual-number build: already an integer. */
    setintV(L->top++, intV(o));
    return 1;
  }
  if (!tvisnum(o)) {  /* Try a numeric string, like lua_tointegerx(). */
    if (!tvisstr(o) || !lj_strscan_num(strV(o), &tmp)) {
      setnilV(L->top++);
      return 1;
    }
    o = &tmp;
  }
  d = numV(o);
  if (d == floor(d) &&
      d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
    lua_pushinteger(L, (lua_Integer)d);
  } else {
    setnilV(L->top++);
  }
  return 1;
}

LJLIB_PUSH(3.14159265358979323846) LJLIB_SET(pi)
LJLIB_PUSH(1e310) LJLIB_SET(huge)

/* ------------------------------------------------------------------------ */

/* This implements a Tausworthe PRNG with period 2^223. Based on:
**   Tables of maximally-equidistributed combined LFSR generators,
**   Pierre L'Ecuyer, 1991, table 3, 1st entry.
** Full-period ME-CF generator with L=64, J=4, k=223, N1=49.
*/

/* Union needed for bit-pattern conversion between uint64_t and double. */
typedef union { uint64_t u64; double d; } U64double;

/* PRNG seeding function. */
static void random_seed(PRNGState *rs, double d)
{
  uint32_t r = 0x11090601;  /* 64-k[i] as four 8 bit constants. */
  int i;
  for (i = 0; i < 4; i++) {
    U64double u;
    uint32_t m = 1u << (r&255);
    r >>= 8;
    u.d = d = d * 3.14159265358979323846 + 2.7182818284590452354;
    if (u.u64 < m) u.u64 += m;  /* Ensure k[i] MSB of u[i] are non-zero. */
    rs->u[i] = u.u64;
  }
  for (i = 0; i < 10; i++)
    (void)lj_prng_u64(rs);
}

/* PRNG extract function. */
LJLIB_PUSH(top-2)  /* Upvalue holds userdata with PRNGState. */
LJLIB_CF(math_random)		LJLIB_REC(.)
{
  int n = (int)(L->top - L->base);
  PRNGState *rs = (PRNGState *)(uddata(udataV(lj_lib_upvalue(L, 1))));
  U64double u;
  double d;
  u.u64 = lj_prng_u64d(rs);
  d = u.d - 1.0;
  if (n > 0) {
#if LJ_DUALNUM
    int isint = 1;
    double r1;
    lj_lib_checknumber(L, 1);
    if (tvisint(L->base)) {
      r1 = (lua_Number)intV(L->base);
    } else {
      isint = 0;
      r1 = numV(L->base);
    }
#else
    double r1 = lj_lib_checknum(L, 1);
#endif
    if (n == 1) {
      d = lj_vm_floor(d*r1) + 1.0;  /* d is an int in range [1, r1] */
    } else {
#if LJ_DUALNUM
      double r2;
      lj_lib_checknumber(L, 2);
      if (tvisint(L->base+1)) {
	r2 = (lua_Number)intV(L->base+1);
      } else {
	isint = 0;
	r2 = numV(L->base+1);
      }
#else
      double r2 = lj_lib_checknum(L, 2);
#endif
      d = lj_vm_floor(d*(r2-r1+1.0)) + r1;  /* d is an int in range [r1, r2] */
    }
#if LJ_DUALNUM
    if (isint) {
      setintV(L->top-1, lj_num2int(d));
      return 1;
    }
#endif
  }  /* else: d is a double in range [0, 1] */
  setnumV(L->top++, d);
  return 1;
}

/* PRNG seed function. */
LJLIB_PUSH(top-2)  /* Upvalue holds userdata with PRNGState. */
LJLIB_CF(math_randomseed)
{
  PRNGState *rs = (PRNGState *)(uddata(udataV(lj_lib_upvalue(L, 1))));
  if (L->base != L->top)
    random_seed(rs, lj_lib_checknum(L, 1));
  else if (!lj_prng_seed_secure(rs))
    lj_err_caller(L, LJ_ERR_PRNGSD);
  return 0;
}

/* -- Deprecated functions ------------------------------------------------ */

/* These functions were removed in Lua 5.5 (LUA_COMPAT_MATHLIB is gone).
** They are kept for compatibility, but print a one-time deprecation
** warning with a suggestion for the replacement.
*/
static const struct {
  const char *name;
  const char *msg;
} math_deprecated[] = {
  { "pow", "math.pow is deprecated: use the '^' operator instead" },
  { "log10", "math.log10 is deprecated: use math.log(x, 10) instead" },
  { "atan2", "math.atan2 is deprecated: use math.atan(y, x) instead" },
  { "sinh", "math.sinh is deprecated: it is not available in Lua 5.5" },
  { "cosh", "math.cosh is deprecated: it is not available in Lua 5.5" },
  { "tanh", "math.tanh is deprecated: it is not available in Lua 5.5" },
};

/* Emit a deprecation warning through the Lua warning system. */
static int math_depwarn(lua_State *L)
{
  lua_warning(L, luaL_checkstring(L, 1), 0);
  return 0;
}

/* Warn once, then call the original function. The warning branch is only
** taken by the first (interpreted) call, so it is never recorded in a trace.
*/
static const char math_depwrapper[] =
  "local f, msg, warn = ...\n"
  "return function(...)\n"
  "  if msg then local m = msg; msg = nil; warn(m) end\n"
  "  return f(...)\n"
  "end\n";

/* Wrap the deprecated functions in the math table. */
static void math_wrap_deprecated(lua_State *L)
{
  MSize i, n = sizeof(math_deprecated)/sizeof(math_deprecated[0]);
  int ch;
  if (luaL_loadstring(L, math_depwrapper) != 0) {
    lua_pop(L, 1);  /* Ignore: keep the unwrapped functions. */
    return;
  }
  ch = lua_gettop(L);  /* Stack index of the wrapper chunk. */
  for (i = 0; i < n; i++) {
    lua_pushvalue(L, ch);
    lua_getfield(L, ch-1, math_deprecated[i].name);
    lua_pushstring(L, math_deprecated[i].msg);
    lua_pushcfunction(L, math_depwarn);
    lua_call(L, 3, 1);  /* -> wrapper closure */
    lua_setfield(L, ch-1, math_deprecated[i].name);
  }
  lua_remove(L, ch);
}

/* -- Lua 5.5 compatibility ----------------------------------------------- */

/* math.atan(y [, x]) with two arguments (Lua 5.5). The two-argument form is
** an alias for math.atan2(). Install before wrapping the deprecated
** functions, so that the raw atan2() is used and no warning is emitted.
*/
static const char math_atanwrapper[] =
  "local atan, atan2 = ...\n"
  "return function(y, x)\n"
  "  if x == nil then return atan(y) end\n"
  "  return atan2(y, x)\n"
  "end\n";

static void math_wrap_atan(lua_State *L)
{
  int top = lua_gettop(L);  /* Stack index of the math table. */
  if (luaL_loadstring(L, math_atanwrapper) != 0) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, top, "atan");
  lua_getfield(L, top, "atan2");
  lua_call(L, 2, 1);  /* Consumes the chunk, yields the wrapper closure. */
  lua_setfield(L, top, "atan");
}

/* Add the integer limits. These are approximate: a dual-number build uses
** the int32 range, a double-only build uses the largest contiguous exact
** integer range (2^53 - 1 .. -2^53).
*/
static void math_setlimits(lua_State *L)
{
#if LJ_DUALNUM
  lua_pushnumber(L, 2147483647.0);
  lua_setfield(L, -2, "maxinteger");
  lua_pushnumber(L, -2147483648.0);
  lua_setfield(L, -2, "mininteger");
#else
  lua_pushnumber(L, 9007199254740991.0);
  lua_setfield(L, -2, "maxinteger");
  lua_pushnumber(L, -9007199254740992.0);
  lua_setfield(L, -2, "mininteger");
#endif
}

/* ------------------------------------------------------------------------ */

#include "lj_libdef.h"

LUALIB_API int luaopen_math(lua_State *L)
{
  PRNGState *rs = (PRNGState *)lua_newuserdata(L, sizeof(PRNGState));
  lj_prng_seed_fixed(rs);
  LJ_LIB_REG(L, LUA_MATHLIBNAME, math);
  math_setlimits(L);
  math_wrap_atan(L);
  math_wrap_deprecated(L);
  return 1;
}

