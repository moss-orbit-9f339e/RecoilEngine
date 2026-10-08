/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <string>

/**
 * Low-overhead statistical profiler of the main (sim+render) thread.
 *
 * Implemented for x86-64 Windows only (System/Platform/Win/SamplingProfiler.cpp);
 * everything is an inline no-op elsewhere. Config:
 *   SamplingProfiler          0 = disabled, else default sampling interval in microseconds
 *   SamplingProfilerDepth     max native frames per sample (default 64)
 *   SamplingProfilerLuaDepth  max Lua frames per sample (default 4, 0 = no Lua)
 * Control: /sampler start [intervalUs] | stop | status  (UnsyncedGameCommands)
 * Output:  <writedir>/sampling-YYYYmmdd-HHMMSS.bin, read with tools/sampling-profiler/sampler.py
 */
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
	// keep in sync with LUAI_TRACK_ACTIVESTATE in lib/lua/src/lstate.h
	#define RECOIL_SAMPLING_PROFILER 1
#endif

namespace SamplingProfiler {
#ifdef RECOIL_SAMPLING_PROFILER
	/// Must be called on the main thread (the thread that gets sampled).
	/// intervalUs <= 0 means "use the SamplingProfiler config value".
	/// Returns false (and logs why) if disabled, already running or on error.
	bool Start(int intervalUs = 0);
	/// Stops sampling and writes the file; returns false if not running.
	bool Stop();
	bool IsRunning();
	/// One-line human readable state, for /sampler status.
	std::string StatusLine();
	/// Stop() if running; called on engine shutdown.
	void Shutdown();

	/// Associates a Lua global_State with a display name ("LuaUI", "LuaRules")
	/// for the current output file. Cheap no-op when not running.
	void NoteLuaState(const void* globalState, const std::string& name, bool synced);

	/// Implemented in Lua/LuaHandle.cpp: calls NoteLuaState for every live Lua handle state.
	/// Main thread only.
	void ReportLuaHandleStates();
#else
	inline bool Start(int = 0) { return false; }
	inline bool Stop() { return false; }
	inline bool IsRunning() { return false; }
	inline std::string StatusLine() { return "the sampling profiler is only available on x86-64 Windows"; }
	inline void Shutdown() {}
	inline void NoteLuaState(const void*, const std::string&, bool) {}
#endif
}
