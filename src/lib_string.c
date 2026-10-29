/*
** String library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lib_string_c
#define LUA_LIB

#include <limits.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_ff.h"
#include "lj_bcdump.h"
#include "lj_char.h"
#include "lj_strfmt.h"
#include "lj_lib.h"

/* ------------------------------------------------------------------------ */

#define LJLIB_MODULE_string

LJLIB_LUA(string_len) /*
  function(s)
    CHECK_str(s)
    return #s
  end
*/

LJLIB_ASM(string_byte)		LJLIB_REC(string_range 0)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  int32_t len = (int32_t)s->len;
  int32_t start = lj_lib_optint(L, 2, 1);
  int32_t stop = lj_lib_optint(L, 3, start);
  int32_t n, i;
  const unsigned char *p;
  if (stop < 0) stop += len+1;
  if (start < 0) start += len+1;
  if (start <= 0) start = 1;
  if (stop > len) stop = len;
  if (start > stop) return FFH_RES(0);  /* Empty interval: return no results. */
  start--;
  n = stop - start;
  if ((uint32_t)n > LUAI_MAXCSTACK)
    lj_err_caller(L, LJ_ERR_STRSLC);
  lj_state_checkstack(L, (MSize)n);
  p = (const unsigned char *)strdata(s) + start;
  for (i = 0; i < n; i++)
    setintV(L->base + i-1-LJ_FR2, p[i]);
  return FFH_RES(n);
}

LJLIB_ASM(string_char)		LJLIB_REC(.)
{
  int i, nargs = (int)(L->top - L->base);
  char *buf = lj_buf_tmp(L, (MSize)nargs);
  for (i = 1; i <= nargs; i++) {
    int32_t k = lj_lib_checkint(L, i);
    if (!checku8(k))
      lj_err_arg(L, i, LJ_ERR_BADVAL);
    buf[i-1] = (char)k;
  }
  setstrV(L, L->base-1-LJ_FR2, lj_str_new(L, buf, (size_t)nargs));
  return FFH_RES(1);
}

LJLIB_ASM(string_sub)		LJLIB_REC(string_range 1)
{
  lj_lib_checkstr(L, 1);
  lj_lib_checkint(L, 2);
  setintV(L->base+2, lj_lib_optint(L, 3, -1));
  return FFH_RETRY;
}

LJLIB_CF(string_rep)		LJLIB_REC(.)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  int32_t rep = lj_lib_checkint(L, 2);
  GCstr *sep = lj_lib_optstr(L, 3);
  SBuf *sb = lj_buf_tmp_(L);
  if (sep && rep > 1) {
    GCstr *s2 = lj_buf_cat2str(L, sep, s);
    lj_buf_reset(sb);
    lj_buf_putstr(sb, s);
    s = s2;
    rep--;
  }
  sb = lj_buf_putstr_rep(sb, s, rep);
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

LJLIB_ASM(string_reverse)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_reverse)
{
  lj_lib_checkstr(L, 1);
  return FFH_RETRY;
}
LJLIB_ASM_(string_lower)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_lower)
LJLIB_ASM_(string_upper)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_upper)

/* ------------------------------------------------------------------------ */

static int writer_buf(lua_State *L, const void *p, size_t size, void *sb)
{
  lj_buf_putmem((SBuf *)sb, p, (MSize)size);
  UNUSED(L);
  return 0;
}

LJLIB_CF(string_dump)
{
  GCproto *pt = lj_lib_checkLproto(L, 1, 1);
  uint32_t flags = 0;
  SBuf *sb;
  TValue *o = L->base+1;
  if (o < L->top) {
    if (tvisstr(o)) {
      const char *mode = strVdata(o);
      char c;
      while ((c = *mode++)) {
	if (c == 's') flags |= BCDUMP_F_STRIP;
	if (c == 'd') flags |= BCDUMP_F_DETERMINISTIC;
      }
    } else if (tvistruecond(o)) {
      flags |= BCDUMP_F_STRIP;
    }
  }
  sb = lj_buf_tmp_(L);  /* Assumes lj_bcwrite() doesn't use tmpbuf. */
  L->top = L->base+1;
  if (!pt || lj_bcwrite(L, pt, writer_buf, sb, flags))
    lj_err_caller(L, LJ_ERR_STRDUMP);
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

/* macro to `unsign' a character */
#define uchar(c)	((unsigned char)(c))

#define CAP_UNFINISHED	(-1)
#define CAP_POSITION	(-2)

typedef struct MatchState {
  const char *src_init;  /* init of source string */
  const char *src_end;  /* end (`\0') of source string */
  lua_State *L;
  int level;  /* total number of captures (finished or unfinished) */
  int depth;
  struct {
    const char *init;
    ptrdiff_t len;
  } capture[LUA_MAXCAPTURES];
} MatchState;

#define L_ESC		'%'

static int check_capture(MatchState *ms, int l)
{
  l -= '1';
  if (l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED)
    lj_err_caller(ms->L, LJ_ERR_STRCAPI);
  return l;
}

static int capture_to_close(MatchState *ms)
{
  int level = ms->level;
  for (level--; level>=0; level--)
    if (ms->capture[level].len == CAP_UNFINISHED) return level;
  lj_err_caller(ms->L, LJ_ERR_STRPATC);
  return 0;  /* unreachable */
}

static const char *classend(MatchState *ms, const char *p)
{
  switch (*p++) {
  case L_ESC:
    if (*p == '\0')
      lj_err_caller(ms->L, LJ_ERR_STRPATE);
    return p+1;
  case '[':
    if (*p == '^') p++;
    do {  /* look for a `]' */
      if (*p == '\0')
	lj_err_caller(ms->L, LJ_ERR_STRPATM);
      if (*(p++) == L_ESC && *p != '\0')
	p++;  /* skip escapes (e.g. `%]') */
    } while (*p != ']');
    return p+1;
  default:
    return p;
  }
}

static const unsigned char match_class_map[32] = {
  0,LJ_CHAR_ALPHA,0,LJ_CHAR_CNTRL,LJ_CHAR_DIGIT,0,0,LJ_CHAR_GRAPH,0,0,0,0,
  LJ_CHAR_LOWER,0,0,0,LJ_CHAR_PUNCT,0,0,LJ_CHAR_SPACE,0,
  LJ_CHAR_UPPER,0,LJ_CHAR_ALNUM,LJ_CHAR_XDIGIT,0,0,0,0,0,0,0
};

static int match_class(int c, int cl)
{
  if ((cl & 0xc0) == 0x40) {
    int t = match_class_map[(cl&0x1f)];
    if (t) {
      t = lj_char_isa(c, t);
      return (cl & 0x20) ? t : !t;
    }
    if (cl == 'z') return c == 0;
    if (cl == 'Z') return c != 0;
  }
  return (cl == c);
}

static int matchbracketclass(int c, const char *p, const char *ec)
{
  int sig = 1;
  if (*(p+1) == '^') {
    sig = 0;
    p++;  /* skip the `^' */
  }
  while (++p < ec) {
    if (*p == L_ESC) {
      p++;
      if (match_class(c, uchar(*p)))
	return sig;
    }
    else if ((*(p+1) == '-') && (p+2 < ec)) {
      p+=2;
      if (uchar(*(p-2)) <= c && c <= uchar(*p))
	return sig;
    }
    else if (uchar(*p) == c) return sig;
  }
  return !sig;
}

static int singlematch(int c, const char *p, const char *ep)
{
  switch (*p) {
  case '.': return 1;  /* matches any char */
  case L_ESC: return match_class(c, uchar(*(p+1)));
  case '[': return matchbracketclass(c, p, ep-1);
  default:  return (uchar(*p) == c);
  }
}

static const char *match(MatchState *ms, const char *s, const char *p);

static const char *matchbalance(MatchState *ms, const char *s, const char *p)
{
  if (*p == 0 || *(p+1) == 0)
    lj_err_caller(ms->L, LJ_ERR_STRPATU);
  if (*s != *p) {
    return NULL;
  } else {
    int b = *p;
    int e = *(p+1);
    int cont = 1;
    while (++s < ms->src_end) {
      if (*s == e) {
	if (--cont == 0) return s+1;
      } else if (*s == b) {
	cont++;
      }
    }
  }
  return NULL;  /* string ends out of balance */
}

static const char *max_expand(MatchState *ms, const char *s,
			      const char *p, const char *ep)
{
  ptrdiff_t i = 0;  /* counts maximum expand for item */
  while ((s+i)<ms->src_end && singlematch(uchar(*(s+i)), p, ep))
    i++;
  /* keeps trying to match with the maximum repetitions */
  while (i>=0) {
    const char *res = match(ms, (s+i), ep+1);
    if (res) return res;
    i--;  /* else didn't match; reduce 1 repetition to try again */
  }
  return NULL;
}

static const char *min_expand(MatchState *ms, const char *s,
			      const char *p, const char *ep)
{
  for (;;) {
    const char *res = match(ms, s, ep+1);
    if (res != NULL)
      return res;
    else if (s<ms->src_end && singlematch(uchar(*s), p, ep))
      s++;  /* try with one more repetition */
    else
      return NULL;
  }
}

static const char *start_capture(MatchState *ms, const char *s,
				 const char *p, int what)
{
  const char *res;
  int level = ms->level;
  if (level >= LUA_MAXCAPTURES) lj_err_caller(ms->L, LJ_ERR_STRCAPN);
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = level+1;
  if ((res=match(ms, s, p)) == NULL)  /* match failed? */
    ms->level--;  /* undo capture */
  return res;
}

static const char *end_capture(MatchState *ms, const char *s,
			       const char *p)
{
  int l = capture_to_close(ms);
  const char *res;
  ms->capture[l].len = s - ms->capture[l].init;  /* close capture */
  if ((res = match(ms, s, p)) == NULL)  /* match failed? */
    ms->capture[l].len = CAP_UNFINISHED;  /* undo capture */
  return res;
}

static const char *match_capture(MatchState *ms, const char *s, int l)
{
  size_t len;
  l = check_capture(ms, l);
  len = (size_t)ms->capture[l].len;
  if ((size_t)(ms->src_end-s) >= len &&
      memcmp(ms->capture[l].init, s, len) == 0)
    return s+len;
  else
    return NULL;
}

static const char *match(MatchState *ms, const char *s, const char *p)
{
  if (++ms->depth > LJ_MAX_XLEVEL)
    lj_err_caller(ms->L, LJ_ERR_STRPATX);
  init: /* using goto's to optimize tail recursion */
  switch (*p) {
  case '(':  /* start capture */
    if (*(p+1) == ')')  /* position capture? */
      s = start_capture(ms, s, p+2, CAP_POSITION);
    else
      s = start_capture(ms, s, p+1, CAP_UNFINISHED);
    break;
  case ')':  /* end capture */
    s = end_capture(ms, s, p+1);
    break;
  case L_ESC:
    switch (*(p+1)) {
    case 'b':  /* balanced string? */
      s = matchbalance(ms, s, p+2);
      if (s == NULL) break;
      p+=4;
      goto init;  /* else s = match(ms, s, p+4); */
    case 'f': {  /* frontier? */
      const char *ep; char previous;
      p += 2;
      if (*p != '[')
	lj_err_caller(ms->L, LJ_ERR_STRPATB);
      ep = classend(ms, p);  /* points to what is next */
      previous = (s == ms->src_init) ? '\0' : *(s-1);
      if (matchbracketclass(uchar(previous), p, ep-1) ||
	 !matchbracketclass(uchar(*s), p, ep-1)) { s = NULL; break; }
      p=ep;
      goto init;  /* else s = match(ms, s, ep); */
      }
    default:
      if (lj_char_isdigit(uchar(*(p+1)))) {  /* capture results (%0-%9)? */
	s = match_capture(ms, s, uchar(*(p+1)));
	if (s == NULL) break;
	p+=2;
	goto init;  /* else s = match(ms, s, p+2) */
      }
      goto dflt;  /* case default */
    }
    break;
  case '\0':  /* end of pattern */
    break;  /* match succeeded */
  case '$':
    /* is the `$' the last char in pattern? */
    if (*(p+1) != '\0') goto dflt;
    if (s != ms->src_end) s = NULL;  /* check end of string */
    break;
  default: dflt: {  /* it is a pattern item */
    const char *ep = classend(ms, p);  /* points to what is next */
    int m = s<ms->src_end && singlematch(uchar(*s), p, ep);
    switch (*ep) {
    case '?': {  /* optional */
      const char *res;
      if (m && ((res=match(ms, s+1, ep+1)) != NULL)) {
	s = res;
	break;
      }
      p=ep+1;
      goto init;  /* else s = match(ms, s, ep+1); */
      }
    case '*':  /* 0 or more repetitions */
      s = max_expand(ms, s, p, ep);
      break;
    case '+':  /* 1 or more repetitions */
      s = (m ? max_expand(ms, s+1, p, ep) : NULL);
      break;
    case '-':  /* 0 or more repetitions (minimum) */
      s = min_expand(ms, s, p, ep);
      break;
    default:
      if (m) { s++; p=ep; goto init; }  /* else s = match(ms, s+1, ep); */
      s = NULL;
      break;
    }
    break;
    }
  }
  ms->depth--;
  return s;
}

static void push_onecapture(MatchState *ms, int i, const char *s, const char *e)
{
  if (i >= ms->level) {
    if (i == 0)  /* ms->level == 0, too */
      lua_pushlstring(ms->L, s, (size_t)(e - s));  /* add whole match */
    else
      lj_err_caller(ms->L, LJ_ERR_STRCAPI);
  } else {
    ptrdiff_t l = ms->capture[i].len;
    if (l == CAP_UNFINISHED) lj_err_caller(ms->L, LJ_ERR_STRCAPU);
    if (l == CAP_POSITION)
      lua_pushinteger(ms->L, ms->capture[i].init - ms->src_init + 1);
    else
      lua_pushlstring(ms->L, ms->capture[i].init, (size_t)l);
  }
}

static int push_captures(MatchState *ms, const char *s, const char *e)
{
  int i;
  int nlevels = (ms->level == 0 && s) ? 1 : ms->level;
  luaL_checkstack(ms->L, nlevels, "too many captures");
  for (i = 0; i < nlevels; i++)
    push_onecapture(ms, i, s, e);
  return nlevels;  /* number of strings pushed */
}

static int str_find_aux(lua_State *L, int find)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  GCstr *p = lj_lib_checkstr(L, 2);
  int32_t start = lj_lib_optint(L, 3, 1);
  MSize st;
  if (start < 0) start += (int32_t)s->len; else start--;
  if (start < 0) start = 0;
  st = (MSize)start;
  if (st > s->len) {
#if LJ_52
    setnilV(L->top-1);
    return 1;
#else
    st = s->len;
#endif
  }
  if (find && ((L->base+3 < L->top && tvistruecond(L->base+3)) ||
	       !lj_str_haspattern(p))) {  /* Search for fixed string. */
    const char *q = lj_str_find(strdata(s)+st, strdata(p), s->len-st, p->len);
    if (q) {
      setintV(L->top-2, (int32_t)(q-strdata(s)) + 1);
      setintV(L->top-1, (int32_t)(q-strdata(s)) + (int32_t)p->len);
      return 2;
    }
  } else {  /* Search for pattern. */
    MatchState ms;
    const char *pstr = strdata(p);
    const char *sstr = strdata(s) + st;
    int anchor = 0;
    if (*pstr == '^') { pstr++; anchor = 1; }
    ms.L = L;
    ms.src_init = strdata(s);
    ms.src_end = strdata(s) + s->len;
    do {  /* Loop through string and try to match the pattern. */
      const char *q;
      ms.level = ms.depth = 0;
      q = match(&ms, sstr, pstr);
      if (q) {
	if (find) {
	  setintV(L->top++, (int32_t)(sstr-(strdata(s)-1)));
	  setintV(L->top++, (int32_t)(q-strdata(s)));
	  return push_captures(&ms, NULL, NULL) + 2;
	} else {
	  return push_captures(&ms, sstr, q);
	}
      }
    } while (sstr++ < ms.src_end && !anchor);
  }
  setnilV(L->top-1);  /* Not found. */
  return 1;
}

LJLIB_CF(string_find)		LJLIB_REC(.)
{
  return str_find_aux(L, 1);
}

LJLIB_CF(string_match)
{
  return str_find_aux(L, 0);
}

LJLIB_NOREG LJLIB_CF(string_gmatch_aux)
{
  const char *p = strVdata(lj_lib_upvalue(L, 2));
  GCstr *str = strV(lj_lib_upvalue(L, 1));
  const char *s = strdata(str);
  TValue *tvpos = lj_lib_upvalue(L, 3);
  const char *src = s + tvpos->u32.lo;
  MatchState ms;
  ms.L = L;
  ms.src_init = s;
  ms.src_end = s + str->len;
  for (; src <= ms.src_end; src++) {
    const char *e;
    ms.level = ms.depth = 0;
    if ((e = match(&ms, src, p)) != NULL) {
      int32_t pos = (int32_t)(e - s);
      if (e == src) pos++;  /* Ensure progress for empty match. */
      tvpos->u32.lo = (uint32_t)pos;
      return push_captures(&ms, src, e);
    }
  }
  return 0;  /* not found */
}

LJLIB_CF(string_gmatch)
{
  lj_lib_checkstr(L, 1);
  lj_lib_checkstr(L, 2);
  L->top = L->base+3;
  (L->top-1)->u64 = 0;
  lj_lib_pushcc(L, lj_cf_string_gmatch_aux, FF_string_gmatch_aux, 3);
  return 1;
}

static void add_s(MatchState *ms, luaL_Buffer *b, const char *s, const char *e)
{
  size_t l, i;
  const char *news = lua_tolstring(ms->L, 3, &l);
  for (i = 0; i < l; i++) {
    if (news[i] != L_ESC) {
      luaL_addchar(b, news[i]);
    } else {
      i++;  /* skip ESC */
      if (!lj_char_isdigit(uchar(news[i]))) {
	luaL_addchar(b, news[i]);
      } else if (news[i] == '0') {
	luaL_addlstring(b, s, (size_t)(e - s));
      } else {
	push_onecapture(ms, news[i] - '1', s, e);
	luaL_addvalue(b);  /* add capture to accumulated result */
      }
    }
  }
}

static void add_value(MatchState *ms, luaL_Buffer *b,
		      const char *s, const char *e)
{
  lua_State *L = ms->L;
  switch (lua_type(L, 3)) {
    case LUA_TNUMBER:
    case LUA_TSTRING: {
      add_s(ms, b, s, e);
      return;
    }
    case LUA_TFUNCTION: {
      int n;
      lua_pushvalue(L, 3);
      n = push_captures(ms, s, e);
      lua_call(L, n, 1);
      break;
    }
    case LUA_TTABLE: {
      push_onecapture(ms, 0, s, e);
      lua_gettable(L, 3);
      break;
    }
  }
  if (!lua_toboolean(L, -1)) {  /* nil or false? */
    lua_pop(L, 1);
    lua_pushlstring(L, s, (size_t)(e - s));  /* keep original text */
  } else if (!lua_isstring(L, -1)) {
    lj_err_callerv(L, LJ_ERR_STRGSRV, luaL_typename(L, -1));
  }
  luaL_addvalue(b);  /* add result to accumulator */
}

LJLIB_CF(string_gsub)
{
  size_t srcl;
  const char *src = luaL_checklstring(L, 1, &srcl);
  const char *p = luaL_checkstring(L, 2);
  int  tr = lua_type(L, 3);
  int max_s = luaL_optint(L, 4, (int)(srcl+1));
  int anchor = (*p == '^') ? (p++, 1) : 0;
  int n = 0;
  MatchState ms;
  luaL_Buffer b;
  if (!(tr == LUA_TNUMBER || tr == LUA_TSTRING ||
	tr == LUA_TFUNCTION || tr == LUA_TTABLE))
    lj_err_arg(L, 3, LJ_ERR_NOSFT);
  luaL_buffinit(L, &b);
  ms.L = L;
  ms.src_init = src;
  ms.src_end = src+srcl;
  while (n < max_s) {
    const char *e;
    ms.level = ms.depth = 0;
    e = match(&ms, src, p);
    if (e) {
      n++;
      add_value(&ms, &b, src, e);
    }
    if (e && e>src) /* non empty match? */
      src = e;  /* skip it */
    else if (src < ms.src_end)
      luaL_addchar(&b, *src++);
    else
      break;
    if (anchor)
      break;
  }
  luaL_addlstring(&b, src, (size_t)(ms.src_end-src));
  luaL_pushresult(&b);
  lua_pushinteger(L, n);  /* number of substitutions */
  return 2;
}

/* ------------------------------------------------------------------------ */

LJLIB_CF(string_format)		LJLIB_REC(.)
{
  int retry = 0;
  SBuf *sb;
  do {
    sb = lj_buf_tmp_(L);
    retry = lj_strfmt_putarg(L, sb, 1, -retry);
  } while (retry > 0);
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

/* -- string.pack/unpack (ported from Lua 5.5) --------------------------- */

#define PK_PADBYTE	0x00
#define PK_MAXINTSIZE	16
#define PK_NB		CHAR_BIT
#define PK_MC		((1 << PK_NB) - 1)
#define PK_SZINT	((int)sizeof(lua_Integer))
#define PK_MAXSIZE	((size_t)-1)
#define PK_MAXALIGN	union { lua_Number n; double u; void *s; lua_Integer i; long l; }

/* Unsigned counterpart of lua_Integer (same width). */
typedef uintptr_t pk_Unsigned;

/* Dummy union to get native endianness. */
static const union { int dummy; char little; } pk_nativeendian = {1};

/* Information to pack/unpack stuff. */
typedef struct PKHeader {
  lua_State *L;
  int islittle;
  unsigned maxalign;
} PKHeader;

/* Options for pack/unpack. */
typedef enum PKOption {
  PK_int, PK_uint, PK_float, PK_number, PK_double,
  PK_char, PK_string, PK_zstr, PK_padding, PK_paddalign, PK_nop
} PKOption;

static int pk_digit(int c) { return '0' <= c && c <= '9'; }

/* Read an integer numeral from 'fmt' or return 'df'. */
static size_t pk_getnum(const char **fmt, size_t df)
{
  if (!pk_digit(**fmt)) {
    return df;
  } else {
    size_t a = 0;
    do {
      a = a*10 + (size_t)(*((*fmt)++) - '0');
    } while (pk_digit(**fmt) && a <= (PK_MAXSIZE - 9)/10);
    return a;
  }
}

/* Read an integer numeral and check the maximum size of integers. */
static unsigned pk_getnumlimit(PKHeader *h, const char **fmt, size_t df)
{
  size_t sz = pk_getnum(fmt, df);
  if (LJ_UNLIKELY((sz - 1u) >= PK_MAXINTSIZE))
    luaL_error(h->L, "integral size (%d) out of limits [1,%d]",
	       (int)sz, PK_MAXINTSIZE);
  return (unsigned)sz;
}

static void pk_initheader(lua_State *L, PKHeader *h)
{
  h->L = L;
  h->islittle = pk_nativeendian.little;
  h->maxalign = 1;
}

/* Read and classify the next option. */
static PKOption pk_getoption(PKHeader *h, const char **fmt, size_t *size)
{
  struct cD { char c; union { PK_MAXALIGN; } u; };
  int opt = *((*fmt)++);
  *size = 0;
  switch (opt) {
  case 'b': *size = sizeof(char); return PK_int;
  case 'B': *size = sizeof(char); return PK_uint;
  case 'h': *size = sizeof(short); return PK_int;
  case 'H': *size = sizeof(short); return PK_uint;
  case 'l': *size = sizeof(long); return PK_int;
  case 'L': *size = sizeof(long); return PK_uint;
  case 'j': *size = sizeof(lua_Integer); return PK_int;
  case 'J': *size = sizeof(lua_Integer); return PK_uint;
  case 'T': *size = sizeof(size_t); return PK_uint;
  case 'f': *size = sizeof(float); return PK_float;
  case 'n': *size = sizeof(lua_Number); return PK_number;
  case 'd': *size = sizeof(double); return PK_double;
  case 'i': *size = pk_getnumlimit(h, fmt, sizeof(int)); return PK_int;
  case 'I': *size = pk_getnumlimit(h, fmt, sizeof(int)); return PK_uint;
  case 's': *size = pk_getnumlimit(h, fmt, sizeof(size_t)); return PK_string;
  case 'c':
    *size = pk_getnum(fmt, (size_t)-1);
    if (LJ_UNLIKELY(*size == (size_t)-1))
      luaL_error(h->L, "missing size for format option 'c'");
    return PK_char;
  case 'z': return PK_zstr;
  case 'x': *size = 1; return PK_padding;
  case 'X': return PK_paddalign;
  case ' ': break;
  case '<': h->islittle = 1; break;
  case '>': h->islittle = 0; break;
  case '=': h->islittle = pk_nativeendian.little; break;
  case '!': {
      const size_t maxalign = offsetof(struct cD, u);
      h->maxalign = pk_getnumlimit(h, fmt, maxalign);
      break;
    }
  default: luaL_error(h->L, "invalid format option '%c'", opt);
  }
  return PK_nop;
}

/* Read, classify and fill other details about the next option. */
static PKOption pk_getdetails(PKHeader *h, size_t totalsize, const char **fmt,
			      size_t *psize, unsigned *ntoalign)
{
  PKOption opt = pk_getoption(h, fmt, psize);
  size_t align = *psize;
  if (opt == PK_paddalign) {  /* 'X' gets alignment from following option */
    if (**fmt == '\0' || pk_getoption(h, fmt, &align) == PK_char || align == 0)
      luaL_argerror(h->L, 1, "invalid next option for option 'X'");
  }
  if (align <= 1 || opt == PK_char) {
    *ntoalign = 0;
  } else {
    if (align > h->maxalign)
      align = h->maxalign;
    if (LJ_UNLIKELY((align & (align - 1)) != 0)) {
      *ntoalign = 0;
      luaL_argerror(h->L, 1, "format asks for alignment not power of 2");
    } else {
      unsigned szmoda = (unsigned)(totalsize & (align - 1));
      *ntoalign = (unsigned)((align - szmoda) & (align - 1));
    }
  }
  return opt;
}

/* Pack integer 'n' with 'size' bytes and 'islittle' endianness. */
static void pk_packint(luaL_Buffer *b, pk_Unsigned n,
		       int islittle, unsigned size, int neg)
{
  char buff[PK_MAXINTSIZE];
  unsigned i;
  buff[islittle ? 0 : size - 1] = (char)(n & PK_MC);
  for (i = 1; i < size; i++) {
    n >>= PK_NB;
    buff[islittle ? i : size - 1 - i] = (char)(n & PK_MC);
  }
  if (neg && size > (unsigned)PK_SZINT) {  /* sign extension? */
    for (i = (unsigned)PK_SZINT; i < size; i++)
      buff[islittle ? i : size - 1 - i] = (char)PK_MC;
  }
  luaL_addlstring(b, buff, size);
}

/* Copy 'size' bytes correcting endianness. */
static void pk_copywithendian(char *dest, const char *src,
			      unsigned size, int islittle)
{
  if (islittle == pk_nativeendian.little) {
    memcpy(dest, src, size);
  } else {
    dest += size - 1;
    while (size-- != 0)
      *(dest--) = *(src++);
  }
}

/* Check for an integer argument (same semantics as Lua 5.5). */
static lua_Integer pk_checkinteger(lua_State *L, int arg)
{
  lua_Number d = luaL_checknumber(L, arg);
  lua_Integer n;
  if (!(d == d) || !(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
    return (lua_Integer)luaL_argerror(L, arg, "number has no integer representation");
  n = (lua_Integer)d;
  if ((lua_Number)n != d)
    return (lua_Integer)luaL_argerror(L, arg, "number has no integer representation");
  return n;
}

/* Translate a relative string position for unpack. */
static size_t pk_posrelatI(lua_Integer pos, size_t len)
{
  if (pos > 0) return (size_t)pos;
  else if (pos < -(lua_Integer)len) return 1;
  else return len + (size_t)pos + 1;
}

static int str_pack(lua_State *L)
{
  luaL_Buffer b;
  PKHeader h;
  const char *fmt = luaL_checkstring(L, 1);
  int arg = 1;
  size_t totalsize = 0;
  pk_initheader(L, &h);
  lua_pushnil(L);  /* mark to separate arguments from string buffer */
  luaL_buffinit(L, &b);
  while (*fmt != '\0') {
    unsigned ntoalign;
    size_t size;
    PKOption opt = pk_getdetails(&h, totalsize, &fmt, &size, &ntoalign);
    luaL_argcheck(L, size + ntoalign <= PK_MAXSIZE - totalsize, arg,
		  "result too long");
    totalsize += ntoalign + size;
    while (ntoalign-- > 0)
      luaL_addchar(&b, PK_PADBYTE);  /* fill alignment */
    arg++;
    switch (opt) {
    case PK_int: {
      lua_Integer n = pk_checkinteger(L, arg);
      if (size < (size_t)PK_SZINT) {  /* need overflow check? */
	lua_Integer lim = (lua_Integer)1 << ((size * PK_NB) - 1);
	luaL_argcheck(L, -lim <= n && n < lim, arg, "integer overflow");
      }
      pk_packint(&b, (pk_Unsigned)n, h.islittle, (unsigned)size, (n < 0));
      break;
    }
    case PK_uint: {
      lua_Integer n = pk_checkinteger(L, arg);
      if (size < (size_t)PK_SZINT)
	luaL_argcheck(L, (pk_Unsigned)n < ((pk_Unsigned)1 << (size * PK_NB)),
		      arg, "unsigned overflow");
      pk_packint(&b, (pk_Unsigned)n, h.islittle, (unsigned)size, 0);
      break;
    }
    case PK_float: {
      float f = (float)luaL_checknumber(L, arg);
      char buff[sizeof(f)];
      pk_copywithendian(buff, (const char *)&f, sizeof(f), h.islittle);
      luaL_addlstring(&b, buff, sizeof(f));
      break;
    }
    case PK_number: {
      lua_Number f = luaL_checknumber(L, arg);
      char buff[sizeof(f)];
      pk_copywithendian(buff, (const char *)&f, sizeof(f), h.islittle);
      luaL_addlstring(&b, buff, sizeof(f));
      break;
    }
    case PK_double: {
      double f = (double)luaL_checknumber(L, arg);
      char buff[sizeof(f)];
      pk_copywithendian(buff, (const char *)&f, sizeof(f), h.islittle);
      luaL_addlstring(&b, buff, sizeof(f));
      break;
    }
    case PK_char: {
      size_t len;
      const char *s = luaL_checklstring(L, arg, &len);
      luaL_argcheck(L, len <= size, arg, "string longer than given size");
      luaL_addlstring(&b, s, len);
      while (len < size) {  /* pad with zeros */
	luaL_addchar(&b, PK_PADBYTE);
	len++;
      }
      break;
    }
    case PK_string: {
      size_t len;
      const char *s = luaL_checklstring(L, arg, &len);
      luaL_argcheck(L, size >= sizeof(pk_Unsigned) ||
		       len < ((pk_Unsigned)1 << (size * PK_NB)),
		    arg, "string length does not fit in given size");
      pk_packint(&b, (pk_Unsigned)len, h.islittle, (unsigned)size, 0);
      luaL_addlstring(&b, s, len);
      totalsize += len;
      break;
    }
    case PK_zstr: {
      size_t len;
      const char *s = luaL_checklstring(L, arg, &len);
      luaL_argcheck(L, strlen(s) == len, arg, "string contains zeros");
      luaL_addlstring(&b, s, len);
      luaL_addchar(&b, '\0');
      totalsize += len + 1;
      break;
    }
    case PK_padding: luaL_addchar(&b, PK_PADBYTE);  /* fallthrough */
    case PK_paddalign: case PK_nop:
      arg--;  /* undo increment */
      break;
    }
  }
  luaL_pushresult(&b);
  return 1;
}

static int str_packsize(lua_State *L)
{
  PKHeader h;
  const char *fmt = luaL_checkstring(L, 1);
  size_t totalsize = 0;
  pk_initheader(L, &h);
  while (*fmt != '\0') {
    unsigned ntoalign;
    size_t size;
    PKOption opt = pk_getdetails(&h, totalsize, &fmt, &size, &ntoalign);
    luaL_argcheck(L, opt != PK_string && opt != PK_zstr, 1,
		  "variable-length format");
    size += ntoalign;
    luaL_argcheck(L, totalsize <= PK_MAXSIZE - size, 1,
		  "format result too large");
    totalsize += size;
  }
  lua_pushinteger(L, (lua_Integer)totalsize);
  return 1;
}

/* Unpack an integer with 'size' bytes and 'islittle' endianness. */
static lua_Integer pk_unpackint(lua_State *L, const char *str,
				int islittle, int size, int issigned)
{
  pk_Unsigned res = 0;
  int i;
  int limit = (size <= PK_SZINT) ? size : PK_SZINT;
  for (i = limit - 1; i >= 0; i--) {
    res <<= PK_NB;
    res |= (pk_Unsigned)(unsigned char)str[islittle ? i : size - 1 - i];
  }
  if (size < PK_SZINT) {  /* real size smaller than lua_Integer? */
    if (issigned) {  /* needs sign extension? */
      pk_Unsigned mask = (pk_Unsigned)1 << (size*PK_NB - 1);
      res = ((res ^ mask) - mask);  /* do sign extension */
    }
  } else if (size > PK_SZINT) {  /* must check unread bytes */
    int mask = (!issigned || (lua_Integer)res >= 0) ? 0 : PK_MC;
    for (i = limit; i < size; i++) {
      if (LJ_UNLIKELY((unsigned char)str[islittle ? i : size - 1 - i] != mask))
	luaL_error(L, "%d-byte integer does not fit into Lua Integer", size);
    }
  }
  return (lua_Integer)res;
}

static int str_unpack(lua_State *L)
{
  PKHeader h;
  const char *fmt = luaL_checkstring(L, 1);
  size_t ld;
  const char *data = luaL_checklstring(L, 2, &ld);
  size_t pos = pk_posrelatI(luaL_optinteger(L, 3, 1), ld) - 1;
  int n = 0;  /* number of results */
  luaL_argcheck(L, pos <= ld, 3, "initial position out of string");
  pk_initheader(L, &h);
  while (*fmt != '\0') {
    unsigned ntoalign;
    size_t size;
    PKOption opt = pk_getdetails(&h, pos, &fmt, &size, &ntoalign);
    luaL_argcheck(L, ntoalign + size <= ld - pos, 2,
		  "data string too short");
    pos += ntoalign;  /* skip alignment */
    luaL_checkstack(L, 2, "too many results");  /* item + next position */
    n++;
    switch (opt) {
    case PK_int:
    case PK_uint: {
      lua_Integer res = pk_unpackint(L, data + pos, h.islittle,
				     (int)size, (opt == PK_int));
      lua_pushinteger(L, res);
      break;
    }
    case PK_float: {
      float f;
      pk_copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
      lua_pushnumber(L, (lua_Number)f);
      break;
    }
    case PK_number: {
      lua_Number f;
      pk_copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
      lua_pushnumber(L, f);
      break;
    }
    case PK_double: {
      double f;
      pk_copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
      lua_pushnumber(L, (lua_Number)f);
      break;
    }
    case PK_char: {
      lua_pushlstring(L, data + pos, size);
      break;
    }
    case PK_string: {
      pk_Unsigned len = (pk_Unsigned)pk_unpackint(L, data + pos,
						  h.islittle, (int)size, 0);
      luaL_argcheck(L, len <= ld - pos - size, 2, "data string too short");
      lua_pushlstring(L, data + pos + size, (size_t)len);
      pos += (size_t)len;  /* skip string */
      break;
    }
    case PK_zstr: {
      size_t len = strlen(data + pos);
      luaL_argcheck(L, pos + len < ld, 2, "unfinished string for format 'z'");
      lua_pushlstring(L, data + pos, len);
      pos += len + 1;  /* skip string plus final '\0' */
      break;
    }
    case PK_paddalign: case PK_padding: case PK_nop:
      n--;  /* undo increment */
      break;
    }
    pos += size;
  }
  lua_pushinteger(L, (lua_Integer)pos + 1);  /* next position */
  return n + 1;
}

#include "lj_libdef.h"

LUALIB_API int luaopen_string(lua_State *L)
{
  GCtab *mt;
  global_State *g;
  LJ_LIB_REG(L, LUA_STRLIBNAME, string);
  lua_pushcfunction(L, str_pack);
  lua_setfield(L, -2, "pack");
  lua_pushcfunction(L, str_packsize);
  lua_setfield(L, -2, "packsize");
  lua_pushcfunction(L, str_unpack);
  lua_setfield(L, -2, "unpack");
  mt = lj_tab_new(L, 0, 1);
  /* NOBARRIER: basemt is a GC root. */
  g = G(L);
  setgcref(basemt_it(g, LJ_TSTR), obj2gco(mt));
  settabV(L, lj_tab_setstr(L, mt, mmname_str(g, MM_index)), tabV(L->top-1));
  mt->nomm = (uint8_t)(~(1u<<MM_index));
#if LJ_HASBUFFER
  lj_lib_prereg(L, LUA_STRLIBNAME ".buffer", luaopen_string_buffer, tabV(L->top-1));
#endif
  return 1;
}

