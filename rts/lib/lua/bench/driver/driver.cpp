// Standalone driver for the Recoil Lua VM (benchmark + equality harness).
//
//   luadrv [-q] [-t] [-g] [-k N] [-m] file.lua [args...]
//
// - deterministic allocator living at a fixed virtual address, so pointer-keyed
//   hashing (tables/functions as keys) is reproducible across processes/builds
// - every allocator call (osize, nsize) is folded into an FNV hash: identical
//   allocation/free sequences <=> identical GC behaviour (printed at exit)
// - a stub "Spring" table with C functions in the style of the engine API
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <map>
#include <vector>

extern "C++" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// ---------------------------------------------------------------- allocator
static char* arenaBase = nullptr;
static size_t arenaUsed = 0;
#ifdef _WIN32
static const size_t ARENA_SIZE = size_t(2) << 30;  // 2 GB (committed lazily by the OS on touch)
#else
static const size_t ARENA_SIZE = size_t(8) << 30;  // 8 GB reserved (NORESERVE)
#endif
static std::vector<void*> smallFree[257];           // 16-byte classes up to 4096
static std::map<size_t, std::vector<void*>> bigFree;
static uint64_t allocHash = 1469598103934665603ULL;
static uint64_t allocCalls = 0;
static size_t liveBytes = 0, peakBytes = 0;

static inline void hashmix(uint64_t v) {
	for (int i = 0; i < 8; i++) { allocHash ^= (v & 0xff); allocHash *= 1099511628211ULL; v >>= 8; }
}

static void* arenaGet(size_t n) {
	n = (n + 15) & ~size_t(15);
	if (n <= 4096) {
		std::vector<void*>& fl = smallFree[n >> 4];
		if (!fl.empty()) { void* p = fl.back(); fl.pop_back(); return p; }
	} else {
		auto it = bigFree.find(n);
		if (it != bigFree.end() && !it->second.empty()) { void* p = it->second.back(); it->second.pop_back(); return p; }
	}
	if (arenaUsed + n > ARENA_SIZE) { fprintf(stderr, "arena exhausted\n"); abort(); }
	void* p = arenaBase + arenaUsed;
	arenaUsed += n;
	return p;
}

static void arenaPut(void* p, size_t n) {
	n = (n + 15) & ~size_t(15);
	if (n <= 4096) smallFree[n >> 4].push_back(p);
	else bigFree[n].push_back(p);
}

static bool useMalloc = false;  // -m: libc realloc (for ASan; addresses not deterministic)

static void* detAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
	(void)ud;
	if (useMalloc) {
		allocCalls++;
		hashmix(osize); hashmix(nsize);
		liveBytes = liveBytes - osize + nsize;
		if (liveBytes > peakBytes) peakBytes = liveBytes;
		if (nsize == 0) { free(ptr); return nullptr; }
		return realloc(ptr, nsize);
	}
	allocCalls++;
	hashmix(osize); hashmix(nsize);
	liveBytes = liveBytes - osize + nsize;
	if (liveBytes > peakBytes) peakBytes = liveBytes;
	if (nsize == 0) {
		if (ptr) arenaPut(ptr, osize);
		return nullptr;
	}
	void* np = arenaGet(nsize);
	if (ptr) {
		memcpy(np, ptr, osize < nsize ? osize : nsize);
		arenaPut(ptr, osize);
	}
	return np;
}

// ---------------------------------------------------------------- stub engine API
static int unitCounter = 0;

static int S_GetUnitPosition(lua_State* L) {
	const int id = luaL_checkint(L, 1);
	lua_pushnumber(L, (float)(id * 3 % 1000) + 0.5f);
	lua_pushnumber(L, (float)(id % 7) * 2.0f);
	lua_pushnumber(L, (float)(id * 11 % 1000) + 0.25f);
	return 3;
}
static int S_GetUnitDefID(lua_State* L) {
	const int id = luaL_checkint(L, 1);
	lua_pushnumber(L, (float)(id % 13 + 1));
	return 1;
}
static int S_ValidUnitID(lua_State* L) {
	const int id = luaL_checkint(L, 1);
	lua_pushboolean(L, (id % 17) != 0);
	return 1;
}
static int S_GetUnitHealth(lua_State* L) {
	const int id = luaL_checkint(L, 1);
	lua_pushnumber(L, (float)(id % 500));
	lua_pushnumber(L, 500.0f);
	lua_pushnumber(L, 0.0f);
	lua_pushnumber(L, 0.0f);
	lua_pushnumber(L, 1.0f);
	return 5;
}
static int S_GetGameFrame(lua_State* L) {
	lua_pushnumber(L, (float)(unitCounter++ % 100000));
	return 1;
}
static int S_GetUnitsInRectangle(lua_State* L) {
	const int n = luaL_optint(L, 5, 20);
	lua_createtable(L, n, 0);
	for (int i = 1; i <= n; i++) {
		lua_pushnumber(L, (float)(i * 7 + 1000));
		lua_rawseti(L, -2, i);
	}
	return 1;
}
static int S_GetUnitRulesParam(lua_State* L) {
	const int id = luaL_checkint(L, 1);
	const char* name = luaL_checkstring(L, 2);
	lua_pushnumber(L, (float)((id + (int)strlen(name)) % 5));
	return 1;
}
static int S_SetUnitRulesParam(lua_State* L) {
	luaL_checkint(L, 1);
	luaL_checkstring(L, 2);
	lua_tonumber(L, 3);
	return 0;
}
static int S_Echo(lua_State* L) {
	const int n = lua_gettop(L);
	for (int i = 1; i <= n; i++) {
		size_t len;
		lua_getglobal(L, "tostring");
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1) fputc('\t', stdout);
		fwrite(s, 1, len, stdout);
		lua_pop(L, 1);
	}
	fputc('\n', stdout);
	return 0;
}
// generic table-reading C function: reads fields via lua_getfield (string keys)
static int S_SumFields(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	float s = 0.0f;
	static const char* const names[] = {"x", "y", "z", "health", "speed"};
	for (const char* n : names) {
		lua_getfield(L, 1, n);
		s += (float)lua_tonumber(L, -1);
		lua_pop(L, 1);
	}
	lua_pushnumber(L, s);
	return 1;
}
static int S_Noop(lua_State* L) { (void)L; return 0; }
static int S_GetUnitPiecePosDir(lua_State* L) {
	const int id = luaL_checkint(L, 1);
	const int piece = luaL_optint(L, 2, 0);
	for (int i = 0; i < 6; i++) lua_pushnumber(L, (float)((id * 31 + piece * 7 + i) % 997) * 0.5f);
	return 6;
}
static int D_Clock(lua_State* L) {
	lua_pushnumber(L, (lua_Number)0);  // never expose wall time to scripts (determinism)
	return 1;
}

static const luaL_Reg springFuncs[] = {
	{"GetUnitPosition", S_GetUnitPosition},
	{"GetUnitDefID", S_GetUnitDefID},
	{"ValidUnitID", S_ValidUnitID},
	{"GetUnitHealth", S_GetUnitHealth},
	{"GetGameFrame", S_GetGameFrame},
	{"GetUnitsInRectangle", S_GetUnitsInRectangle},
	{"GetUnitRulesParam", S_GetUnitRulesParam},
	{"SetUnitRulesParam", S_SetUnitRulesParam},
	{"Echo", S_Echo},
	{"SumFields", S_SumFields},
	{"Clock", D_Clock},
	{"Noop", S_Noop},
	{"GetUnitPiecePosDir", S_GetUnitPiecePosDir},
	{NULL, NULL}
};

static int traceback(lua_State* L) {
	const char* msg = lua_tostring(L, 1);
	if (msg == NULL) msg = "(non-string error)";
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
	lua_pushstring(L, msg);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

static unsigned long long hookCalls = 0;
static void countHook(lua_State* L, lua_Debug* ar) {
	hookCalls++;
	// look at the running function like a profiler would (exercises debug info / savedpc)
	if ((hookCalls & 63) == 0 && lua_getinfo(L, "Sl", ar))
		hookCalls += (unsigned)ar->currentline;
}

static double now() {
#ifdef _WIN32
	ULONG64 cycles = 0;  // cycles charged to this thread (robust against preemption)
	QueryThreadCycleTime(GetCurrentThread(), &cycles);
	return (double)cycles / 3.2e9;  // nominal 3.2 GHz TSC: reported as seconds-equivalent
#else
	timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);  // CPU time: robust against preemption on a loaded box
	return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

int main(int argc, char** argv) {
	bool quiet = false, timing = false, gcstress = false;
	int hookEvery = 0;
	int argi = 1;
	while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != 0) {
		if (!strcmp(argv[argi], "-q")) quiet = true;
		else if (!strcmp(argv[argi], "-t")) timing = true;
		else if (!strcmp(argv[argi], "-g")) gcstress = true;                // aggressive incremental GC
		else if (!strcmp(argv[argi], "-m")) useMalloc = true;               // libc realloc (ASan; addresses not deterministic)
		else if (!strcmp(argv[argi], "-k") && argi + 1 < argc) hookEvery = atoi(argv[++argi]);  // count hook
		else break;
		argi++;
	}
	if (argi >= argc) { fprintf(stderr, "usage: %s [-q] [-t] file.lua [args]\n", argv[0]); return 2; }

#ifdef _WIN32
	arenaBase = (char*)VirtualAlloc((void*)0x200000000000ULL, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (arenaBase != (char*)0x200000000000ULL) { fprintf(stderr, "VirtualAlloc arena failed\n"); return 3; }
#else
	arenaBase = (char*)mmap((void*)0x200000000000ULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (arenaBase != (char*)0x200000000000ULL) { perror("mmap arena"); return 3; }
#endif

	lua_State* L = lua_newstate(detAlloc, nullptr);
	luaL_openlibs(L);
	if (gcstress) {
		lua_gc(L, LUA_GCSETPAUSE, 20);
		lua_gc(L, LUA_GCSETSTEPMUL, 2000);
	}
	if (hookEvery > 0)
		lua_sethook(L, countHook, LUA_MASKCOUNT, hookEvery);
	luaL_register(L, "Spring", springFuncs);
	lua_pop(L, 1);
	lua_pushboolean(L, quiet);
	lua_setglobal(L, "QUIET");

	// arg table
	lua_newtable(L);
	for (int i = argi; i < argc; i++) {
		lua_pushstring(L, argv[i]);
		lua_rawseti(L, -2, i - argi);
	}
	lua_setglobal(L, "arg");

	lua_pushcfunction(L, traceback);
	const int errIdx = lua_gettop(L);
	if (luaL_loadfile(L, argv[argi]) != 0) {
		fprintf(stdout, "LOAD ERROR: %s\n", lua_tostring(L, -1));
		return 1;
	}
	const double t0 = now();
	const int status = lua_pcall(L, 0, 0, errIdx);
	const double t1 = now();
	if (status != 0)
		fprintf(stdout, "RUN ERROR: %s\n", lua_tostring(L, -1));
	fflush(stdout);
	const int gcKB = lua_gc(L, LUA_GCCOUNT, 0);
	const int gcB = lua_gc(L, LUA_GCCOUNTB, 0);
	if (hookEvery > 0)
		fprintf(stdout, "HOOK calls=%llu\n", hookCalls);
	if (!quiet || !timing)
		fprintf(stdout, "ALLOC calls=%llu hash=%016llx live=%zu peak=%zu gc=%dK+%d\n",
		        (unsigned long long)allocCalls, (unsigned long long)allocHash, liveBytes, peakBytes, gcKB, gcB);
	lua_close(L);
	if (timing)
		fprintf(stderr, "TIME %.6f\n", t1 - t0);
	return status;
}
