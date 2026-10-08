/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Engine-independent building blocks of the Windows x86-64 sampling profiler
// (see SamplingProfiler.cpp for the overall design and the file format):
//
//   SuspendAndGetContext / CopyStack  -- run while the target thread is
//       suspended; they take no locks and allocate nothing.
//   UnwindStackCopy                   -- runs after the target was resumed;
//       walks the *copy* of the stack with RtlLookupFunctionEntry and
//       RtlVirtualUnwind (approach of Chromium's base/profiler stack copier).
//   SafeRead                          -- fault-tolerant read of possibly stale
//       pointers (ReadProcessMemory on our own process).
//
// Kept header-only and free of engine includes so that a stand-alone test
// program (Win/test/SamplingProfilerUnwindTest.cpp) can exercise the same code.

#if !defined(_WIN64) || !(defined(_M_X64) || defined(__x86_64__))
	#error "SamplingProfilerCore.h is x86-64 Windows only"
#endif

#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SamplingProfilerCore {

// per-sample flags (also written to the file, keep in sync with SamplingProfiler.cpp + sampler.py)
enum SampleFlags : uint32_t {
	SF_WAIT            = 1u << 0, // leaf RIP inside a known kernel wait/sleep syscall stub
	SF_TRUNCATED       = 1u << 1, // unwinding left the copied stack range (copy cap hit) or frame too large
	SF_DEPTH_LIMIT     = 1u << 2, // stopped at the configured native depth
	SF_UNWIND_NOPDATA  = 1u << 3, // non-leaf frame without unwind info (JIT code, missing .pdata)
	SF_UNWIND_BADFRAME = 1u << 4, // RSP/frame register not inside the copy, or no progress
	SF_STACK_UNKNOWN   = 1u << 5, // RSP outside the thread's stack: only the leaf frame was recorded
	SF_LUA_ACTIVE      = 1u << 6, // a Lua state was active on the thread (lua frames may still be 0)
	SF_LUA_READ_FAIL   = 1u << 7, // a ReadProcessMemory of Lua data failed (stale pointer, race)
	SF_LUA_TRUNCATED   = 1u << 8, // more Lua frames existed than were recorded
};


// Fault-tolerant memory read. Never touches the memory directly, so a stale or
// garbage pointer only makes the call fail. Safe to call while another thread is
// suspended (ReadProcessMemory is a plain syscall wrapper, no user-mode locks).
inline bool SafeRead(const void* src, void* dst, size_t size)
{
	if (src == nullptr || size == 0)
		return (size == 0);

	SIZE_T got = 0;
	return (ReadProcessMemory(GetCurrentProcess(), src, dst, size, &got) != FALSE && got == size);
}

// Upper end (exclusive) of the calling thread's stack: NT_TIB::StackBase, at
// gs:[0x08] on x64 (read directly; NtCurrentTeb()->... trips a GCC -Warray-bounds
// false positive).
inline uintptr_t CurrentThreadStackBase()
{
	static_assert(offsetof(NT_TIB, StackBase) == 0x08, "x64 TEB layout");
#if defined(_MSC_VER)
	return static_cast<uintptr_t>(__readgsqword(0x08));
#else
	uintptr_t base;
	__asm__ volatile ("movq %%gs:0x08, %0" : "=r"(base));
	return base;
#endif
}


// Suspend the thread and fetch its integer+control registers. SuspendThread is
// asynchronous; GetThreadContext blocks until the thread is really stopped.
// On success the thread is left suspended and the caller must ResumeThread.
inline bool SuspendAndGetContext(HANDLE thread, CONTEXT* ctx, bool* suspendFailed)
{
	*suspendFailed = false;

	if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
		*suspendFailed = true;
		return false;
	}

	ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

	if (!GetThreadContext(thread, ctx)) {
		ResumeThread(thread);
		return false;
	}

	return true;
}

// Copy [rsp, min(stackBase, rsp + cap)) into dst; returns the number of bytes
// copied, 0 if rsp does not look like it is on this stack. Everything between
// RSP and the stack base is committed memory (the guard page is below RSP), so
// a plain copy cannot fault. Must not lock or allocate (target is suspended).
inline size_t CopyStack(uintptr_t rsp, uintptr_t stackBase, void* dst, size_t cap)
{
	constexpr uintptr_t MAX_STACK_RESERVE = uintptr_t(1) << 30;

	if (rsp >= stackBase || (stackBase - rsp) > MAX_STACK_RESERVE || (rsp & 7) != 0)
		return 0;

	size_t size = static_cast<size_t>(stackBase - rsp);
	if (size > cap)
		size = cap;

	size &= ~size_t(7);
	// memcpy is lock-free; the target's stack pages cannot be decommitted under us
	std::memcpy(dst, reinterpret_cast<const void*>(rsp), size);
	return size;
}


// --- x64 unwind-info inspection (only reads the module's own .xdata) ---------

struct UnwindInfoHeader {
	uint8_t versionAndFlags; // Version:3, Flags:5
	uint8_t sizeOfProlog;
	uint8_t countOfCodes;
	uint8_t frameRegAndOffset; // FrameRegister:4, FrameOffset:4 (scaled by 16)
};

struct FrameExtent {
	uint32_t frameRegMask; // bit r set: GPR r is used as frame register by (one of) the chained unwind infos
	uint32_t frameOffset;  // FrameOffset*16 of the primary info
	uint64_t maxReadFromBase; // upper bound on bytes RtlVirtualUnwind reads above the frame base
	bool ok;
};

// Conservative bound on how far above RSP (or the frame-register base) RtlVirtualUnwind
// will read for this function, so we never let it read past the end of our copy.
inline FrameExtent GetFrameExtent(DWORD64 imageBase, const RUNTIME_FUNCTION* rf)
{
	FrameExtent fe = {0, 0, 8 /* return address */, true};
	uint64_t pushedAndAllocated = 0;
	uint64_t maxSaveOffset = 0;
	bool first = true;

	for (int chain = 0; rf != nullptr && chain < 32; ++chain) {
		DWORD unwindRva = rf->UnwindData;

		// RUNTIME_FUNCTION_INDIRECT: UnwindData points to another RUNTIME_FUNCTION
		if ((unwindRva & 1) != 0) {
			rf = reinterpret_cast<const RUNTIME_FUNCTION*>(imageBase + (unwindRva & ~DWORD(1)));
			continue;
		}

		const UnwindInfoHeader* ui = reinterpret_cast<const UnwindInfoHeader*>(imageBase + unwindRva);
		const unsigned version = ui->versionAndFlags & 7;
		const unsigned flags = ui->versionAndFlags >> 3;
		const unsigned frameReg = ui->frameRegAndOffset & 0x0F;

		if (version != 1 && version != 2) {
			fe.ok = false;
			return fe;
		}

		if (frameReg != 0)
			fe.frameRegMask |= (1u << frameReg);
		if (first)
			fe.frameOffset = (ui->frameRegAndOffset >> 4) * 16u;

		const uint16_t* codes = reinterpret_cast<const uint16_t*>(ui + 1);
		const unsigned count = ui->countOfCodes;

		for (unsigned i = 0; i < count; ) {
			const unsigned op   = (codes[i] >> 8) & 0x0F;
			const unsigned info = (codes[i] >> 12) & 0x0F;

			switch (op) {
				case 0: { pushedAndAllocated += 8; i += 1; } break;                       // UWOP_PUSH_NONVOL
				case 1: {                                                                  // UWOP_ALLOC_LARGE
					if (info == 0) {
						if (i + 1 >= count) { fe.ok = false; return fe; }
						pushedAndAllocated += uint64_t(codes[i + 1]) * 8; i += 2;
					} else {
						if (i + 2 >= count) { fe.ok = false; return fe; }
						pushedAndAllocated += uint64_t(codes[i + 1]) | (uint64_t(codes[i + 2]) << 16); i += 3;
					}
				} break;
				case 2: { pushedAndAllocated += info * 8 + 8; i += 1; } break;           // UWOP_ALLOC_SMALL
				case 3: { i += 1; } break;                                                 // UWOP_SET_FPREG
				case 4: {                                                                  // UWOP_SAVE_NONVOL
					if (i + 1 >= count) { fe.ok = false; return fe; }
					maxSaveOffset = std::max<uint64_t>(maxSaveOffset, uint64_t(codes[i + 1]) * 8 + 8); i += 2;
				} break;
				case 5: {                                                                  // UWOP_SAVE_NONVOL_FAR
					if (i + 2 >= count) { fe.ok = false; return fe; }
					maxSaveOffset = std::max<uint64_t>(maxSaveOffset, (uint64_t(codes[i + 1]) | (uint64_t(codes[i + 2]) << 16)) + 8); i += 3;
				} break;
				case 6: { i += (version == 2) ? 1 : 2; } break;                            // v2: UWOP_EPILOG, v1: UWOP_SAVE_XMM
				case 7: { i += 3; } break;                                                 // UWOP_SAVE_XMM_FAR (v1) / spare
				case 8: {                                                                  // UWOP_SAVE_XMM128
					if (i + 1 >= count) { fe.ok = false; return fe; }
					maxSaveOffset = std::max<uint64_t>(maxSaveOffset, uint64_t(codes[i + 1]) * 16 + 16); i += 2;
				} break;
				case 9: {                                                                  // UWOP_SAVE_XMM128_FAR
					if (i + 2 >= count) { fe.ok = false; return fe; }
					maxSaveOffset = std::max<uint64_t>(maxSaveOffset, (uint64_t(codes[i + 1]) | (uint64_t(codes[i + 2]) << 16)) + 16); i += 3;
				} break;
				case 10: { pushedAndAllocated += 48 + 8; i += 1; } break;                // UWOP_PUSH_MACHFRAME (+error code)
				default: { fe.ok = false; return fe; }
			}
		}

		first = false;

		if ((flags & UNW_FLAG_CHAININFO) == 0)
			break;

		// chained RUNTIME_FUNCTION follows the (even-padded) unwind code array
		rf = reinterpret_cast<const RUNTIME_FUNCTION*>(codes + ((count + 1) & ~1u));
	}

	fe.maxReadFromBase = std::max<uint64_t>(pushedAndAllocated + 8, maxSaveOffset) + 64 /* margin */;
	return fe;
}


// Direct-mapped cache RIP -> (function entry, frame extent). The main thread's
// stacks are very repetitive, so this skips most RtlLookupFunctionEntry binary
// searches and unwind-info parsing. Entries point into module .pdata/.xdata:
// Clear() it whenever a DLL was loaded or unloaded (LdrRegisterDllNotification).
struct UnwindCache {
	struct Entry {
		DWORD64 rip;
		DWORD64 imageBase;
		PRUNTIME_FUNCTION rf;
		FrameExtent fe;
	};

	static constexpr size_t SIZE = 4096;

	void Clear() { std::memset(entries, 0, sizeof(entries)); }

	const Entry& Lookup(DWORD64 rip) {
		Entry& e = entries[((rip >> 2) ^ (rip >> 14)) & (SIZE - 1)];
		if (e.rip != rip)
			Fill(e, rip);
		return e;
	}

	static void Fill(Entry& e, DWORD64 rip) {
		e.rip = rip;
		e.imageBase = 0;
		e.rf = RtlLookupFunctionEntry(rip, &e.imageBase, nullptr);
		e.fe = (e.rf != nullptr)? GetFrameExtent(e.imageBase, e.rf): FrameExtent{0, 0, 0, false};
	}

	Entry entries[SIZE];
};

inline const UnwindCache::Entry& LookupUnwind(UnwindCache* cache, DWORD64 rip, UnwindCache::Entry& scratch)
{
	if (cache != nullptr)
		return cache->Lookup(rip);

	UnwindCache::Fill(scratch, rip);
	return scratch;
}


// Unwind a stack *copy*. `captured` is the context taken while the thread was
// suspended; `origRsp` its RSP; the copy holds `copySize` bytes from origRsp and
// `copyAlloc` >= copySize bytes are readable at `copy` (slack, zero-filled).
// Frame 0 is the exact RIP; frames >= 1 are return addresses. `cache` may be null.
// Must only be called after the target thread was resumed (RtlLookupFunctionEntry
// may take loader/function-table locks).
inline int UnwindStackCopy(
	const CONTEXT& captured,
	uintptr_t origRsp,
	const uint8_t* copy,
	size_t copySize,
	size_t copyAlloc,
	uint64_t* frames,
	int maxFrames,
	uint32_t* flags,
	UnwindCache* cache = nullptr
) {
	alignas(16) CONTEXT ctx;
	std::memcpy(&ctx, &captured, sizeof(ctx));

	const uintptr_t origTop = origRsp + copySize;
	const uintptr_t copyLo = reinterpret_cast<uintptr_t>(copy);
	const uintptr_t copyHi = copyLo + copySize;
	const uintptr_t copyEnd = copyLo + copyAlloc;
	const intptr_t delta = static_cast<intptr_t>(copyLo - origRsp);

	// Rax..R15 are 16 consecutive DWORD64s in CONTEXT, indexed by x64 register number
	DWORD64* gpr = &ctx.Rax;

	// any register that points into the original stack range is moved into the copy;
	// done after every unwind step because restored registers hold original addresses
	const auto relocate = [&]() {
		for (int r = 0; r < 16; ++r) {
			if (gpr[r] >= origRsp && gpr[r] < origTop)
				gpr[r] += delta;
		}
	};

	relocate();

	int n = 0;
	bool topFrame = true;

	while (n < maxFrames) {
		const DWORD64 rip = ctx.Rip;
		const DWORD64 rsp = ctx.Rsp;

		if (rip == 0)
			return n;

		// rip was loaded from just below rsp; beyond the copy it would be stale data
		if (rsp < copyLo || rsp > copyHi) {
			*flags |= SF_TRUNCATED;
			return n;
		}

		frames[n++] = rip;

		if (rsp == copyHi) {
			*flags |= SF_TRUNCATED;
			return n;
		}

		UnwindCache::Entry scratch;
		const UnwindCache::Entry& ue = LookupUnwind(cache, rip, scratch);
		const DWORD64 imageBase = ue.imageBase;
		const PRUNTIME_FUNCTION rf = ue.rf;

		if (rf != nullptr) {
			const FrameExtent& fe = ue.fe;

			if (!fe.ok) {
				*flags |= SF_UNWIND_BADFRAME;
				return n;
			}

			if (rsp + fe.maxReadFromBase > copyEnd) {
				*flags |= SF_TRUNCATED;
				return n;
			}

			// RtlVirtualUnwind computes the frame base from the frame register once
			// the prolog has set it. A frame register pointing into the copy gets
			// its reads bounds-checked. One pointing elsewhere is not in use yet
			// (RIP in the prolog before SET_FPREG, or at the final ret), so it is
			// temporarily replaced by an in-copy value: that keeps every read the
			// unwinder might do inside our buffer, and the register is restored if
			// the unwind did not reload it from the stack.
			const DWORD64 fpSubstitute = rsp + fe.frameOffset;
			DWORD64 savedRegs[16];
			uint32_t substituted = 0;

			for (int r = 0; r < 16; ++r) {
				if ((fe.frameRegMask & (1u << r)) == 0)
					continue;

				if (gpr[r] >= copyLo && gpr[r] < copyEnd) {
					const uint64_t fpBase = gpr[r] - fe.frameOffset;
					if (fpBase < copyLo || fpBase + fe.maxReadFromBase > copyEnd) {
						*flags |= SF_TRUNCATED;
						return n;
					}
					continue;
				}

				savedRegs[r] = gpr[r];
				gpr[r] = fpSubstitute;
				substituted |= (1u << r);
			}

			PVOID handlerData = nullptr;
			DWORD64 establisherFrame = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, rip, rf, &ctx, &handlerData, &establisherFrame, nullptr);

			for (int r = 0; r < 16; ++r) {
				if ((substituted & (1u << r)) != 0 && gpr[r] == fpSubstitute)
					gpr[r] = savedRegs[r];
			}
		} else {
			// No unwind info: a leaf function (allowed only at the top by the x64 ABI),
			// with its return address at [RSP]. Anywhere else this is JIT code or a
			// module without .pdata; we cannot continue reliably.
			if (!topFrame) {
				*flags |= SF_UNWIND_NOPDATA;
				return n;
			}

			// Small asm helpers without .pdata may have pushed a few registers first
			// (libgcc's ___chkstk_ms pushes rcx and rax): take the first of the top
			// 3 slots that looks like a return address into code with unwind info.
			int slot = 0;
			for (int k = 0; k < 3 && rsp + 8 * (k + 1) <= copyHi; ++k) {
				UnwindCache::Entry probe;
				if (LookupUnwind(cache, *reinterpret_cast<const DWORD64*>(rsp + 8 * k), probe).rf != nullptr) {
					slot = k;
					break;
				}
			}

			ctx.Rip = *reinterpret_cast<const DWORD64*>(rsp + 8 * slot);
			ctx.Rsp = rsp + 8 * (slot + 1);
		}

		topFrame = false;
		relocate();

		if (ctx.Rsp <= rsp) {
			*flags |= SF_UNWIND_BADFRAME;
			return n;
		}
	}

	if (ctx.Rip != 0)
		*flags |= SF_DEPTH_LIMIT;

	return n;
}

} // namespace SamplingProfilerCore
