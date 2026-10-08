/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Checks the sampling profiler's suspend / stack-copy / unwind path
// (System/Platform/Win/SamplingProfilerCore.h) on real Windows (x86-64 only).
//
// A worker thread spins in a known call chain (frame-pointer function with
// alloca, frameless functions, a large-frame function, recursion) and
// periodically sleeps; the test samples it exactly like the profiler does and
// checks that
//   - every sample unwinds without crashing,
//   - most busy samples contain the whole chain Worker -> FuncA -> FuncB -> FuncC,
//   - a deliberately tiny stack copy stops with SF_TRUNCATED instead of
//     reading outside the copy,
//   - walks with and without the UnwindCache give the same results,
//   - SafeRead fails cleanly on unmapped memory.
// Samples taken while the worker sleeps are expected to have their leaf inside
// ntdll!NtDelayExecution; that count is only reported.

#include "System/Platform/Win/SamplingProfilerCore.h"

#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <catch_amalgamated.hpp>

using namespace SamplingProfilerCore;

namespace {

#if defined(_MSC_VER)
	#define TEST_NOINLINE __declspec(noinline)
#else
	#define TEST_NOINLINE __attribute__((noinline))
#endif

std::atomic<bool> stopWorker{false};
std::atomic<uintptr_t> workerStackBase{0};
std::atomic<int> sleepPhase{0};
volatile uint64_t sink = 0;

TEST_NOINLINE uint64_t Leaf(uint64_t x)
{
	// small busy loop, no calls: frameless leaf (may or may not get .pdata)
	for (int i = 0; i < 200; ++i)
		x = x * 6364136223846793005ull + 1442695040888963407ull;
	return x;
}

TEST_NOINLINE uint64_t LargeFrame(uint64_t x)
{
	volatile uint8_t big[64 * 1024]; // __chkstk + UWOP_ALLOC_LARGE
	big[x & 0xFFFF] = uint8_t(x);
	return Leaf(x + big[(x >> 3) & 0xFFFF]);
}

TEST_NOINLINE uint64_t Recurse(uint64_t x, int depth)
{
	if (depth == 0)
		return LargeFrame(x);
	return Recurse(x ^ uint64_t(depth), depth - 1) + 1;
}

TEST_NOINLINE uint64_t FuncC(uint64_t x)
{
	return Recurse(x, 5) + Leaf(x);
}

TEST_NOINLINE uint64_t FuncB(uint64_t x)
{
	// dynamic stack allocation forces a frame pointer (UWOP_SET_FPREG)
	volatile uint8_t* p = static_cast<volatile uint8_t*>(_alloca(64 + (x & 63)));
	p[0] = uint8_t(x);
	return FuncC(x + p[0]);
}

TEST_NOINLINE uint64_t FuncA(uint64_t x)
{
	return FuncB(x * 3) ^ FuncB(x * 5);
}

DWORD WINAPI Worker(LPVOID)
{
	workerStackBase.store(CurrentThreadStackBase());
	uint64_t x = 1;
	uint32_t iter = 0;

	while (!stopWorker.load(std::memory_order_relaxed)) {
		if (((++iter) & 0xFFF) == 0) {
			sleepPhase.store(1);
			Sleep(2);
			sleepPhase.store(0);
		}
		x = FuncA(x);
	}

	sink = x;
	return 0;
}

struct FuncRange {
	uint64_t lo;
	uint64_t hi;
};

FuncRange RangeOf(const void* fn)
{
	DWORD64 imageBase = 0;
	const uint64_t a = reinterpret_cast<uintptr_t>(fn);
	PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(a, &imageBase, nullptr);

	if (rf == nullptr)
		return {a, a + 1};

	return {imageBase + rf->BeginAddress, imageBase + rf->EndAddress};
}

bool Contains(const FuncRange& r, uint64_t addr, bool isReturnAddress)
{
	const uint64_t a = isReturnAddress? addr - 1: addr;
	return (a >= r.lo && a < r.hi);
}

} // namespace


TEST_CASE("SamplingProfilerSafeRead")
{
	void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	REQUIRE(page != nullptr);
	VirtualFree(page, 0, MEM_RELEASE);

	const int valid = 42;
	uint64_t v = 0;

	CHECK_FALSE(SafeRead(page, &v, sizeof(v)));
	CHECK_FALSE(SafeRead(reinterpret_cast<const void*>(uintptr_t(16)), &v, sizeof(v)));
	CHECK(SafeRead(&valid, &v, sizeof(valid)));
}


TEST_CASE("SamplingProfilerUnwindStackCopy")
{
	constexpr int NUM_SAMPLES = 2000;
	constexpr size_t COPY_CAP = 512 * 1024;
	constexpr size_t COPY_ALLOC = COPY_CAP + 64 * 1024;
	constexpr int MAX_FRAMES = 128;

	stopWorker.store(false);
	workerStackBase.store(0);

	HANDLE thread = CreateThread(nullptr, 1 << 20, Worker, nullptr, 0, nullptr);
	REQUIRE(thread != nullptr);

	while (workerStackBase.load() == 0)
		Sleep(1);
	Sleep(20);

	const FuncRange chain[] = {
		RangeOf(reinterpret_cast<const void*>(&Worker)),
		RangeOf(reinterpret_cast<const void*>(&FuncA)),
		RangeOf(reinterpret_cast<const void*>(&FuncB)),
		RangeOf(reinterpret_cast<const void*>(&FuncC)),
	};
	const uint64_t delayExecAddr = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDelayExecution"));
	const FuncRange delayExec = {delayExecAddr, delayExecAddr + 32};

	uint8_t* copy = static_cast<uint8_t*>(VirtualAlloc(nullptr, COPY_ALLOC, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	REQUIRE(copy != nullptr);

	alignas(16) static CONTEXT ctx;
	static UnwindCache cache; // cached and uncached walks of the same copy must agree
	cache.Clear();

	uint64_t frames[MAX_FRAMES];
	uint64_t framesCached[MAX_FRAMES];

	int sampleFailures = 0, cacheMismatches = 0;
	int busy = 0, busyFull = 0, sleeping = 0, sleepingInStub = 0, truncatedOk = 0, truncatedRuns = 0;

	for (int i = 0; i < NUM_SAMPLES; ++i) {
		bool suspendFailed = false;

		if (!SuspendAndGetContext(thread, &ctx, &suspendFailed)) {
			++sampleFailures;
			continue;
		}

		const int phase = sleepPhase.load();
		const uintptr_t rsp = ctx.Rsp;
		// every 10th sample: tiny copy to exercise the truncation guards
		const size_t cap = ((i % 10) == 9)? 256: COPY_CAP;
		const size_t copied = CopyStack(rsp, workerStackBase.load(), copy, cap);
		ResumeThread(thread);

		if (copied == 0) {
			++sampleFailures;
			continue;
		}

		uint32_t flags = 0;
		uint32_t flagsCached = 0;
		const int n = UnwindStackCopy(ctx, rsp, copy, copied, COPY_ALLOC, frames, MAX_FRAMES, &flags, nullptr);
		const int nc = UnwindStackCopy(ctx, rsp, copy, copied, COPY_ALLOC, framesCached, MAX_FRAMES, &flagsCached, &cache);

		if (n != nc || flags != flagsCached || std::memcmp(frames, framesCached, n * sizeof(uint64_t)) != 0)
			++cacheMismatches;

		if (cap != COPY_CAP) {
			++truncatedRuns;
			// must stop early (or legitimately finish) without walking stale buffer contents
			truncatedOk += ((flags & (SF_TRUNCATED | SF_UNWIND_BADFRAME)) != 0 || n < 8);
			continue;
		}

		if (phase != 0) {
			++sleeping;
			sleepingInStub += (n > 0 && Contains(delayExec, frames[0], false));
			continue;
		}

		++busy;

		// expect Worker, FuncA, FuncB, FuncC in this order (outermost first) among the frames
		int next = 0;
		for (int f = n - 1; f >= 0 && next < 4; --f) {
			if (Contains(chain[next], frames[f], f > 0))
				++next;
		}
		busyFull += (next == 4);
	}

	stopWorker.store(true);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	VirtualFree(copy, 0, MEM_RELEASE);

	INFO("busy " << busy << ", full chain " << busyFull << ", sleeping " << sleeping << " (leaf in NtDelayExecution: " << sleepingInStub << ")");
	CHECK(sampleFailures == 0);
	CHECK(cacheMismatches == 0);
	CHECK(truncatedOk == truncatedRuns);
	REQUIRE(busy > 0);
	CHECK(busyFull * 100 >= busy * 95);
}
