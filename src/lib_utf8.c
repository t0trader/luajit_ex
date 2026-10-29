/*
** UTF-8 library.
** Ported from the Lua 5.5 standard library (see Copyright Notice in lua.h).
*/

#define lib_utf8_c
#define LUA_LIB

#include <limits.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_def.h"

#define UTF_MAXUNICODE	0x10FFFFu
#define UTF_MAX		0x7FFFFFFFu

#define UTF_MSGINVALID	"invalid UTF-8 code"

#define utf_iscont(c)	(((c) & 0xC0) == 0x80)
#define utf_iscontp(p)	utf_iscont(*(p))

/* Unsigned counterpart of lua_Integer (same width). */
typedef uintptr_t lua_Unsigned;

/* Translate a relative string position: negative means back from end. */
static lua_Integer u_posrelat(lua_Integer pos, size_t len)
{
  if (pos >= 0) {
    return pos;
  } else if (0u - (size_t)pos > len) {
    return 0;
  } else {
    return (lua_Integer)len + pos + 1;
  }
}

/* Decode one UTF-8 sequence, returning NULL if the byte sequence is
** invalid. The array 'limits' stores the minimum value for each sequence
** length, to check for overlong representations.
*/
static const char *utf8_decode(const char *s, uint32_t *val, int strict)
{
  static const uint32_t limits[] =
    {~(uint32_t)0, 0x80, 0x800, 0x10000u, 0x200000u, 0x4000000u};
  unsigned int c = (unsigned char)s[0];
  uint32_t res = 0;  /* final result */
  if (c < 0x80) {  /* ASCII? */
    res = c;
  } else if (c >= 0xfe) {  /* c >= 1111 1110b ? */
    return NULL;  /* would need six or more continuation bytes */
  } else {
    int count = 0;  /* to count number of continuation bytes */
    for (; c & 0x40; c <<= 1) {  /* while it needs continuation bytes... */
      unsigned int cc = (unsigned char)s[++count];  /* read next byte */
      if (!utf_iscont(cc))  /* not a continuation byte? */
	return NULL;  /* invalid byte sequence */
      res = (res << 6) | (cc & 0x3F);  /* add lower 6 bits from cont. byte */
    }
    lua_assert(count <= 5);
    res |= ((uint32_t)(c & 0x7F) << (count * 5));  /* add first byte */
    if (res > UTF_MAX || res < limits[count])
      return NULL;  /* invalid byte sequence */
    s += count;  /* skip continuation bytes read */
  }
  if (strict) {  /* check for invalid code points; too large or surrogates */
    if (res > UTF_MAXUNICODE || (0xD800u <= res && res <= 0xDFFFu))
      return NULL;
  }
  if (val) *val = res;
  return s + 1;  /* +1 to include first byte */
}

/* utf8.len(s [, i [, j [, lax]]]) -> number of characters, or nil + pos. */
static int utflen(lua_State *L)
{
  lua_Integer n = 0;  /* counter for the number of characters */
  size_t len;  /* string length in bytes */
  const char *s = luaL_checklstring(L, 1, &len);
  lua_Integer posi = u_posrelat(luaL_optinteger(L, 2, 1), len);
  lua_Integer posj = u_posrelat(luaL_optinteger(L, 3, -1), len);
  int lax = lua_toboolean(L, 4);
  luaL_argcheck(L, 1 <= posi && --posi <= (lua_Integer)len, 2,
		"initial position out of bounds");
  luaL_argcheck(L, --posj < (lua_Integer)len, 3,
		"final position out of bounds");
  while (posi <= posj) {
    const char *s1 = utf8_decode(s + posi, NULL, !lax);
    if (s1 == NULL) {  /* conversion error? */
      lua_pushnil(L);  /* return fail ... */
      lua_pushinteger(L, posi + 1);  /* ... and current position */
      return 2;
    }
    posi = (lua_Integer)(s1 - s);
    n++;
  }
  lua_pushinteger(L, n);
  return 1;
}

/* utf8.codepoint(s [, i [, j [, lax]]]) -> codepoints in range [i,j]. */
static int codepoint(lua_State *L)
{
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  lua_Integer posi = u_posrelat(luaL_optinteger(L, 2, 1), len);
  lua_Integer pose = u_posrelat(luaL_optinteger(L, 3, posi), len);
  int lax = lua_toboolean(L, 4);
  int n;
  const char *se;
  luaL_argcheck(L, posi >= 1, 2, "out of bounds");
  luaL_argcheck(L, pose <= (lua_Integer)len, 3, "out of bounds");
  if (posi > pose) return 0;  /* empty interval; return no values */
  if (pose - posi >= INT_MAX)  /* (lua_Integer -> int) overflow? */
    return luaL_error(L, "string slice too long");
  n = (int)(pose - posi) + 1;  /* upper bound for number of returns */
  luaL_checkstack(L, n, "string slice too long");
  n = 0;  /* count the number of returns */
  se = s + pose;  /* string end */
  for (s += posi - 1; s < se;) {
    uint32_t code;
    s = utf8_decode(s, &code, !lax);
    if (s == NULL)
      return luaL_error(L, UTF_MSGINVALID);
    lua_pushinteger(L, (lua_Integer)code);
    n++;
  }
  return n;
}

/* Encode one code point as UTF-8 and push it (same as Lua's "%U"). */
static void pushutfchar(lua_State *L, int arg)
{
  char buff[8];
  int n = 1;  /* number of bytes put in the buffer (backwards) */
  lua_Unsigned code = (lua_Unsigned)luaL_checkinteger(L, arg);
  luaL_argcheck(L, code <= UTF_MAX, arg, "value out of range");
  if (code < 0x80) {
    buff[8 - 1] = (char)code;
  } else {  /* need continuation bytes */
    unsigned int mfb = 0x3f;  /* maximum that fits in first byte */
    do {  /* add continuation bytes */
      buff[8 - (n++)] = (char)(0x80 | (code & 0x3f));
      code >>= 6;  /* remove added bits */
      mfb >>= 1;  /* one less bit available in first byte */
    } while (code > mfb);  /* still needs continuation bytes? */
    buff[8 - n] = (char)((~mfb << 1) | code);  /* add first byte */
  }
  lua_pushlstring(L, &buff[8 - n], (size_t)n);
}

/* utf8.char(n1, n2, ...) -> char(n1)..char(n2)... */
static int utfchar(lua_State *L)
{
  int n = lua_gettop(L);  /* number of arguments */
  if (n == 1) {  /* optimize common case of single char */
    pushutfchar(L, 1);
  } else {
    int i;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (i = 1; i <= n; i++) {
      pushutfchar(L, i);
      luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
  }
  return 1;
}

/* utf8.offset(s, n, [i]) -> initial and final positions of the n-th char. */
static int byteoffset(lua_State *L)
{
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  lua_Integer n = luaL_checkinteger(L, 2);
  lua_Integer posi = (n >= 0) ? 1 : (lua_Integer)len + 1;
  posi = u_posrelat(luaL_optinteger(L, 3, posi), len);
  luaL_argcheck(L, 1 <= posi && --posi <= (lua_Integer)len, 3,
		"position out of bounds");
  if (n == 0) {  /* find beginning of current byte sequence */
    while (posi > 0 && utf_iscontp(s + posi)) posi--;
  } else {
    if (utf_iscontp(s + posi))
      return luaL_error(L, "initial position is a continuation byte");
    if (n < 0) {
      while (n < 0 && posi > 0) {  /* move back */
	do {  /* find beginning of previous character */
	  posi--;
	} while (posi > 0 && utf_iscontp(s + posi));
	n++;
      }
    } else {
      n--;  /* do not move for 1st character */
      while (n > 0 && posi < (lua_Integer)len) {
	do {  /* find beginning of next character */
	  posi++;
	} while (utf_iscontp(s + posi));  /* (cannot pass final '\0') */
	n--;
      }
    }
  }
  if (n != 0) {  /* did not find given character? */
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, posi + 1);  /* initial position */
  if ((s[posi] & 0x80) != 0) {  /* multi-byte character? */
    if (utf_iscont(s[posi]))
      return luaL_error(L, "initial position is a continuation byte");
    while (utf_iscontp(s + posi + 1))
      posi++;  /* skip to last continuation byte */
  }
  lua_pushinteger(L, posi + 1);  /* final position */
  return 2;
}

static int iter_aux(lua_State *L, int strict)
{
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  lua_Unsigned n = (lua_Unsigned)lua_tointeger(L, 2);
  if (n < len) {
    while (utf_iscontp(s + n)) n++;  /* go to next character */
  }
  if (n >= len) {  /* (also handles original 'n' being negative) */
    return 0;  /* no more codepoints */
  } else {
    uint32_t code;
    const char *next = utf8_decode(s + n, &code, strict);
    if (next == NULL || utf_iscontp(next))
      return luaL_error(L, UTF_MSGINVALID);
    lua_pushinteger(L, (lua_Integer)(n + 1));
    lua_pushinteger(L, (lua_Integer)code);
    return 2;
  }
}

static int iter_auxstrict(lua_State *L)
{
  return iter_aux(L, 1);
}

static int iter_auxlax(lua_State *L)
{
  return iter_aux(L, 0);
}

static int iter_codes(lua_State *L)
{
  int lax = lua_toboolean(L, 2);
  const char *s = luaL_checkstring(L, 1);
  luaL_argcheck(L, !utf_iscontp(s), 1, UTF_MSGINVALID);
  lua_pushcfunction(L, lax ? iter_auxlax : iter_auxstrict);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 0);
  return 3;
}

/* Pattern to match a single UTF-8 character. */
#define UTF8PATT	"[\0-\x7F\xC2-\xFD][\x80-\xBF]*"

static const luaL_Reg utflib_funcs[] = {
  {"offset", byteoffset},
  {"codepoint", codepoint},
  {"char", utfchar},
  {"len", utflen},
  {"codes", iter_codes},
  {NULL, NULL}
};

LUALIB_API int luaopen_utf8(lua_State *L)
{
  luaL_register(L, LUA_UTF8LIBNAME, utflib_funcs);
  lua_pushlstring(L, UTF8PATT, sizeof(UTF8PATT)/sizeof(char) - 1);
  lua_setfield(L, -2, "charpattern");
  return 1;
}
