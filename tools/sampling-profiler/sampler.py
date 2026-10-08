#!/usr/bin/env python3
"""Analyse a Recoil sampling-profiler capture (sampling-YYYYmmdd-HHMMSS.bin).

  sampler.py <file.bin> --exe <spring.exe with DWARF> [--top N] [--folded out.txt] [--lua]
             [--dbg NAME=PATH ...] [--exclude-wait] [--no-inline] [--addr2line PATH]

The engine (rts/System/Platform/Win/SamplingProfiler.cpp, which documents the
format) samples the main thread with a stack-copy sampler: absolute return addresses plus up to N
Lua frames (source, linedefined, current line, or the lua_CFunction address) of the Lua state that
was running. Enable with config SamplingProfiler=<interval us>, bracket with /sampler start|stop.

Addresses are mapped to module+offset with the module list in the file. spring.exe addresses
(and any --dbg NAME=PATH module with DWARF) are turned into the file's VMA (offset + preferred
ImageBase from the PE header) and resolved in one batch with `addr2line -f -C -i -a`; return
addresses (all frames but the leaf) use addr-1. Inlined functions become separate frames. Other
modules show as dll!NearestExport (written by the engine at stop; approximate for internal
functions) or dll+0xoffset.

Report: totals and sampler overhead, wait share (leaf in a kernel wait stub), top self functions
(leaf, innermost inline frame), top inclusive functions (each function once per sample), and
optionally a folded-stacks file for flamegraph.pl / speedscope / inferno.

--lua: samples whose leaf is in the Lua runtime (rts/lib/lua, the Lua allocator; CRT/streflop
helpers called from it count too) are attributed to the innermost recorded Lua function
(source:linedefined). Reports top Lua functions by self and by Lua-inclusive samples (every sample
taken while a Lua state was running, each recorded Lua frame once), C functions called from Lua
(the engine's Lua API, any leaf), and per Lua state (LuaUI, LuaRules synced/unsynced, ...). In the
folded output the Lua runtime frames are replaced by the recorded Lua frames.
"""
import argparse
import bisect
import os
import re
import shutil
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"RCLSMPL\0"
HEADER = struct.Struct("<8sIIQQQIIIIQQIIIIQ")
REC = struct.Struct("<II")
SAMPLE = struct.Struct("<QIIQQHHI")
LUAFRAME = struct.Struct("<BBHiQ")
MODULE = struct.Struct("<QQI")
LUASTATE = struct.Struct("<Q4sI")
LUAFUNC = struct.Struct("<IiiIQI")
SYMBOL = struct.Struct("<QQI")
TRAILER_FIELDS = ("endQpc samples drops suspendFailures contextFailures missedTicks suspendMinTicks "
                  "suspendMaxTicks suspendSumTicks workMaxTicks workSumTicks samplerCpu100ns "
                  "writerCpu100ns luaReadFailures luaFuncCount luaFuncOverflow").split()
TRAILER = struct.Struct("<%dQ" % len(TRAILER_FIELDS))

REC_MODULE, REC_SAMPLE, REC_LUASTATE, REC_LUAFUNC, REC_SYMBOL, REC_TRAILER = 1, 2, 3, 4, 5, 6

# sample flags (SamplingProfilerCore::SampleFlags)
SF_WAIT = 1 << 0
SF_TRUNCATED = 1 << 1
SF_DEPTH_LIMIT = 1 << 2
SF_UNWIND_NOPDATA = 1 << 3
SF_UNWIND_BADFRAME = 1 << 4
SF_STACK_UNKNOWN = 1 << 5
SF_LUA_ACTIVE = 1 << 6
SF_LUA_READ_FAIL = 1 << 7
SF_LUA_TRUNCATED = 1 << 8

LF_LUA, LF_C, LF_OTHER, LF_UNREADABLE = 1, 2, 3, 4

# source files of the Lua runtime: the VM, its C API and libs, the engine's Lua allocator
LUA_RUNTIME_PATHS = ("/lib/lua/", "\\lib\\lua\\", "LuaMemPool.", "LuaUser.")
# helpers that are "transparent" for Lua self attribution when called from the Lua runtime
RUNTIME_HELPER_PATHS = ("/streflop/", "/mimalloc/", "/smmalloc/", "libgcc", "libstdc++", "/include/c++/",
                        "/mingw-w64", "/mingw/", "/crt/", "winpthreads")


@dataclass
class Module:
    base: int
    size: int
    path: str

    @property
    def name(self):
        return re.split(r"[\\/]", self.path)[-1] or self.path


@dataclass
class Sample:
    qpc: int
    suspend_ticks: int
    flags: int
    lua_g: int
    lua_l: int
    frames: tuple
    lua: tuple  # ((kind, line, value), ...) innermost first


@dataclass
class LuaFunc:
    id: int
    linedefined: int
    lastlinedefined: int
    full_len: int
    proto: int
    source: str


@dataclass
class Capture:
    path: str
    version: int = 0
    qpc_freq: int = 1
    start_qpc: int = 0
    start_filetime: int = 0
    interval_us: int = 0
    max_frames: int = 0
    max_lua: int = 0
    stack_cap: int = 0
    exe_base: int = 0
    exe_size: int = 0
    thread_id: int = 0
    process_id: int = 0
    header_flags: int = 0
    modules: list = field(default_factory=list)
    samples: list = field(default_factory=list)
    lua_states: dict = field(default_factory=dict)  # g -> [name, ...]
    lua_funcs: dict = field(default_factory=dict)  # id -> LuaFunc
    symbols: dict = field(default_factory=dict)  # addr -> (symaddr, name)
    trailer: dict = None
    warnings: list = field(default_factory=list)

    _bases: list = field(default_factory=list, repr=False)

    def finish(self):
        uniq = {}
        for m in self.modules:
            uniq[(m.base, m.size, m.path)] = m
        self.modules = sorted(uniq.values(), key=lambda m: m.base)
        self._bases = [m.base for m in self.modules]

    def module_of(self, addr):
        i = bisect.bisect_right(self._bases, addr) - 1
        if i >= 0:
            m = self.modules[i]
            if addr < m.base + m.size:
                return m
        return None

    @property
    def exe(self):
        for m in self.modules:
            if m.base == self.exe_base:
                return m
        return None

    def state_name(self, g):
        if not g:
            return "(no Lua state)"
        names = self.lua_states.get(g)
        return "|".join(names) if names else "state@0x%x" % g

    def seconds(self, ticks):
        return ticks / float(self.qpc_freq or 1)


def parse(path):
    data = Path(path).read_bytes()
    cap = Capture(path=str(path))
    if len(data) < HEADER.size:
        raise ValueError("%s: too short for a sampler header" % path)
    (magic, cap.version, header_size, cap.qpc_freq, cap.start_qpc, cap.start_filetime, cap.interval_us,
     cap.max_frames, cap.max_lua, cap.stack_cap, cap.exe_base, cap.exe_size, cap.thread_id, cap.process_id,
     cap.header_flags, pointer_size, _reserved) = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError("%s: bad magic %r (not a Recoil sampling capture)" % (path, magic))
    if cap.version != 1:
        cap.warnings.append("format version %d, this reader knows 1" % cap.version)
    if pointer_size != 8:
        raise ValueError("%s: pointer size %d not supported" % (path, pointer_size))

    off = header_size
    end = len(data)
    while off < end:
        if off + REC.size > end:
            cap.warnings.append("truncated record header at offset %d" % off)
            break
        rtype, size = REC.unpack_from(data, off)
        off += REC.size
        if off + size > end:
            cap.warnings.append("truncated record (type %d, %d bytes) at offset %d; file cut short?" % (rtype, size, off))
            break
        p = data[off:off + size]
        off += size

        if rtype == REC_SAMPLE:
            qpc, susp, flags, g, L, nframes, nlua, _ = SAMPLE.unpack_from(p, 0)
            o = SAMPLE.size
            frames = struct.unpack_from("<%dQ" % nframes, p, o)
            o += 8 * nframes
            lua = tuple((k, line, v) for k, _p0, _p1, line, v in LUAFRAME.iter_unpack(p[o:o + LUAFRAME.size * nlua]))
            cap.samples.append(Sample(qpc, susp, flags, g, L, frames, lua))
        elif rtype == REC_MODULE:
            base, msize, nlen = MODULE.unpack_from(p, 0)
            cap.modules.append(Module(base, msize, p[MODULE.size:MODULE.size + nlen].decode("utf-8", "replace")))
        elif rtype == REC_LUASTATE:
            g, synced, nlen = LUASTATE.unpack_from(p, 0)
            name = p[LUASTATE.size:LUASTATE.size + nlen].decode("utf-8", "replace")
            if name in ("LuaRules", "LuaGaia") or synced[0]:
                name = "%s %s" % (name, "synced" if synced[0] else "unsynced")
            names = cap.lua_states.setdefault(g, [])
            if name not in names:
                names.append(name)
        elif rtype == REC_LUAFUNC:
            fid, ld, lld, full, proto, slen = LUAFUNC.unpack_from(p, 0)
            src = p[LUAFUNC.size:LUAFUNC.size + slen].decode("utf-8", "replace")
            cap.lua_funcs[fid] = LuaFunc(fid, ld, lld, full, proto, src)
        elif rtype == REC_SYMBOL:
            addr, symaddr, nlen = SYMBOL.unpack_from(p, 0)
            cap.symbols[addr] = (symaddr, p[SYMBOL.size:SYMBOL.size + nlen].decode("utf-8", "replace"))
        elif rtype == REC_TRAILER:
            n = min(len(p) // 8, len(TRAILER_FIELDS))
            cap.trailer = dict(zip(TRAILER_FIELDS, struct.unpack_from("<%dQ" % n, p, 0)))
        # unknown record types are skipped
    if cap.trailer is None:
        cap.warnings.append("no trailer: session was not stopped cleanly (crash?); stats are partial")
    cap.finish()
    return cap


def pe_layout(path):
    """(preferred ImageBase, SizeOfImage) from a PE32+ file."""
    with open(path, "rb") as f:
        head = f.read(4096)
    if head[:2] != b"MZ":
        raise ValueError("%s: not a PE file" % path)
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    if head[pe:pe + 4] != b"PE\0\0":
        raise ValueError("%s: no PE signature" % path)
    opt = pe + 4 + 20
    magic = struct.unpack_from("<H", head, opt)[0]
    if magic != 0x20B:
        raise ValueError("%s: not PE32+ (magic 0x%x)" % (path, magic))
    image_base = struct.unpack_from("<Q", head, opt + 24)[0]
    size_of_image = struct.unpack_from("<I", head, opt + 56)[0]
    return image_base, size_of_image


def find_addr2line(explicit=None):
    for cand in (explicit, "x86_64-w64-mingw32-addr2line", "addr2line"):
        if cand and (os.path.isfile(cand) or shutil.which(cand)):
            return cand
    raise SystemExit("addr2line not found (pass --addr2line)")


class Addr2Line:
    """Batch symbolizer: VMA -> [(function, file, line), ...] innermost inline frame first."""

    def __init__(self, binary, path, inline=True):
        self.binary = binary
        self.path = path
        self.inline = inline

    def resolve(self, vmas):
        vmas = sorted(set(vmas))
        if not vmas:
            return {}
        args = [self.binary, "-f", "-C", "-a", "-e", self.path] + (["-i"] if self.inline else [])
        out = subprocess.run(args, input="".join("0x%x\n" % a for a in vmas), capture_output=True, text=True,
                             check=True).stdout
        res, cur, pending = {}, None, None
        for line in out.splitlines():
            if re.fullmatch(r"0x[0-9a-fA-F]+", line):
                cur = int(line, 16)
                res[cur] = []
                pending = None
            elif cur is None:
                continue
            elif pending is None:
                pending = line.strip()
            else:
                file, _, ln = line.strip().rpartition(":")
                ln = ln.split(" ")[0]  # "123 (discriminator 2)"
                res[cur].append((pending, file, int(ln) if ln.isdigit() else 0))
                pending = None
        return res


@dataclass
class Frame:
    name: str  # function (or dll!export, dll+0xoff)
    file: str = ""
    line: int = 0
    module: str = ""


class Resolver:
    """Maps sample addresses to frames (innermost inline first) for each native frame."""

    def __init__(self, cap, debug_files, addr2line=None, inline=True, symbolizers=None):
        self.cap = cap
        self.inline = inline
        self.cache = {}
        self.dwarf = {}  # module base -> (symbolizer, preferred base)
        for m in cap.modules:
            path = debug_files.get(m.name.lower())
            if not path:
                continue
            pref, size = pe_layout(path)
            if size != m.size:
                cap.warnings.append("%s: SizeOfImage 0x%x != loaded module size 0x%x -- wrong binary?"
                                    % (path, size, m.size))
            sym = (symbolizers or {}).get(m.name.lower()) or Addr2Line(addr2line or find_addr2line(), path, inline)
            self.dwarf[m.base] = (sym, pref)

    def prepare(self, keys):
        """keys: iterable of lookup addresses (leaf: addr, others: addr-1)."""
        per_mod = defaultdict(set)
        for a in keys:
            if a in self.cache:
                continue
            m = self.cap.module_of(a)
            if m is not None and m.base in self.dwarf:
                per_mod[m.base].add(a)
            else:
                self.cache[a] = [self._plain(a, m)]
        for base, addrs in per_mod.items():
            sym, pref = self.dwarf[base]
            m = self.cap.module_of(base)
            got = sym.resolve([a - base + pref for a in addrs])
            for a in addrs:
                entries = got.get(a - base + pref) or []
                frames = [Frame(fn, f, ln, m.name) for fn, f, ln in entries if fn and fn != "??"]
                if not self.inline and frames:
                    frames = frames[-1:]
                self.cache[a] = frames or [Frame("%s+0x%x" % (m.name, a - m.base), module=m.name)]

    def _plain(self, a, m):
        if m is None:
            return Frame("[unknown 0x%x]" % a)
        # the engine resolved non-exe addresses by raw address; return addresses were looked up as-is
        for cand in (a, a + 1):
            s = self.cap.symbols.get(cand)
            if s:
                return Frame("%s!%s" % (m.name, s[1]), module=m.name)
        return Frame("%s+0x%x" % (m.name, a - m.base), module=m.name)

    def frames(self, addr):
        return self.cache[addr]


def lookup_keys(sample):
    return [a if i == 0 else a - 1 for i, a in enumerate(sample.frames)]


def is_lua_runtime(frame):
    f = frame.file.replace("\\", "/")
    return any(p.replace("\\", "/") in f for p in LUA_RUNTIME_PATHS)


def is_helper(frame, exe_name):
    if frame.module and frame.module != exe_name:
        return True  # ntdll/ucrtbase/... called from the Lua runtime (memcpy, malloc)
    f = frame.file.replace("\\", "/")
    return (not f) or f.startswith("??") or any(p in f for p in RUNTIME_HELPER_PATHS)


class Analysis:
    def __init__(self, cap, resolver, exclude_wait=False):
        self.cap = cap
        self.res = resolver
        self.samples = [s for s in cap.samples if not (exclude_wait and s.flags & SF_WAIT)]
        keys = set()
        for s in self.samples:
            keys.update(lookup_keys(s))
            for kind, _line, value in s.lua:
                if kind == LF_C:
                    keys.add(value)
        resolver.prepare(keys)
        exe = cap.exe
        self.exe_name = exe.name if exe else ""

    # --- native -------------------------------------------------------------------------------
    def native_stack(self, s):
        """Frames root -> leaf, inline frames expanded (outermost first within one address)."""
        out = []
        for a in reversed(lookup_keys(s)):
            out.extend(reversed(self.res.frames(a)))
        return out

    def self_counts(self):
        c = Counter()
        for s in self.samples:
            if s.frames:
                c[self.res.frames(lookup_keys(s)[0])[0].name] += 1
        return c

    def inclusive_counts(self):
        c = Counter()
        for s in self.samples:
            c.update({f.name for a in lookup_keys(s) for f in self.res.frames(a)})
        return c

    # --- Lua ----------------------------------------------------------------------------------
    def lua_frame_name(self, kind, line, value, with_line=False):
        if kind == LF_LUA:
            fn = self.cap.lua_funcs.get(value)
            if fn is None:
                return "[Lua function #%d]" % value if value != 0xFFFFFFFF else "[Lua function table full]"
            src = fn.source[1:] if fn.source[:1] in ("@", "=") else fn.source
            src = src.replace("\n", " ").replace(";", ",")
            if fn.full_len > len(fn.source):
                src += "..."
            return "%s:%d" % (src, fn.linedefined) + (" @%d" % line if with_line and line >= 0 else "")
        if kind == LF_C:
            fr = self.res.frames(value)
            return "[C] " + fr[-1].name
        return "[not a function]" if kind == LF_OTHER else "[unreadable]"

    def leaf_in_lua(self, s):
        """True if the CPU was in the Lua runtime (walking up through helper frames)."""
        for a in lookup_keys(s):
            for f in self.res.frames(a)[-1:]:  # the containing (non-inlined) function
                if is_lua_runtime(f):
                    return True
                if not is_helper(f, self.exe_name):
                    return False
        return False

    def lua_self_key(self, s):
        for kind, line, value in s.lua:
            if kind == LF_LUA:
                return self.lua_frame_name(kind, line, value)
        if s.lua:
            return self.lua_frame_name(*s.lua[0])
        return "[no Lua frame] " + self.cap.state_name(s.lua_g)

    def lua_report(self):
        total = len(self.samples)
        lua_self, lua_incl, c_from_lua = Counter(), Counter(), Counter()
        state_incl, state_self = Counter(), Counter()
        for s in self.samples:
            active = bool(s.flags & SF_LUA_ACTIVE)
            if not active:
                continue
            state = self.cap.state_name(s.lua_g)
            state_incl[state] += 1
            in_lua = self.leaf_in_lua(s)
            if in_lua:
                state_self[state] += 1
                lua_self[self.lua_self_key(s)] += 1
            lua_incl.update({self.lua_frame_name(*f) for f in s.lua if f[0] in (LF_LUA, LF_C)})
            if s.lua and s.lua[0][0] == LF_C:
                c_from_lua[self.lua_frame_name(*s.lua[0])] += 1
        return dict(total=total, lua_self=lua_self, lua_incl=lua_incl, c_from_lua=c_from_lua,
                    state_incl=state_incl, state_self=state_self)

    # --- folded -------------------------------------------------------------------------------
    def folded(self, with_lua=False):
        c = Counter()
        for s in self.samples:
            stack = [f.name for f in self.native_stack(s)]
            if with_lua and s.lua:
                stack = self._splice_lua(s, stack)
            c[";".join(n.replace(";", ":").replace("\n", " ") for n in stack)] += 1
        return c

    def _splice_lua(self, s, names):
        """Replace the Lua runtime frames by state + recorded Lua frames (outermost first)."""
        native = []
        for a in reversed(lookup_keys(s)):
            native.extend(reversed(self.res.frames(a)))
        idx = [i for i, f in enumerate(native) if is_lua_runtime(f)]
        if not idx:
            return names
        first, last = idx[0], idx[-1]
        lua = [self.lua_frame_name(*f) for f in reversed(s.lua)]
        if s.flags & SF_LUA_TRUNCATED:
            lua.insert(0, "[...]")
        head = [f.name for f in native[:first]]
        tail = [f.name for f in native[last + 1:]]
        return head + ["[Lua] " + self.cap.state_name(s.lua_g)] + lua + tail


def pct(n, total):
    return 100.0 * n / total if total else 0.0


def print_table(title, counter, total, top, unit="samples"):
    print("\n%s" % title)
    print("  %8s %6s  %s" % (unit, "%", "name"))
    for name, n in counter.most_common(top):
        print("  %8d %5.1f%%  %s" % (n, pct(n, total), name))


def summary(cap, an):
    s = cap.samples
    n = len(s)
    print("capture   %s" % cap.path)
    for w in cap.warnings:
        print("warning   %s" % w)
    if n == 0:
        print("no samples")
        return
    dur = cap.seconds(s[-1].qpc - s[0].qpc) if n > 1 else 0.0
    exe = cap.exe
    print("samples   %d over %.2f s (%.0f/s, interval %d us, %s timer), %d modules, exe %s"
          % (n, dur, (n - 1) / dur if dur > 0 else 0, cap.interval_us,
             "high-res" if cap.header_flags & 1 else "1 ms", len(cap.modules), exe.name if exe else "?"))
    t = cap.trailer
    if t:
        freq = cap.qpc_freq
        us = lambda ticks: ticks * 1e6 / freq
        avg_susp = us(t["suspendSumTicks"]) / t["samples"] if t["samples"] else 0.0
        avg_work = us(t["workSumTicks"]) / t["samples"] if t["samples"] else 0.0
        wall = cap.seconds(t["endQpc"] - cap.start_qpc)
        print("overhead  main thread suspended min/avg/max %.1f/%.1f/%.1f us; sampler work avg %.1f us max %.1f us"
              % (us(t["suspendMinTicks"]), avg_susp, us(t["suspendMaxTicks"]), avg_work, us(t["workMaxTicks"])))
        print("          sampler CPU %.2f%% + writer CPU %.2f%% of one core; %d dropped, %d suspend/context "
              "failures, %d missed ticks, %d Lua read failures, %d Lua functions%s"
              % (pct(t["samplerCpu100ns"] * 1e-7, wall), pct(t["writerCpu100ns"] * 1e-7, wall), t["drops"],
                 t["suspendFailures"] + t["contextFailures"], t["missedTicks"], t["luaReadFailures"],
                 t["luaFuncCount"], " (%d overflow)" % t["luaFuncOverflow"] if t["luaFuncOverflow"] else ""))
    flags = Counter()
    for x in s:
        for bit, name in ((SF_WAIT, "wait"), (SF_TRUNCATED, "truncated"), (SF_DEPTH_LIMIT, "depth-limit"),
                          (SF_UNWIND_NOPDATA, "no-pdata"), (SF_UNWIND_BADFRAME, "bad-frame"),
                          (SF_STACK_UNKNOWN, "stack-unknown"), (SF_LUA_ACTIVE, "lua-active"),
                          (SF_LUA_READ_FAIL, "lua-read-fail")):
            if x.flags & bit:
                flags[name] += 1
    depth = sum(len(x.frames) for x in s) / n
    print("waiting   %.1f%% of samples (leaf in a kernel wait/sleep stub)" % pct(flags["wait"], n))
    print("stacks    mean depth %.1f; %s" % (depth, ", ".join("%s %.1f%%" % (k, pct(v, n))
                                                            for k, v in sorted(flags.items()) if k != "wait")))
    if len(an.samples) != n:
        print("analysed  %d samples (waiting samples excluded)" % len(an.samples))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--exe", help="spring.exe with DWARF debug info (the exact build that was sampled)")
    ap.add_argument("--dbg", action="append", default=[], metavar="NAME=PATH",
                    help="extra module with DWARF, e.g. OpenAL32.dll=/path/OpenAL32.dll (repeatable)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--folded", metavar="OUT", help="write folded stacks (flamegraph.pl input)")
    ap.add_argument("--lua", action="store_true", help="Lua attribution report (and Lua frames in --folded)")
    ap.add_argument("--exclude-wait", action="store_true", help="drop samples whose leaf is a kernel wait")
    ap.add_argument("--no-inline", action="store_true", help="do not expand inlined functions")
    ap.add_argument("--addr2line", help="addr2line binary (default x86_64-w64-mingw32-addr2line)")
    args = ap.parse_args(argv)

    cap = parse(args.file)
    debug = {}
    if args.exe:
        exe = cap.exe
        debug[(exe.name if exe else Path(args.exe).name).lower()] = args.exe
    for d in args.dbg:
        name, _, path = d.partition("=")
        debug[name.lower()] = path
    if not args.exe:
        print("note: no --exe given, spring.exe frames stay module+offset")

    res = Resolver(cap, debug, args.addr2line, inline=not args.no_inline)
    an = Analysis(cap, res, exclude_wait=args.exclude_wait)
    summary(cap, an)
    total = len(an.samples)
    if not total:
        return 0

    print_table("top self (leaf function, innermost inline frame)", an.self_counts(), total, args.top)
    print_table("top inclusive (each function counted once per sample)", an.inclusive_counts(), total, args.top)

    if args.lua:
        r = an.lua_report()
        lua_total = sum(r["state_incl"].values())
        print("\nLua: a state was running in %d samples (%.1f%%), the Lua runtime was the leaf in %d (%.1f%%)"
              % (lua_total, pct(lua_total, total), sum(r["state_self"].values()),
                 pct(sum(r["state_self"].values()), total)))
        print("\nper Lua state (%% of all samples)")
        print("  %8s %6s %8s %6s  %s" % ("incl", "%", "self", "%", "state"))
        for name, n in r["state_incl"].most_common():
            sf = r["state_self"][name]
            print("  %8d %5.1f%% %8d %5.1f%%  %s" % (n, pct(n, total), sf, pct(sf, total), name))
        print_table("top Lua functions by self samples (leaf in the Lua runtime)", r["lua_self"], total, args.top)
        print_table("top Lua functions by Lua-inclusive samples", r["lua_incl"], total, args.top)
        print_table("C functions called from Lua (innermost Lua frame is C; inclusive)", r["c_from_lua"], total,
                    args.top)

    if args.folded:
        folded = an.folded(with_lua=args.lua)
        with open(args.folded, "w") as f:
            for stack, n in sorted(folded.items()):
                f.write("%s %d\n" % (stack, n))
        print("\nwrote %s (%d unique stacks)" % (args.folded, len(folded)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
