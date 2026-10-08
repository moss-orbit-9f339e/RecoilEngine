/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/*
 * Statistical (sampling) profiler of the main thread, x86-64 Windows only.
 *
 * Why: Tracy zones inflate tiny hot functions and per-callin Lua dispatch, and
 * Windows sampling profilers cannot read mingw DWARF. This samples the normal
 * (non-Tracy) build and writes raw addresses; symbolization happens offline
 * (tools/sampling-profiler/sampler.py, addr2line on the unstripped spring.exe).
 *
 * How (Chromium base/profiler "stack copy" approach):
 *   sampler thread (TIME_CRITICAL, high-resolution waitable timer):
 *     SuspendThread(main) + GetThreadContext       \
 *     memcpy main stack [RSP, min(base, RSP+cap))   |  target suspended: no locks,
 *     read Lua TLS slot, lua_State, CallInfos and   |  no allocation, no logging,
 *       the function TValues (ReadProcessMemory)   /   no RtlLookupFunctionEntry
 *     ResumeThread(main)
 *     unwind the *copy* (RtlLookupFunctionEntry + RtlVirtualUnwind, registers and
 *       restored frame pointers relocated into the copy), resolve Lua closures,
 *       protos and source strings (ReadProcessMemory, cached per Proto), store the
 *       sample in a preallocated SPSC ring (full ring: sample dropped and counted)
 *   writer thread: drains the ring to the file every ~10 ms
 *
 * Lua attribution: lua_pcall/lua_resume/lua_gc maintain the thread_local
 * `luai_activestate` (lib/lua/src/lstate.h); the address of the main thread's
 * instance is taken in Start() and read while the main thread is suspended.
 * Struct layouts come straight from the engine's own lstate.h/lobject.h.
 *
 * ---------------------------------------------------------------------------
 * File format (little endian, no padding between fields; version 1)
 *
 * FileHeader (96 bytes)
 *    0  char[8] magic            "RCLSMPL\0"
 *    8  u32     version          1
 *   12  u32     headerSize       96
 *   16  u64     qpcFrequency     QueryPerformanceFrequency
 *   24  u64     startQpc
 *   32  u64     startFileTime    UTC FILETIME (100 ns since 1601)
 *   40  u32     intervalUs
 *   44  u32     maxNativeFrames
 *   48  u32     maxLuaFrames
 *   52  u32     stackCopyCap     bytes
 *   56  u64     exeImageBase     runtime base of the main executable
 *   64  u64     exeImageSize
 *   72  u32     threadId         sampled (main) thread
 *   76  u32     processId
 *   80  u32     flags            bit0: high-resolution timer, bit1: timeBeginPeriod(1) fallback
 *   84  u32     pointerSize      8
 *   88  u64     reserved
 *
 * then records: u32 type, u32 payloadSize, payload[payloadSize]
 *   1 MODULE     u64 base, u64 size, u32 nameLen, char name[nameLen] (UTF-8 full path)
 *                written at start and again at stop (reader dedupes by base+size)
 *   2 SAMPLE     u64 qpc (suspend time), u32 suspendTicks (QPC ticks suspended),
 *                u32 flags (SamplingProfilerCore::SampleFlags),
 *                u64 luaG (global_State* of the active Lua state, 0 = none),
 *                u64 luaL (lua_State* = the state or coroutine), u16 nframes,
 *                u16 nlua, u32 reserved,
 *                u64 frames[nframes]   absolute addresses, [0] = RIP (exact),
 *                                      [1..] = return addresses (use addr-1)
 *                LuaFrame lua[nlua]    16 bytes each, [0] = innermost:
 *                  u8 kind (1 = Lua function, 2 = C function, 3 = not a function,
 *                           4 = unreadable), u8 pad, u16 pad,
 *                  i32 line (current line for Lua functions, -1 otherwise),
 *                  u64 value (kind 1: LUAFUNC id, kind 2: lua_CFunction address)
 *   3 LUASTATE   u64 globalState, u8 synced, u8[3] pad, u32 nameLen, char name[]
 *   4 LUAFUNC    u32 id, i32 linedefined, i32 lastlinedefined, u32 sourceFullLen,
 *                u64 proto, u32 srcLen, char source[srcLen] (first <=128 bytes)
 *   6 TRAILER    u64 endQpc, u64 samples, u64 drops, u64 suspendFailures,
 *                u64 contextFailures, u64 missedTicks, u64 suspendMinTicks,
 *                u64 suspendMaxTicks, u64 suspendSumTicks, u64 workMaxTicks,
 *                u64 workSumTicks, u64 samplerCpu100ns, u64 writerCpu100ns,
 *                u64 luaReadFailures, u64 luaFuncCount, u64 luaFuncOverflow
 * Unknown record types must be skipped by readers (size-prefixed).
 * ---------------------------------------------------------------------------
 */

#include "System/Platform/SamplingProfiler.h"

#ifdef RECOIL_SAMPLING_PROFILER

#include "System/Platform/Win/SamplingProfilerCore.h"

#ifndef PSAPI_VERSION
	#define PSAPI_VERSION 2 // K32* in kernel32, no psapi.lib needed
#endif
#include <psapi.h>
#include <mmsystem.h>

#include "lib/lua/src/lstate.h"

#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileQueryFlags.h"
#include "System/Log/ILog.h"
#include "System/Platform/Threading.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>


CONFIG(int, SamplingProfiler)
	.defaultValue(0)
	.minimumValue(0)
	.description("Sampling profiler of the main thread (x86-64 Windows only): 0 = disabled, otherwise the sampling interval in microseconds (e.g. 1000). Start/stop with /sampler start|stop; each session writes sampling-YYYYmmdd-HHMMSS.bin to the write dir.");
CONFIG(int, SamplingProfilerDepth)
	.defaultValue(64)
	.minimumValue(1)
	.maximumValue(256)
	.description("Sampling profiler: maximum number of native stack frames recorded per sample.");
CONFIG(int, SamplingProfilerLuaDepth)
	.defaultValue(4)
	.minimumValue(0)
	.maximumValue(8)
	.description("Sampling profiler: maximum number of Lua call frames recorded per sample (0 = no Lua attribution).");


namespace {

using namespace SamplingProfilerCore;

constexpr char     FILE_MAGIC[8]     = {'R', 'C', 'L', 'S', 'M', 'P', 'L', '\0'};
constexpr uint32_t FILE_VERSION      = 1;

constexpr uint32_t REC_MODULE   = 1;
constexpr uint32_t REC_SAMPLE   = 2;
constexpr uint32_t REC_LUASTATE = 3;
constexpr uint32_t REC_LUAFUNC  = 4;
constexpr uint32_t REC_TRAILER  = 6;

constexpr uint32_t HDR_FLAG_HIGHRES_TIMER = 1u << 0;
constexpr uint32_t HDR_FLAG_TIMEPERIOD    = 1u << 1;

constexpr int      MAX_LUA_FRAMES    = 8;
constexpr size_t   STACK_COPY_CAP    = 512 * 1024;
constexpr size_t   STACK_COPY_SLACK  = 64 * 1024;   // readable zero-filled tail behind the copy
constexpr uint32_t RING_SLOTS        = 8192;        // power of two; ~8 s at 1 kHz
constexpr uint32_t MAX_LUA_FUNCS     = 32768;
constexpr uint32_t LUA_FUNC_BUCKETS  = 65536;       // power of two, 2x MAX_LUA_FUNCS
constexpr uint32_t LUA_FUNC_OVERFLOW = 0xFFFFFFFFu;
constexpr size_t   LUA_SOURCE_MAX    = 128;
constexpr size_t   LUA_FUNC_SPAN_MAX = 4096;        // read all frame functions at once if this close

enum LuaFrameKind : uint8_t {
	LF_LUA        = 1,
	LF_C          = 2,
	LF_OTHER      = 3,
	LF_UNREADABLE = 4,
};

#pragma pack(push, 1)
struct FileHeader {
	char     magic[8];
	uint32_t version;
	uint32_t headerSize;
	uint64_t qpcFrequency;
	uint64_t startQpc;
	uint64_t startFileTime;
	uint32_t intervalUs;
	uint32_t maxNativeFrames;
	uint32_t maxLuaFrames;
	uint32_t stackCopyCap;
	uint64_t exeImageBase;
	uint64_t exeImageSize;
	uint32_t threadId;
	uint32_t processId;
	uint32_t flags;
	uint32_t pointerSize;
	uint64_t reserved;
};

struct LuaFrameRec {
	uint8_t  kind;
	uint8_t  pad0;
	uint16_t pad1;
	int32_t  line;
	uint64_t value;
};

struct SampleRecHead {
	uint64_t qpc;
	uint32_t suspendTicks;
	uint32_t flags;
	uint64_t luaG;
	uint64_t luaL;
	uint16_t nframes;
	uint16_t nlua;
	uint32_t reserved;
};

struct TrailerRec {
	uint64_t endQpc;
	uint64_t samples;
	uint64_t drops;
	uint64_t suspendFailures;
	uint64_t contextFailures;
	uint64_t missedTicks;
	uint64_t suspendMinTicks;
	uint64_t suspendMaxTicks;
	uint64_t suspendSumTicks;
	uint64_t workMaxTicks;
	uint64_t workSumTicks;
	uint64_t samplerCpu100ns;
	uint64_t writerCpu100ns;
	uint64_t luaReadFailures;
	uint64_t luaFuncCount;
	uint64_t luaFuncOverflow;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 96, "file format");
static_assert(sizeof(LuaFrameRec) == 16, "file format");
static_assert(sizeof(SampleRecHead) == 40, "file format");
static_assert(sizeof(TrailerRec) == 16 * 8, "file format");


// one fixed-size ring slot: header + Lua frames, followed by maxFrames u64 native frames
struct SlotHead {
	SampleRecHead rec;
	LuaFrameRec lua[MAX_LUA_FRAMES];
};


inline uint64_t QPC()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return static_cast<uint64_t>(t.QuadPart);
}

inline uint64_t FileTimeTo100ns(const FILETIME& ft)
{
	return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring Utf8ToWide(const std::string& s)
{
	if (s.empty())
		return {};

	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(std::max(n, 0)), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

std::string WideToUtf8(const wchar_t* w, size_t len)
{
	if (len == 0)
		return {};

	const int n = WideCharToMultiByte(CP_UTF8, 0, w, int(len), nullptr, 0, nullptr, nullptr);
	std::string s(size_t(std::max(n, 0)), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, int(len), s.data(), n, nullptr, nullptr);
	return s;
}


// buffered sequential writer; used by one thread at a time
class OutFile {
public:
	~OutFile() { Close(); }

	// returns 0 on success, else the Win32 error code
	DWORD Create(const std::wstring& path) {
		h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return std::max<DWORD>(GetLastError(), 1);
		buf.reserve(BUF_SIZE);
		return 0;
	}

	void Put(const void* data, size_t size) {
		if (buf.size() + size > BUF_SIZE)
			Flush();
		if (size > BUF_SIZE) {
			WriteRaw(data, size);
			return;
		}
		const uint8_t* p = static_cast<const uint8_t*>(data);
		buf.insert(buf.end(), p, p + size);
	}

	template<typename T> void PutT(const T& v) { Put(&v, sizeof(v)); }

	void Record(uint32_t type, const void* payload, uint32_t size) {
		PutT(type);
		PutT(size);
		Put(payload, size);
	}

	void Flush() {
		if (!buf.empty())
			WriteRaw(buf.data(), buf.size());
		buf.clear();
	}

	void Close() {
		if (h == INVALID_HANDLE_VALUE)
			return;
		Flush();
		CloseHandle(h);
		h = INVALID_HANDLE_VALUE;
	}

	uint64_t BytesWritten() const { return written; }
	bool Failed() const { return failed; }

private:
	void WriteRaw(const void* data, size_t size) {
		const uint8_t* p = static_cast<const uint8_t*>(data);
		while (size > 0 && !failed) {
			DWORD n = 0;
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
			if (!WriteFile(h, p, chunk, &n, nullptr) || n == 0) {
				failed = true;
				break;
			}
			p += n;
			size -= n;
			written += n;
		}
	}

	static constexpr size_t BUF_SIZE = 1 << 20;

	HANDLE h = INVALID_HANDLE_VALUE;
	std::vector<uint8_t> buf;
	uint64_t written = 0;
	bool failed = false;
};


// Proto -> id table with the first bytes of each function's source name.
// Preallocated; only touched by the sampler thread while running.
class LuaFuncTable {
public:
	struct Entry {
		uint64_t proto;
		uint64_t source; // TString*
		int32_t  lineDefined;
		int32_t  lastLineDefined;
		uint32_t srcLen;
		uint32_t srcFullLen;
		char     src[LUA_SOURCE_MAX];
	};

	void Init() {
		entries.resize(MAX_LUA_FUNCS);
		buckets.assign(LUA_FUNC_BUCKETS, 0);
		count = 0;
		overflow = 0;
	}

	// Validated against source pointer and linedefined, so a Proto address
	// reused after GC by a different function gets a new id.
	uint32_t Intern(const void* protoPtr, const Proto& p) {
		const uint64_t key = reinterpret_cast<uintptr_t>(protoPtr);
		uint32_t h = static_cast<uint32_t>((key >> 4) * 0x9E3779B97F4A7C15ull >> 40) & (LUA_FUNC_BUCKETS - 1);

		for (uint32_t probe = 0; probe < LUA_FUNC_BUCKETS; ++probe, h = (h + 1) & (LUA_FUNC_BUCKETS - 1)) {
			const uint32_t idx = buckets[h];

			if (idx == 0)
				return Insert(h, key, p);

			const Entry& e = entries[idx - 1];
			if (e.proto == key && e.source == reinterpret_cast<uintptr_t>(p.source) && e.lineDefined == p.linedefined)
				return (idx - 1);
		}

		++overflow;
		return LUA_FUNC_OVERFLOW;
	}

	uint32_t Count() const { return count; }
	uint64_t Overflow() const { return overflow; }
	const Entry& Get(uint32_t i) const { return entries[i]; }

private:
	uint32_t Insert(uint32_t bucket, uint64_t key, const Proto& p) {
		if (count >= MAX_LUA_FUNCS) {
			++overflow;
			return LUA_FUNC_OVERFLOW;
		}

		Entry& e = entries[count];
		e.proto = key;
		e.source = reinterpret_cast<uintptr_t>(p.source);
		e.lineDefined = p.linedefined;
		e.lastLineDefined = p.lastlinedefined;
		e.srcLen = 0;
		e.srcFullLen = 0;

		TString ts;
		if (p.source != nullptr && SafeRead(p.source, &ts, sizeof(ts)) && ts.tsv.tt == LUA_TSTRING) {
			const size_t n = std::min<size_t>(ts.tsv.len, LUA_SOURCE_MAX);
			e.srcFullLen = static_cast<uint32_t>(std::min<size_t>(ts.tsv.len, UINT32_MAX));
			// string bytes follow the (max-aligned) header, see getstr()
			if (SafeRead(reinterpret_cast<const char*>(p.source + 1), e.src, n))
				e.srcLen = static_cast<uint32_t>(n);
		}

		buckets[bucket] = ++count;
		return (count - 1);
	}

	std::vector<Entry> entries;
	std::vector<uint32_t> buckets;
	uint32_t count = 0;
	uint64_t overflow = 0;
};


// raw Lua data captured while the main thread is suspended
struct LuaRaw {
	uintptr_t L;
	bool stateOk;
	bool truncated;
	int n; // captured CallInfos, [0] = innermost
	lua_State state;
	CallInfo ci[MAX_LUA_FRAMES];
	TValue func[MAX_LUA_FRAMES];
	bool funcOk[MAX_LUA_FRAMES];
	CallInfo ciTmp[MAX_LUA_FRAMES];
	alignas(16) uint8_t span[LUA_FUNC_SPAN_MAX];
};

struct AddrRange {
	uint64_t lo;
	uint64_t hi;
};

struct LuaStateName {
	uint64_t g;
	bool synced;
	std::string name;
};


struct Session {
	// settings
	int intervalUs = 1000;
	int maxFrames = 64;
	int maxLua = 4;
	uint64_t qpcFreq = 1;
	uint64_t intervalTicks = 1;
	uint64_t startQpc = 0;
	uint32_t headerFlags = 0;

	// target
	HANDLE thread = nullptr;
	DWORD threadId = 0;
	uintptr_t stackBase = 0;
	lua_State* const volatile* luaSlot = nullptr;
	uint64_t exeBase = 0;
	uint64_t exeSize = 0;
	std::vector<AddrRange> waitRanges;

	// threads
	HANDLE timer = nullptr;
	HANDLE stopEvent = nullptr;
	HANDLE writerStopEvent = nullptr;
	HANDLE samplerThread = nullptr;
	HANDLE writerThread = nullptr;

	// buffers (allocated in Start, never resized while running)
	alignas(16) CONTEXT ctx;
	uint8_t* stackCopy = nullptr;
	size_t stackCopyAlloc = 0;
	std::vector<uint8_t> ring;
	size_t slotStride = 0;
	std::atomic<uint64_t> head{0}; // written by sampler thread
	std::atomic<uint64_t> tail{0}; // written by writer thread
	LuaFuncTable funcs;
	LuaRaw luaRaw;
	UnwindCache unwindCache;
	uint32_t unwindCacheEpoch = 0;
	PVOID dllNotifyCookie = nullptr; // non-null: unwindCache in use

	// output
	std::string path;
	OutFile out; // writer thread while running, then Stop()

	// stats (sampler thread; read by others only after it exited, except relaxed atomics)
	std::atomic<uint64_t> drops{0};
	uint64_t suspendFailures = 0;
	uint64_t contextFailures = 0;
	uint64_t missedTicks = 0;
	uint64_t luaReadFailures = 0;
	uint64_t suspMin = UINT64_MAX;
	uint64_t suspMax = 0;
	uint64_t suspSum = 0;
	uint64_t workMax = 0;
	uint64_t workSum = 0;

	SlotHead* Slot(uint64_t i) { return reinterpret_cast<SlotHead*>(ring.data() + (i & (RING_SLOTS - 1)) * slotStride); }
	static uint64_t* Frames(SlotHead* s) { return reinterpret_cast<uint64_t*>(s + 1); }

	bool IsWaitAddress(uint64_t a) const {
		for (const AddrRange& r: waitRanges) {
			if (a >= r.lo && a < r.hi)
				return true;
		}
		return false;
	}
};


std::mutex controlMutex;            // Start/Stop/Status
Session* session = nullptr;         // guarded by controlMutex
std::atomic<bool> running{false};

std::atomic<uint32_t> moduleEpoch{0}; // bumped on every DLL load/unload

std::mutex namesMutex;              // Lua state names (NoteLuaState may run off the main thread)
std::vector<LuaStateName> stateNames;
std::atomic<bool> collectNames{false};



// ---------------------------------------------------------------------------
// DLL load/unload notifications (ntdll, Vista+) invalidate the unwind cache

typedef VOID (CALLBACK* LdrDllNotificationFn)(ULONG reason, const void* data, PVOID context);
typedef LONG (NTAPI* LdrRegisterDllNotificationFn)(ULONG flags, LdrDllNotificationFn fn, PVOID context, PVOID* cookie);
typedef LONG (NTAPI* LdrUnregisterDllNotificationFn)(PVOID cookie);

// runs on the loading thread under the loader lock: only bump a counter
VOID CALLBACK OnDllNotification(ULONG, const void*, PVOID)
{
	moduleEpoch.fetch_add(1, std::memory_order_acq_rel);
}

template<typename F> F NtdllProc(const char* name)
{
	return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)));
}



// ---------------------------------------------------------------------------
// Lua (raw memory, fault tolerant)

// Target suspended: only plain loads of our own data and ReadProcessMemory.
void SnapshotLua(Session& s, LuaRaw& r)
{
	r.n = 0;
	r.stateOk = false;
	r.truncated = false;
	r.L = reinterpret_cast<uintptr_t>(*s.luaSlot);

	if (r.L == 0)
		return;
	if (!SafeRead(reinterpret_cast<const void*>(r.L), &r.state, sizeof(r.state)))
		return;

	r.stateOk = true;

	const uintptr_t ci   = reinterpret_cast<uintptr_t>(r.state.ci);
	const uintptr_t base = reinterpret_cast<uintptr_t>(r.state.base_ci);
	const uintptr_t end  = reinterpret_cast<uintptr_t>(r.state.end_ci);

	// base_ci[0] is the dummy entry of lua_newstate/lua_newthread, never a real call
	if (base == 0 || ci <= base || ci >= end || ((ci - base) % sizeof(CallInfo)) != 0)
		return;

	const size_t avail = (ci - base) / sizeof(CallInfo);
	const size_t n = std::min<size_t>(avail, size_t(s.maxLua));
	r.truncated = (avail > n);

	if (!SafeRead(reinterpret_cast<const void*>(ci - (n - 1) * sizeof(CallInfo)), r.ciTmp, n * sizeof(CallInfo)))
		return;

	for (size_t i = 0; i < n; ++i)
		r.ci[i] = r.ciTmp[n - 1 - i];

	const uintptr_t stackLo = reinterpret_cast<uintptr_t>(r.state.stack);
	const uintptr_t stackHi = stackLo + size_t(std::max(r.state.stacksize, 0)) * sizeof(TValue);
	uintptr_t lo = UINTPTR_MAX;
	uintptr_t hi = 0;

	for (size_t i = 0; i < n; ++i) {
		const uintptr_t f = reinterpret_cast<uintptr_t>(r.ci[i].func);
		r.funcOk[i] = (f >= stackLo && f < stackHi && ((f - stackLo) % sizeof(TValue)) == 0);

		if (!r.funcOk[i])
			continue;

		lo = std::min(lo, f);
		hi = std::max(hi, f + sizeof(TValue));
	}

	if (lo < hi && (hi - lo) <= sizeof(r.span) && SafeRead(reinterpret_cast<const void*>(lo), r.span, hi - lo)) {
		for (size_t i = 0; i < n; ++i) {
			if (r.funcOk[i])
				std::memcpy(&r.func[i], r.span + (reinterpret_cast<uintptr_t>(r.ci[i].func) - lo), sizeof(TValue));
		}
	} else {
		for (size_t i = 0; i < n; ++i) {
			if (r.funcOk[i])
				r.funcOk[i] = SafeRead(r.ci[i].func, &r.func[i], sizeof(TValue));
		}
	}

	r.n = static_cast<int>(n);
}

// Target running again: closures, protos and strings are immutable once
// created and stay alive while referenced from the Lua stack; ReadProcessMemory
// keeps us safe in the (microsecond) window where they could have been freed.
void ResolveLua(Session& s, const LuaRaw& r, SlotHead* slot)
{
	SampleRecHead& rec = slot->rec;
	rec.nlua = 0;
	rec.luaG = 0;
	rec.luaL = r.L;

	if (r.L == 0)
		return;

	rec.flags |= SF_LUA_ACTIVE;

	if (!r.stateOk) {
		rec.flags |= SF_LUA_READ_FAIL;
		++s.luaReadFailures;
		return;
	}

	rec.luaG = reinterpret_cast<uintptr_t>(r.state.l_G);

	if (r.truncated)
		rec.flags |= SF_LUA_TRUNCATED;

	constexpr size_t CLOSURE_PREFIX = std::max(offsetof(CClosure, f) + sizeof(lua_CFunction), offsetof(LClosure, p) + sizeof(Proto*));
	bool readFail = false;

	for (int i = 0; i < r.n; ++i) {
		LuaFrameRec& o = slot->lua[i];
		o = {};
		o.line = -1;

		if (!r.funcOk[i]) {
			o.kind = LF_UNREADABLE;
			readFail = true;
			continue;
		}

		const TValue& tv = r.func[i];

		if (tv.tt != LUA_TFUNCTION) {
			o.kind = LF_OTHER;
			continue;
		}

		Closure cl;
		if (!SafeRead(tv.value.gc, &cl, CLOSURE_PREFIX) || cl.c.tt != LUA_TFUNCTION) {
			o.kind = LF_UNREADABLE;
			readFail = true;
			continue;
		}

		if (cl.c.isC) {
			o.kind = LF_C;
			o.value = reinterpret_cast<uintptr_t>(cl.c.f);
			continue;
		}

		Proto p;
		if (!SafeRead(cl.l.p, &p, sizeof(p)) || p.tt != LUA_TPROTO) {
			o.kind = LF_UNREADABLE;
			readFail = true;
			continue;
		}

		o.kind = LF_LUA;
		o.value = s.funcs.Intern(cl.l.p, p);
		o.line = p.linedefined;

		// current line, as ldebug.c currentline(): lineinfo[pc - code - 1]
		const uintptr_t pc = reinterpret_cast<uintptr_t>((i == 0) ? r.state.savedpc : r.ci[i].savedpc);
		const uintptr_t code = reinterpret_cast<uintptr_t>(p.code);

		if (p.lineinfo != nullptr && pc > code && ((pc - code) % sizeof(Instruction)) == 0) {
			const uintptr_t idx = (pc - code) / sizeof(Instruction) - 1;
			int line = 0;

			if (idx < uintptr_t(std::max(p.sizelineinfo, 0)) && SafeRead(p.lineinfo + idx, &line, sizeof(line)))
				o.line = line;
		}
	}

	rec.nlua = static_cast<uint16_t>(r.n);

	if (readFail) {
		rec.flags |= SF_LUA_READ_FAIL;
		++s.luaReadFailures;
	}
}



// ---------------------------------------------------------------------------
// sampler thread

void TakeSample(Session& s)
{
	const uint64_t head = s.head.load(std::memory_order_relaxed);

	// writer fell behind: do not even suspend the target
	if ((head - s.tail.load(std::memory_order_acquire)) >= RING_SLOTS) {
		s.drops.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	SlotHead* slot = s.Slot(head);
	uint64_t* frames = Session::Frames(slot);
	LuaRaw& lua = s.luaRaw;
	bool suspendFailed = false;

	const uint64_t t0 = QPC();

	if (!SuspendAndGetContext(s.thread, &s.ctx, &suspendFailed)) {
		++(suspendFailed ? s.suspendFailures : s.contextFailures);
		return;
	}

	// ---- target suspended: no locks, no allocation, no logging ----
	const uintptr_t rsp = s.ctx.Rsp;
	const size_t copied = CopyStack(rsp, s.stackBase, s.stackCopy, STACK_COPY_CAP);

	if (s.maxLua > 0)
		SnapshotLua(s, lua);
	else
		lua.L = 0;

	ResumeThread(s.thread);
	// ---- target running again ----

	const uint64_t t1 = QPC();

	SampleRecHead& rec = slot->rec;
	rec.qpc = t0;
	rec.suspendTicks = static_cast<uint32_t>(std::min<uint64_t>(t1 - t0, UINT32_MAX));
	rec.flags = 0;
	rec.reserved = 0;

	int nframes = 0;

	UnwindCache* cache = nullptr;

	if (s.dllNotifyCookie != nullptr) {
		const uint32_t epoch = moduleEpoch.load(std::memory_order_acquire);

		if (epoch != s.unwindCacheEpoch) {
			s.unwindCache.Clear();
			s.unwindCacheEpoch = epoch;
		}

		cache = &s.unwindCache;
	}

	if (copied == 0) {
		frames[nframes++] = s.ctx.Rip;
		rec.flags |= SF_STACK_UNKNOWN;
	} else {
		nframes = UnwindStackCopy(s.ctx, rsp, s.stackCopy, copied, s.stackCopyAlloc, frames, s.maxFrames, &rec.flags, cache);
	}

	if (nframes > 0 && s.IsWaitAddress(frames[0]))
		rec.flags |= SF_WAIT;

	rec.nframes = static_cast<uint16_t>(nframes);
	ResolveLua(s, lua, slot);

	s.head.store(head + 1, std::memory_order_release);

	const uint64_t susp = t1 - t0;
	const uint64_t work = QPC() - t1;
	s.suspMin = std::min(s.suspMin, susp);
	s.suspMax = std::max(s.suspMax, susp);
	s.suspSum += susp;
	s.workMax = std::max(s.workMax, work);
	s.workSum += work;
}

DWORD WINAPI SamplerThreadMain(LPVOID param)
{
	Session& s = *static_cast<Session*>(param);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	const HANDLE handles[2] = {s.stopEvent, s.timer};
	uint64_t next = QPC() + s.intervalTicks;

	for (;;) {
		const uint64_t now = QPC();

		if (now < next) {
			LARGE_INTEGER due;
			due.QuadPart = -static_cast<LONGLONG>(((next - now) * 10000000ull) / s.qpcFreq);
			if (due.QuadPart == 0)
				due.QuadPart = -1;

			if (!SetWaitableTimer(s.timer, &due, 0, nullptr, nullptr, FALSE))
				break;
			if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != (WAIT_OBJECT_0 + 1))
				break;
		} else if (WaitForSingleObject(s.stopEvent, 0) == WAIT_OBJECT_0) {
			break;
		}

		TakeSample(s);
		next += s.intervalTicks;

		// Fell behind (sampler descheduled, or a sample took longer than the
		// interval): skip the missed ticks and wait a full interval, so a too
		// small interval degrades the rate instead of spinning this thread.
		const uint64_t after = QPC();
		if (after >= next) {
			s.missedTicks += 1 + (after - next) / s.intervalTicks;
			next = after + s.intervalTicks;
		}
	}

	CancelWaitableTimer(s.timer);
	return 0;
}



// ---------------------------------------------------------------------------
// writer thread

void WriteSample(Session& s, SlotHead* slot)
{
	const SampleRecHead& rec = slot->rec;
	const uint32_t payload = uint32_t(sizeof(SampleRecHead) + rec.nframes * sizeof(uint64_t) + rec.nlua * sizeof(LuaFrameRec));

	s.out.PutT(REC_SAMPLE);
	s.out.PutT(payload);
	s.out.Put(&rec, sizeof(rec));
	s.out.Put(Session::Frames(slot), rec.nframes * sizeof(uint64_t));
	s.out.Put(slot->lua, rec.nlua * sizeof(LuaFrameRec));
}

void DrainRing(Session& s)
{
	uint64_t tail = s.tail.load(std::memory_order_relaxed);
	const uint64_t head = s.head.load(std::memory_order_acquire);

	while (tail < head) {
		WriteSample(s, s.Slot(tail));
		s.tail.store(++tail, std::memory_order_release);
	}
}

DWORD WINAPI WriterThreadMain(LPVOID param)
{
	Session& s = *static_cast<Session*>(param);

	for (;;) {
		const bool stopping = (WaitForSingleObject(s.writerStopEvent, 10) == WAIT_OBJECT_0);
		DrainRing(s);

		if (stopping)
			break;
	}

	s.out.Flush();
	return 0;
}



// ---------------------------------------------------------------------------
// setup / teardown helpers (normal threads, may lock and allocate)

void WriteModules(Session& s)
{
	std::vector<HMODULE> mods(1024);
	DWORD needed = 0;

	for (int attempt = 0; attempt < 4; ++attempt) {
		if (!EnumProcessModules(GetCurrentProcess(), mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &needed))
			return;
		if (needed <= mods.size() * sizeof(HMODULE))
			break;
		mods.resize(needed / sizeof(HMODULE) + 64);
	}

	mods.resize(std::min<size_t>(mods.size(), needed / sizeof(HMODULE)));

	std::vector<wchar_t> wname(32768);
	std::vector<uint8_t> payload;

	for (HMODULE m: mods) {
		MODULEINFO mi;
		if (!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi)))
			continue;

		const DWORD len = GetModuleFileNameW(m, wname.data(), DWORD(wname.size()));
		const std::string name = WideToUtf8(wname.data(), len);
		const uint64_t base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
		const uint64_t size = mi.SizeOfImage;
		const uint32_t nameLen = uint32_t(name.size());

		payload.clear();
		payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(&base), reinterpret_cast<const uint8_t*>(&base) + 8);
		payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(&size), reinterpret_cast<const uint8_t*>(&size) + 8);
		payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(&nameLen), reinterpret_cast<const uint8_t*>(&nameLen) + 4);
		payload.insert(payload.end(), name.begin(), name.end());
		s.out.Record(REC_MODULE, payload.data(), uint32_t(payload.size()));
	}
}

void CollectWaitRanges(Session& s)
{
	// syscall stubs are 32 bytes; a thread blocked in the kernel has its user RIP inside one
	static const char* const ntdllWaits[] = {
		"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtWaitForMultipleObjects32",
		"NtDelayExecution", "NtWaitForAlertByThreadId", "NtWaitForKeyedEvent",
		"NtRemoveIoCompletion", "NtRemoveIoCompletionEx", "NtSignalAndWaitForSingleObject",
		"NtYieldExecution", "NtWaitForWorkViaWorkerFactory", "NtReplyWaitReceivePort",
		"NtReplyWaitReceivePortEx", "NtAlpcSendWaitReceivePort", "NtWaitLowEventPair",
	};
	static const char* const win32uWaits[] = {
		"NtUserMsgWaitForMultipleObjectsEx", "NtUserWaitMessage", "NtUserGetMessage",
		"NtUserWaitForInputIdle", "NtUserRealWaitMessageEx",
		"NtGdiDdDDIWaitForSynchronizationObject", "NtGdiDdDDIWaitForSynchronizationObjectFromCpu",
		"NtGdiDdDDIWaitForSynchronizationObjectFromGpu", "NtGdiDdDDIWaitForVerticalBlankEvent",
		"NtGdiDdDDIWaitForVerticalBlankEvent2",
	};
	constexpr uint64_t STUB_SIZE = 32;

	const auto add = [&](const wchar_t* dll, const char* const* names, size_t count) {
		const HMODULE m = GetModuleHandleW(dll);
		if (m == nullptr)
			return;
		for (size_t i = 0; i < count; ++i) {
			const FARPROC f = GetProcAddress(m, names[i]);
			if (f == nullptr)
				continue;
			const uint64_t a = reinterpret_cast<uintptr_t>(f);
			s.waitRanges.push_back({a, a + STUB_SIZE});
		}
	};

	add(L"ntdll.dll", ntdllWaits, sizeof(ntdllWaits) / sizeof(ntdllWaits[0]));
	add(L"win32u.dll", win32uWaits, sizeof(win32uWaits) / sizeof(win32uWaits[0]));
}

std::string MakeOutputPath(int attempt)
{
	char name[64];
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::snprintf(name, sizeof(name), "sampling-%04u%02u%02u-%02u%02u%02u",
		unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond));

	std::string file = name;
	if (attempt > 0)
		file += "-" + std::to_string(attempt);
	file += ".bin";

	return dataDirsAccess.LocateFile(file, FileQueryFlags::WRITE | FileQueryFlags::CREATE_DIRS);
}

uint64_t ThreadCpu100ns(HANDLE thread)
{
	FILETIME c, e, k, u;
	if (thread == nullptr || !GetThreadTimes(thread, &c, &e, &k, &u))
		return 0;
	return FileTimeTo100ns(k) + FileTimeTo100ns(u);
}

void DestroySession(Session* s)
{
	if (s->dllNotifyCookie != nullptr) {
		if (const auto unreg = NtdllProc<LdrUnregisterDllNotificationFn>("LdrUnregisterDllNotification"))
			unreg(s->dllNotifyCookie);
	}

	for (HANDLE h: {s->samplerThread, s->writerThread, s->stopEvent, s->writerStopEvent, s->timer, s->thread}) {
		if (h != nullptr)
			CloseHandle(h);
	}
	if (s->stackCopy != nullptr)
		VirtualFree(s->stackCopy, 0, MEM_RELEASE);
	if ((s->headerFlags & HDR_FLAG_TIMEPERIOD) != 0)
		timeEndPeriod(1);

	delete s;
}

double TicksToUs(const Session& s, uint64_t ticks) { return (ticks * 1e6) / double(s.qpcFreq); }

} // namespace



namespace SamplingProfiler {

bool Start(int intervalUs)
{
	std::lock_guard<std::mutex> lock(controlMutex);

	if (session != nullptr) {
		LOG_L(L_WARNING, "[SamplingProfiler] already running, writing to %s", session->path.c_str());
		return false;
	}
	if (!Threading::IsMainThread()) {
		LOG_L(L_ERROR, "[SamplingProfiler] Start() must be called on the main thread");
		return false;
	}

	if (intervalUs <= 0)
		intervalUs = configHandler->GetInt("SamplingProfiler");
	if (intervalUs <= 0) {
		LOG_L(L_WARNING, "[SamplingProfiler] disabled (SamplingProfiler=0); set it to the sampling interval in microseconds, or use /sampler start <us>");
		return false;
	}

	std::unique_ptr<Session> sp(new Session());
	Session& s = *sp;

	s.intervalUs = std::clamp(intervalUs, 100, 1000000);
	s.maxFrames = std::clamp(configHandler->GetInt("SamplingProfilerDepth"), 1, 256);
	s.maxLua = std::clamp(configHandler->GetInt("SamplingProfilerLuaDepth"), 0, MAX_LUA_FRAMES);

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	s.qpcFreq = static_cast<uint64_t>(freq.QuadPart);
	s.intervalTicks = std::max<uint64_t>(1, (s.qpcFreq * uint64_t(s.intervalUs)) / 1000000ull);

	// target = this (main) thread; its TLS slot and stack base are resolved here, on it
	s.threadId = GetCurrentThreadId();
	s.thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.threadId);
	s.stackBase = CurrentThreadStackBase();
	s.luaSlot = &luai_activestate;

	const HMODULE exe = GetModuleHandleW(nullptr);
	MODULEINFO exeInfo = {};
	GetModuleInformation(GetCurrentProcess(), exe, &exeInfo, sizeof(exeInfo));
	s.exeBase = reinterpret_cast<uintptr_t>(exe);
	s.exeSize = exeInfo.SizeOfImage;

	CollectWaitRanges(s);

	// preallocate everything the sampler thread touches
	s.stackCopyAlloc = STACK_COPY_CAP + STACK_COPY_SLACK;
	s.stackCopy = static_cast<uint8_t*>(VirtualAlloc(nullptr, s.stackCopyAlloc, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	s.slotStride = (sizeof(SlotHead) + size_t(s.maxFrames) * sizeof(uint64_t) + 63) & ~size_t(63);
	s.ring.assign(s.slotStride * RING_SLOTS, 0);
	s.funcs.Init();
	s.unwindCache.Clear();
	std::memset(&s.luaRaw, 0, sizeof(s.luaRaw));

	// without load/unload notifications a cached function entry could outlive its DLL
	if (const auto reg = NtdllProc<LdrRegisterDllNotificationFn>("LdrRegisterDllNotification")) {
		s.unwindCacheEpoch = moduleEpoch.load();
		if (reg(0, OnDllNotification, nullptr, &s.dllNotifyCookie) != 0)
			s.dllNotifyCookie = nullptr;
	}

	if (s.thread == nullptr || s.stackCopy == nullptr) {
		LOG_L(L_ERROR, "[SamplingProfiler] setup failed (OpenThread/VirtualAlloc error %lu)", GetLastError());
		DestroySession(sp.release());
		return false;
	}

	// prefer a high-resolution timer (Win10 1803+); else 1 ms scheduler granularity
	s.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (s.timer != nullptr) {
		s.headerFlags |= HDR_FLAG_HIGHRES_TIMER;
	} else {
		s.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		timeBeginPeriod(1);
		s.headerFlags |= HDR_FLAG_TIMEPERIOD;
	}

	s.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	s.writerStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

	if (s.timer == nullptr || s.stopEvent == nullptr || s.writerStopEvent == nullptr) {
		LOG_L(L_ERROR, "[SamplingProfiler] setup failed (timer/event error %lu)", GetLastError());
		DestroySession(sp.release());
		return false;
	}

	for (int attempt = 0; ; ++attempt) {
		s.path = MakeOutputPath(attempt);
		const DWORD err = s.out.Create(Utf8ToWide(s.path));

		if (err == 0)
			break;
		if (err != ERROR_FILE_EXISTS || attempt >= 100) {
			LOG_L(L_ERROR, "[SamplingProfiler] cannot create \"%s\" (error %lu)", s.path.c_str(), err);
			DestroySession(sp.release());
			return false;
		}
	}

	FILETIME startTime;
	GetSystemTimeAsFileTime(&startTime);
	s.startQpc = QPC();

	FileHeader hdr = {};
	std::memcpy(hdr.magic, FILE_MAGIC, sizeof(hdr.magic));
	hdr.version = FILE_VERSION;
	hdr.headerSize = sizeof(FileHeader);
	hdr.qpcFrequency = s.qpcFreq;
	hdr.startQpc = s.startQpc;
	hdr.startFileTime = FileTimeTo100ns(startTime);
	hdr.intervalUs = uint32_t(s.intervalUs);
	hdr.maxNativeFrames = uint32_t(s.maxFrames);
	hdr.maxLuaFrames = uint32_t(s.maxLua);
	hdr.stackCopyCap = uint32_t(STACK_COPY_CAP);
	hdr.exeImageBase = s.exeBase;
	hdr.exeImageSize = s.exeSize;
	hdr.threadId = s.threadId;
	hdr.processId = GetCurrentProcessId();
	hdr.flags = s.headerFlags;
	hdr.pointerSize = sizeof(void*);
	s.out.PutT(hdr);
	WriteModules(s);
	s.out.Flush();

	{
		std::lock_guard<std::mutex> namesLock(namesMutex);
		stateNames.clear();
		collectNames.store(true);
	}
	ReportLuaHandleStates();

	s.writerThread = CreateThread(nullptr, 0, WriterThreadMain, &s, 0, nullptr);
	s.samplerThread = (s.writerThread != nullptr)? CreateThread(nullptr, 64 * 1024, SamplerThreadMain, &s, 0, nullptr): nullptr;

	if (s.samplerThread == nullptr) {
		LOG_L(L_ERROR, "[SamplingProfiler] CreateThread failed (error %lu)", GetLastError());
		if (s.writerThread != nullptr) {
			SetEvent(s.writerStopEvent);
			WaitForSingleObject(s.writerThread, INFINITE);
		}
		collectNames.store(false);
		s.out.Close();
		DestroySession(sp.release());
		return false;
	}

	session = sp.release();
	running.store(true);

	LOG("[SamplingProfiler] started: interval=%dus depth=%d luaDepth=%d timer=%s file=%s",
		s.intervalUs, s.maxFrames, s.maxLua,
		((s.headerFlags & HDR_FLAG_HIGHRES_TIMER) != 0)? "high-res": "timeBeginPeriod(1)",
		s.path.c_str());
	return true;
}


bool Stop()
{
	std::lock_guard<std::mutex> lock(controlMutex);

	if (session == nullptr) {
		LOG_L(L_WARNING, "[SamplingProfiler] not running");
		return false;
	}

	Session& s = *session;

	SetEvent(s.stopEvent);
	WaitForSingleObject(s.samplerThread, INFINITE);
	SetEvent(s.writerStopEvent);
	WaitForSingleObject(s.writerThread, INFINITE);
	running.store(false);

	const uint64_t endQpc = QPC();

	// name every Lua state that is still alive (killed ones were noted in KillLua)
	if (Threading::IsMainThread())
		ReportLuaHandleStates();

	std::vector<LuaStateName> names;
	{
		std::lock_guard<std::mutex> namesLock(namesMutex);
		collectNames.store(false);
		names.swap(stateNames);
	}

	// trailing metadata; the reader merges duplicate modules/state names
	WriteModules(s);

	std::vector<uint8_t> payload;
	const auto put = [&payload](const void* p, size_t n) {
		payload.insert(payload.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
	};

	for (const LuaStateName& n: names) {
		const uint8_t synced[4] = {uint8_t(n.synced), 0, 0, 0};
		const uint32_t len = uint32_t(n.name.size());
		payload.clear();
		put(&n.g, 8);
		put(synced, 4);
		put(&len, 4);
		put(n.name.data(), len);
		s.out.Record(REC_LUASTATE, payload.data(), uint32_t(payload.size()));
	}

	for (uint32_t i = 0; i < s.funcs.Count(); ++i) {
		const LuaFuncTable::Entry& e = s.funcs.Get(i);
		payload.clear();
		put(&i, 4);
		put(&e.lineDefined, 4);
		put(&e.lastLineDefined, 4);
		put(&e.srcFullLen, 4);
		put(&e.proto, 8);
		put(&e.srcLen, 4);
		put(e.src, e.srcLen);
		s.out.Record(REC_LUAFUNC, payload.data(), uint32_t(payload.size()));
	}

	const uint64_t samples = s.head.load();
	TrailerRec tr = {};
	tr.endQpc = endQpc;
	tr.samples = samples;
	tr.drops = s.drops.load();
	tr.suspendFailures = s.suspendFailures;
	tr.contextFailures = s.contextFailures;
	tr.missedTicks = s.missedTicks;
	tr.suspendMinTicks = (samples > 0)? s.suspMin: 0;
	tr.suspendMaxTicks = s.suspMax;
	tr.suspendSumTicks = s.suspSum;
	tr.workMaxTicks = s.workMax;
	tr.workSumTicks = s.workSum;
	tr.samplerCpu100ns = ThreadCpu100ns(s.samplerThread);
	tr.writerCpu100ns = ThreadCpu100ns(s.writerThread);
	tr.luaReadFailures = s.luaReadFailures;
	tr.luaFuncCount = s.funcs.Count();
	tr.luaFuncOverflow = s.funcs.Overflow();
	s.out.Record(REC_TRAILER, &tr, sizeof(tr));
	s.out.Close();

	const double seconds = double(endQpc - s.startQpc) / double(s.qpcFreq);
	const double avgSusp = (samples > 0)? TicksToUs(s, s.suspSum) / samples: 0.0;
	const double avgWork = (samples > 0)? TicksToUs(s, s.workSum) / samples: 0.0;
	const double cpuPct = (seconds > 0.0)? (tr.samplerCpu100ns * 1e-7 / seconds) * 100.0: 0.0;

	LOG("[SamplingProfiler] stopped after %.1fs: %" PRIu64 " samples, %" PRIu64 " dropped, %" PRIu64 " suspend/context failures, %" PRIu64 " missed ticks",
		seconds, samples, tr.drops, tr.suspendFailures + tr.contextFailures, tr.missedTicks);
	LOG("[SamplingProfiler] main thread suspended min/avg/max %.1f/%.1f/%.1f us; sampler work avg %.1f us; sampler CPU %.2f%% of a core; %u Lua functions",
		TicksToUs(s, tr.suspendMinTicks), avgSusp, TicksToUs(s, tr.suspendMaxTicks), avgWork, cpuPct, s.funcs.Count());

	if (s.out.Failed()) {
		LOG_L(L_ERROR, "[SamplingProfiler] write error, file is incomplete: %s", s.path.c_str());
	} else {
		LOG("[SamplingProfiler] wrote %s (%.1f MB)", s.path.c_str(), s.out.BytesWritten() / (1024.0 * 1024.0));
	}

	DestroySession(session);
	session = nullptr;
	return true;
}


bool IsRunning() { return running.load(); }


std::string StatusLine()
{
	std::lock_guard<std::mutex> lock(controlMutex);

	if (session == nullptr)
		return "not running (SamplingProfiler=" + std::to_string(configHandler->GetInt("SamplingProfiler")) + ")";

	const Session& s = *session;
	const double seconds = double(QPC() - s.startQpc) / double(s.qpcFreq);
	char buf[512];
	std::snprintf(buf, sizeof(buf), "running for %.1fs at %dus: %" PRIu64 " samples, %" PRIu64 " dropped, file %s",
		seconds, s.intervalUs, s.head.load(std::memory_order_relaxed), s.drops.load(std::memory_order_relaxed), s.path.c_str());
	return buf;
}


void Shutdown()
{
	if (IsRunning())
		Stop();
}


void NoteLuaState(const void* globalState, const std::string& name, bool synced)
{
	if (globalState == nullptr || !collectNames.load(std::memory_order_relaxed))
		return;

	std::lock_guard<std::mutex> lock(namesMutex);

	if (!collectNames.load())
		return;

	for (const LuaStateName& n: stateNames) {
		if (n.g == reinterpret_cast<uintptr_t>(globalState) && n.synced == synced && n.name == name)
			return;
	}

	stateNames.push_back({reinterpret_cast<uintptr_t>(globalState), synced, name});
}

} // namespace SamplingProfiler

#endif // RECOIL_SAMPLING_PROFILER
