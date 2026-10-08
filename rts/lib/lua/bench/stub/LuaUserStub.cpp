// Stub of Recoil's LuaUser.cpp for the standalone VM harness.
// Mutex hooks are empty out-of-line functions (as in the engine, ENABLE_USERSTATE_LOCKS == 0);
// float formatting is copied verbatim from the engine (streflop mode switches removed).
#include <array>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cmath>
#include <limits>
#include <algorithm>
#include "lua.h"
#include "lauxlib.h"
#include "LuaUser.h"

template<class T, typename T2> constexpr T mix(const T& v1, const T& v2, const T2& a) { return (v1 + (v2 - v1) * a); }
namespace streflop { struct Double{}; struct Simple{}; template<class T> inline void streflop_init() {} }

void LuaCreateMutex(lua_State*) {}
void LuaDestroyMutex(lua_State*) {}
void LuaLinkMutex(lua_State*, lua_State*) {}
void LuaMutexLock(lua_State*) {}
void LuaMutexUnlock(lua_State*) {}
void LuaMutexYield(lua_State*) {}
const char* spring_lua_get_handle_name(lua_State*) { return ""; }

static unsigned long long stub_rng = 88172645463325252ULL;
static float stub_nextfloat() { stub_rng ^= stub_rng << 13; stub_rng ^= stub_rng >> 7; stub_rng ^= stub_rng << 17; return (float)((stub_rng >> 40) & 0xFFFFFF) / 16777216.0f; }
int spring_lua_unsynced_rand(lua_State* L) {
	const lua_Number r = stub_nextfloat();
	switch (lua_gettop(L)) {
		case 0: lua_pushnumber(L, r); break;
		case 1: { const int u = luaL_checkint(L, 1); luaL_argcheck(L, 1 <= u, 1, "empty interval"); lua_pushnumber(L, std::floor(r * u) + 1); } break;
		case 2: { const int l = luaL_checkint(L, 1); const int u = luaL_checkint(L, 2); luaL_argcheck(L, l <= u, 2, "empty interval"); lua_pushnumber(L, std::floor(r * (u - l + 1)) + l); } break;
		default: return luaL_error(L, "wrong number of arguments");
	}
	return 1;
}
int spring_lua_unsynced_srand(lua_State* L) { stub_rng = 88172645463325252ULL ^ (unsigned)luaL_checkint(L, 1); return 0; }

//////////////////////////////////////////////////////////
////// Custom synced float to string
//////////////////////////////////////////////////////////

#ifdef _WIN32
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#endif
static inline int sprintf64(char* dst, std::int64_t x) { return sprintf(dst, "%I64d", x); }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#else
static inline int sprintf64(char* dst, long int x)      { return sprintf(dst, "%ld", x); }
static inline int sprintf64(char* dst, long long int x) { return sprintf(dst, "%lld", x); }
#endif

// excluding mantissa, a float has a rest int-precision of: 2^24 = 16,777,216
// int numbers in that range are 100% exact, and don't suffer float precision issues
// static constexpr int MAX_PRECISE_DIGITS_IN_FLOAT = std::numeric_limits<float>::digits10;
// static constexpr auto SPRING_FLOAT_MAX = std::numeric_limits<float>::max();
static constexpr auto SPRING_INT64_MAX = std::numeric_limits<std::int64_t>::max();

static constexpr std::array<double, 11> v = {
	{1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10}
};


static constexpr inline double Pow10d(unsigned i)
{
	return (i < v.size()) ? v[i] : std::pow(double(10), i);
}

static const inline int FastLog10(const float f)
{
	assert(f != 0.0f); // log10(0) = -inf

	if (f < 1.0f || f >= (SPRING_INT64_MAX >> 1))
		return std::floor(std::log10(f));

	const std::int64_t i = f;
	      std::int64_t n = 10;

	int log10 = 0;
	while (i >= n) {
		++log10;
		n *= 10;
	}

	return log10;
}


static constexpr inline int GetDigitsInStdNotation(const int log10)
{
	// log10(0.01) = -2  (4 chars)
	// log10(0.1)  = -1  (3 chars)
	// log10(1)    = 0   (1 char)
	// log10(10)   = 1   (2 chars)
	// log10(100)  = 2   (3 chars)
	return (log10 >= 0) ? (log10 + 1) : (-log10 + 2);
}



static inline int PrintIntPart(char* buf, float f, const bool roundingCarryBit = false)
{
#ifdef _WIN32
	if (f < (std::numeric_limits<int>::max() - roundingCarryBit)) {
		return sprintf(buf, "%d", int(f) + roundingCarryBit);
	} else
#endif
	if (f < (SPRING_INT64_MAX - roundingCarryBit))
		return sprintf64(buf, std::int64_t(f) + roundingCarryBit); // much faster than printing a float!

	return sprintf(buf, "%1.0f", f + roundingCarryBit);
}

static inline int PrintFractPart(char* buf, float f, int digits, int precision)
{
	//XXX: Hacks, with streflop enabled we limit the FPU normally to 32bit float
	//     and doing any double math will use floats math then!
	//     But here we need the precision of doubles, so switch the FPU to it just
	//     for this casting.
	//Note: We are still in synced code, so even these doubles need to sync!
	//     Also performance seems to be unaffected by switching the FPU mode.
	streflop::streflop_init<streflop::Double>();

	const char* old = buf;
	char s[16];

	assert(digits <= 15);
	assert(digits <= std::numeric_limits<std::int64_t>::digits10);

	const std::int64_t i = double(f) * Pow10d(digits) + 0.5;
	const int len = sprintf64(s, i);

	if (len < digits) {
		memset(buf, '0', digits - len);
		buf += digits - len;
	}

	memcpy(buf, s, len);
	buf += len;

	// removing trailing zeros
	precision = std::max(1, precision);
	while (buf[-1] == '0' && (buf - old) > precision)
		--buf;
	buf[0] = '\0';

	streflop::streflop_init<streflop::Simple>();
	assert((buf - old) >= 1);
	return (buf - old);
}


static inline bool HandleRounding(float* fractF, int log10, int charsInStdNotation, int nDigits, int precision, bool useScientificNot)
{
	// We handle here the case when rounding in the
	// fract part carries into the integer part.
	// We don't handle the fract rounding itself!
	// fDigits excludes the dot when precision is < 0
	const int iDigits = mix(1, charsInStdNotation, (!useScientificNot && log10 >= 0));
	const int fDigits = mix(std::max(0, nDigits - (iDigits + 1)), precision, (precision >= 0));

	// check fractional part against the rounding limit
	// 1 -> 0.95   -%.1f-> 1.0
	// 2 -> 0.995  -%.2f-> 1.00
	// 3 -> 0.9995 -%.3f-> 1.000
	const float roundLimit = 1.0f - 0.5f * std::pow(0.1f, fDigits);

	if (*fractF >= roundLimit) {
		*fractF = 0.0f;
		return true;
	}

	return false;
}


void spring_lua_ftoa(float f, char* buf, int precision)
{
	static constexpr int MAX_DIGITS = 10;
	static_assert(MAX_DIGITS > 6, "must have enough room for at least 1.0e+23");

	// get rid of integers
	int x = f;
	if (float(x) == f) {
		sprintf(buf, "%i", x);

		if (precision > 0) {
			char* endBuf = strchr(buf, '\0');
			*endBuf = '.';
			++endBuf;
			memset(endBuf, '0', precision);
			endBuf[precision] = '\0';
		}

		return;
	}


	int nDigits = MAX_DIGITS;

	if (std::signbit(f)) { // use signbit() cause < doesn't work with nans
		f = -f;
		buf[0] = '-';
		++buf;
		--nDigits;
	}

	if (std::isinf(f)) {
		strcpy(buf, "inf");
		return;
	}
	if (std::isnan(f)) {
		strcpy(buf, "nan");
		return;
	}

	int e10 = 0;
	const int log10 = FastLog10(f);
	const int charsInStdNotation = GetDigitsInStdNotation(log10);

	if ((charsInStdNotation > nDigits) && (precision == -1)) {
		nDigits -= 4; // space needed for "e+01"
		f *= std::pow(10.0f, -(e10 = log10));
	}

	float truncF;
	float fractF = std::modf(f, &truncF);

	const bool useScientificNot = (e10 != 0);
	const bool roundingCarryBit = HandleRounding(&fractF, log10, charsInStdNotation, nDigits, precision, useScientificNot);

	int iDigits = PrintIntPart(buf, truncF, roundingCarryBit);

	if (useScientificNot) {
		assert(iDigits != 2 || fractF == 0.0f);

		e10 += (iDigits == 2);
		iDigits = mix(iDigits, 1, iDigits == 2);

		assert(iDigits == 1);
	}

	nDigits -= iDigits;
	nDigits = mix(nDigits, precision + 1, precision >= 0); // add 1 for dot if precision is positive
	buf += iDigits;

	if ((nDigits > 1) && (useScientificNot || fractF != 0 || precision > 0)) {
		buf[0] = '.';

		++buf;
		--nDigits;

		buf += PrintFractPart(buf, fractF, nDigits, precision);
	}

	if (!useScientificNot)
		return;

	sprintf(buf, "e%+02d", e10);
}


void spring_lua_format(float f, const char* fmt, char* buf)
{
	if (fmt[0] == '\0')
		return spring_lua_ftoa(f, buf);

	// handles `%(sign)(width)(.precision)f`, i.e. %+10.2f
	char bufC[128];
	char* buf2 = bufC;

	// insert sign; f might be NaN so check with signbit()
	if (fmt[0] == '+' || fmt[0] == ' ') {
		if (!std::signbit(f)) {
			buf2[0] = fmt[0];
			++buf2;
		}
		++fmt;
	}

	// width
	const int width = atoi(fmt);

	// precision
	int precision = -1;
	const char* dotPos = strchr(fmt, '.');

	if (dotPos != nullptr)
		precision = std::clamp(atoi(fmt = dotPos + 1), 0, 15);


	// convert the float
	spring_lua_ftoa(f, buf2, precision);

	// right align the number when `width` is given
	const int len = strlen(bufC);
	if (len < width) {
		memset(buf, ' ', width - len);
		buf += (width - len);
	}

	// copy the float string into dst
	memcpy(buf, bufC, len + 1);
}

