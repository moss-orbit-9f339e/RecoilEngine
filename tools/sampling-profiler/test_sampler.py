#!/usr/bin/env python3
"""Self-test for sampler.py with synthetic captures (no Windows needed).

  cd tools/sampling-profiler && python3 test_sampler.py      (or: pytest test_sampler.py)

A small writer below produces files in the engine's format (SamplingProfiler.cpp, version 1). The
tests check parsing, module/VMA mapping (runtime base != preferred ImageBase, return address - 1),
self/inclusive counting with recursion and inlined frames, the wait share, Lua attribution (Lua
runtime leaf, CRT helper leaf called from Lua, engine API leaf, per state), folded output with and
without Lua splicing, truncated files, and the CLI end to end through a fake addr2line that speaks
binutils' -f -i -a output format. If a mingw toolchain is installed, a last test compiles a tiny
program with DWARF and resolves addresses through the real x86_64-w64-mingw32-addr2line.
"""
import contextlib
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sampler  # noqa: E402

QPC_FREQ = 10_000_000
EXE_BASE = 0x7FF6_1000_0000   # runtime (ASLR) base of spring.exe in the capture
EXE_SIZE = 0x0040_0000
PREF_BASE = 0x1_4000_0000      # preferred ImageBase in the PE header
NTDLL_BASE = 0x7FFE_2000_0000
UCRT_BASE = 0x7FFE_3000_0000
G_LUAUI = 0x2000_0000
G_RULES = 0x3000_0000

# fake DWARF: VMA range -> inline chain innermost first [(func, file, line)]
FUNCS = {
    "main":        (0x140001000, [("main", "rts/System/Main.cpp", 10)]),
    "update":      (0x140002000, [("CGame::Update()", "rts/Game/Game.cpp", 100)]),
    "sim":         (0x140003000, [("float3::dot(float3 const&) const", "rts/System/float3.h", 5),
                                  ("CSim::Update()", "rts/Sim/Sim.cpp", 50)]),
    "runcallin":   (0x140004000, [("CLuaHandle::RunCallInTraceback()", "rts/Lua/LuaHandle.cpp", 400)]),
    "pcall":       (0x140005000, [("lua_pcall", "rts/lib/lua/src/lapi.cpp", 817)]),
    "vm":          (0x140006000, [("luaV_execute", "rts/lib/lua/src/lvm.cpp", 380)]),
    "precall":     (0x140007000, [("luaD_precall", "rts/lib/lua/src/ldo.cpp", 265)]),
    "getunitpos":  (0x140008000, [("LuaSyncedRead::GetUnitPosition(lua_State*)", "rts/Lua/LuaSyncedRead.cpp", 900)]),
    "recurse":     (0x140009000, [("Recurse(int)", "rts/Misc/Recurse.cpp", 7)]),
    "concat":      (0x14000A000, [("luaV_concat", "rts/lib/lua/src/lvm.cpp", 280)]),
}
FUNC_SIZE = 0x1000


def rt(name, off=0x10):
    """runtime address inside a fake exe function"""
    return EXE_BASE + (FUNCS[name][0] - PREF_BASE) + off


def ret(name, off=0x20):
    """runtime return address inside a fake exe function (the analysis looks up addr - 1)"""
    return rt(name, off)


def fake_dwarf(vma):
    for _name, (start, chain) in FUNCS.items():
        if start <= vma < start + FUNC_SIZE:
            return chain
    return None


# --- writer (mirror of SamplingProfiler.cpp) --------------------------------------------------------

def record(rtype, payload):
    return struct.pack("<II", rtype, len(payload)) + payload


def header(interval_us=1000, max_frames=64, max_lua=4, flags=1):
    return struct.pack("<8sIIQQQIIIIQQIIIIQ", b"RCLSMPL\0", 1, 96, QPC_FREQ, 1000, 0, interval_us, max_frames,
                       max_lua, 512 * 1024, EXE_BASE, EXE_SIZE, 1234, 5678, flags, 8, 0)


def module(base, size, path):
    p = path.encode()
    return record(1, struct.pack("<QQI", base, size, len(p)) + p)


def sample(qpc, frames, flags=0, g=0, L=0, lua=(), susp=30):
    if g:
        flags |= sampler.SF_LUA_ACTIVE
    body = struct.pack("<QIIQQHHI", qpc, susp, flags, g, L, len(frames), len(lua), 0)
    body += struct.pack("<%dQ" % len(frames), *frames)
    for kind, line, value in lua:
        body += struct.pack("<BBHiQ", kind, 0, 0, line, value)
    return record(2, body)


def luastate(g, name, synced):
    n = name.encode()
    return record(3, struct.pack("<Q4sI", g, bytes([int(synced), 0, 0, 0]), len(n)) + n)


def luafunc(fid, source, linedefined, lastline=0):
    s = source.encode()
    shown = s[:128]
    return record(4, struct.pack("<IiiIQI", fid, linedefined, lastline, len(s), 0x5000 + fid, len(shown)) + shown)


def symbol(addr, symaddr, name):
    n = name.encode()
    return record(5, struct.pack("<QQI", addr, symaddr, len(n)) + n)


def trailer(samples, end_qpc, drops=0):
    vals = dict(endQpc=end_qpc, samples=samples, drops=drops, suspendFailures=0, contextFailures=0, missedTicks=1,
                suspendMinTicks=150, suspendMaxTicks=900, suspendSumTicks=300 * samples, workMaxTicks=500,
                workSumTicks=100 * samples, samplerCpu100ns=1_000_000, writerCpu100ns=100_000,
                luaReadFailures=0, luaFuncCount=3, luaFuncOverflow=0)
    return record(6, struct.pack("<16Q", *[vals[k] for k in sampler.TRAILER_FIELDS]))


def fake_pe(path, image_base=PREF_BASE, size_of_image=EXE_SIZE):
    """Minimal PE32+ header: only what sampler.pe_layout reads."""
    b = bytearray(1024)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x80)
    b[0x80:0x84] = b"PE\0\0"
    opt = 0x80 + 4 + 20
    struct.pack_into("<H", b, opt, 0x20B)
    struct.pack_into("<Q", b, opt + 24, image_base)
    struct.pack_into("<I", b, opt + 56, size_of_image)
    Path(path).write_bytes(bytes(b))


WAIT_ADDR = NTDLL_BASE + 0x9D014     # inside NtWaitForSingleObject
MEMCPY_ADDR = UCRT_BASE + 0x1234

# (count, frames leaf-first, kwargs)
SCENARIO = [
    (50, [rt("sim"), ret("update"), ret("main")], {}),
    (30, [WAIT_ADDR, ret("update"), ret("main")], dict(flags=sampler.SF_WAIT)),
    (20, [rt("vm"), ret("pcall"), ret("runcallin"), ret("update"), ret("main")],
     dict(g=G_LUAUI, L=G_LUAUI + 8, lua=((1, 12, 1), (1, 105, 2)))),
    (10, [rt("getunitpos"), ret("precall"), ret("vm"), ret("pcall"), ret("runcallin"), ret("update"), ret("main")],
     dict(g=G_LUAUI, L=G_LUAUI + 8, lua=((2, -1, rt("getunitpos", 0)), (1, 14, 1), (1, 106, 2)))),
    (5, [rt("recurse"), ret("recurse"), ret("recurse"), ret("main")], {}),
    (5, [rt("vm"), ret("pcall"), ret("runcallin"), ret("update"), ret("main")],
     dict(g=G_RULES, L=G_RULES + 8, lua=((1, 33, 3),))),
    (2, [MEMCPY_ADDR, ret("concat"), ret("vm"), ret("pcall"), ret("runcallin"), ret("update"), ret("main")],
     dict(g=G_LUAUI, L=G_LUAUI + 8, lua=((1, 107, 2),))),
]
TOTAL = sum(c for c, _f, _k in SCENARIO)


def write_capture(path, truncate=0):
    out = bytearray(header())
    out += module(EXE_BASE, EXE_SIZE, r"C:\Games\Recoil\engine\spring.exe")
    out += module(NTDLL_BASE, 0x200000, r"C:\Windows\SYSTEM32\ntdll.dll")
    out += module(UCRT_BASE, 0x100000, r"C:\Windows\System32\ucrtbase.dll")
    qpc = 2000
    for count, frames, kw in SCENARIO:
        for _ in range(count):
            out += sample(qpc, frames, **kw)
            qpc += QPC_FREQ // 1000
    # trailing metadata, as written at stop (modules repeated: reader must dedupe)
    out += module(EXE_BASE, EXE_SIZE, r"C:\Games\Recoil\engine\spring.exe")
    out += luastate(G_LUAUI, "LuaUI", False)
    out += luastate(G_RULES, "LuaRules", True)
    out += luafunc(1, "@LuaUI/Widgets/gui_a.lua", 10, 40)
    out += luafunc(2, "LuaUI/widgets.lua", 100, 200)
    out += luafunc(3, "LuaRules/Gadgets/unit_b.lua", 30, 60)
    out += symbol(WAIT_ADDR, NTDLL_BASE + 0x9D000, "NtWaitForSingleObject")
    out += symbol(MEMCPY_ADDR, UCRT_BASE + 0x1200, "memcpy")
    out += trailer(TOTAL, qpc)
    if truncate:
        out = out[:-truncate]
    Path(path).write_bytes(bytes(out))


class FakeSymbolizer:
    def __init__(self):
        self.calls = 0

    def resolve(self, vmas):
        self.calls += 1
        return {v: fake_dwarf(v) or [("??", "??", 0)] for v in vmas}


def analyse(path, inline=True, exclude_wait=False):
    cap = sampler.parse(path)
    d = Path(path).parent
    fake_pe(d / "spring.exe")
    res = sampler.Resolver(cap, {"spring.exe": str(d / "spring.exe")}, inline=inline,
                           symbolizers={"spring.exe": FakeSymbolizer()})
    return cap, sampler.Analysis(cap, res, exclude_wait=exclude_wait)


class SamplerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.file = self.dir / "sampling-test.bin"
        write_capture(self.file)

    def tearDown(self):
        self.tmp.cleanup()

    def test_parse(self):
        cap = sampler.parse(self.file)
        self.assertEqual(len(cap.samples), TOTAL)
        self.assertEqual(cap.interval_us, 1000)
        self.assertEqual(cap.qpc_freq, QPC_FREQ)
        self.assertEqual(len(cap.modules), 3, "duplicate module records must be merged")
        self.assertEqual(cap.exe.name, "spring.exe")
        self.assertEqual(cap.trailer["samples"], TOTAL)
        self.assertEqual(cap.state_name(G_LUAUI), "LuaUI")
        self.assertEqual(cap.state_name(G_RULES), "LuaRules synced")
        self.assertEqual(cap.lua_funcs[1].source, "@LuaUI/Widgets/gui_a.lua")
        self.assertEqual(cap.samples[0].frames[0], rt("sim"))
        self.assertEqual(cap.samples[80].lua, ((1, 12, 1), (1, 105, 2)))
        self.assertEqual(cap.samples[80].lua_g, G_LUAUI)
        self.assertEqual(cap.warnings, [])

    def test_vma_mapping(self):
        cap = sampler.parse(self.file)
        fake_pe(self.dir / "spring.exe")
        seen = []

        class Recorder(FakeSymbolizer):
            def resolve(self, vmas):
                seen.extend(vmas)
                return super().resolve(vmas)

        res = sampler.Resolver(cap, {"spring.exe": str(self.dir / "spring.exe")}, symbolizers={"spring.exe": Recorder()})
        res.prepare([rt("sim"), ret("update") - 1])
        self.assertIn(FUNCS["sim"][0] + 0x10, seen)
        self.assertIn(FUNCS["update"][0] + 0x20 - 1, seen)
        self.assertEqual(res.frames(rt("sim"))[0].name, "float3::dot(float3 const&) const")
        self.assertEqual(res.frames(rt("sim"))[1].name, "CSim::Update()")
        res.prepare([WAIT_ADDR, NTDLL_BASE + 0x10, 0x1234])
        self.assertEqual(res.frames(WAIT_ADDR)[0].name, "ntdll.dll!NtWaitForSingleObject")
        self.assertEqual(res.frames(NTDLL_BASE + 0x10)[0].name, "ntdll.dll+0x10")
        self.assertTrue(res.frames(0x1234)[0].name.startswith("[unknown"))

    def test_wrong_exe_warns(self):
        cap = sampler.parse(self.file)
        fake_pe(self.dir / "spring.exe", size_of_image=EXE_SIZE + 0x1000)
        sampler.Resolver(cap, {"spring.exe": str(self.dir / "spring.exe")}, symbolizers={"spring.exe": FakeSymbolizer()})
        self.assertTrue(any("wrong binary" in w for w in cap.warnings))

    def test_self_and_inclusive(self):
        _cap, an = analyse(self.file)
        self_c = an.self_counts()
        self.assertEqual(self_c["float3::dot(float3 const&) const"], 50)  # innermost inline frame
        self.assertEqual(self_c["ntdll.dll!NtWaitForSingleObject"], 30)
        self.assertEqual(self_c["luaV_execute"], 25)
        self.assertEqual(self_c["Recurse(int)"], 5)
        self.assertEqual(sum(self_c.values()), TOTAL)
        incl = an.inclusive_counts()
        self.assertEqual(incl["main"], TOTAL)
        self.assertEqual(incl["Recurse(int)"], 5, "recursion counted once per sample")
        self.assertEqual(incl["CSim::Update()"], 50)
        self.assertEqual(incl["luaV_execute"], 20 + 10 + 5 + 2)
        self.assertEqual(incl["CGame::Update()"], TOTAL - 5)

    def test_no_inline(self):
        _cap, an = analyse(self.file, inline=False)
        self.assertEqual(an.self_counts()["CSim::Update()"], 50)

    def test_exclude_wait(self):
        _cap, an = analyse(self.file, exclude_wait=True)
        self.assertEqual(len(an.samples), TOTAL - 30)

    def test_lua_attribution(self):
        cap, an = analyse(self.file)
        r = an.lua_report()
        # leaf in the Lua runtime (lvm.cpp), or in ucrtbase memcpy called from luaV_concat
        self.assertEqual(r["lua_self"]["LuaUI/Widgets/gui_a.lua:10"], 20)
        self.assertEqual(r["lua_self"]["LuaUI/widgets.lua:100"], 2)
        self.assertEqual(r["lua_self"]["LuaRules/Gadgets/unit_b.lua:30"], 5)
        self.assertEqual(sum(r["lua_self"].values()), 27, "engine API leaf (GetUnitPosition) is not Lua self")
        self.assertEqual(r["lua_incl"]["LuaUI/Widgets/gui_a.lua:10"], 30)
        self.assertEqual(r["lua_incl"]["LuaUI/widgets.lua:100"], 32)
        self.assertEqual(r["lua_incl"]["[C] LuaSyncedRead::GetUnitPosition(lua_State*)"], 10)
        self.assertEqual(r["c_from_lua"]["[C] LuaSyncedRead::GetUnitPosition(lua_State*)"], 10)
        self.assertEqual(r["state_incl"]["LuaUI"], 32)
        self.assertEqual(r["state_self"]["LuaUI"], 22)
        self.assertEqual(r["state_incl"]["LuaRules synced"], 5)
        self.assertEqual(an.lua_frame_name(1, 12, 1, with_line=True), "LuaUI/Widgets/gui_a.lua:10 @12")

    def test_folded(self):
        _cap, an = analyse(self.file)
        f = an.folded()
        self.assertEqual(f["main;CGame::Update();CSim::Update();float3::dot(float3 const&) const"], 50)
        self.assertEqual(f["main;Recurse(int);Recurse(int);Recurse(int)"], 5)
        self.assertEqual(sum(f.values()), TOTAL)
        fl = an.folded(with_lua=True)
        self.assertEqual(fl["main;CGame::Update();CLuaHandle::RunCallInTraceback();[Lua] LuaUI;"
                            "LuaUI/widgets.lua:100;LuaUI/Widgets/gui_a.lua:10"], 20)
        self.assertEqual(fl["main;CGame::Update();CLuaHandle::RunCallInTraceback();[Lua] LuaUI;"
                            "LuaUI/widgets.lua:100;LuaUI/Widgets/gui_a.lua:10;"
                            "[C] LuaSyncedRead::GetUnitPosition(lua_State*);"
                            "LuaSyncedRead::GetUnitPosition(lua_State*)"], 10)
        self.assertEqual(fl["main;CGame::Update();CLuaHandle::RunCallInTraceback();[Lua] LuaUI;"
                            "LuaUI/widgets.lua:100;ucrtbase.dll!memcpy"], 2)
        self.assertEqual(sum(fl.values()), TOTAL)

    def test_truncated_file(self):
        bad = self.dir / "cut.bin"
        write_capture(bad, truncate=7)
        cap = sampler.parse(bad)
        self.assertEqual(len(cap.samples), TOTAL)
        self.assertIsNone(cap.trailer)
        self.assertTrue(any("truncated" in w for w in cap.warnings))
        with self.assertRaises(ValueError):
            (self.dir / "junk.bin").write_bytes(b"x" * 200)
            sampler.parse(self.dir / "junk.bin")

    def test_cli_with_fake_addr2line(self):
        """End to end: main() -> subprocess addr2line (a fake speaking binutils' format)."""
        fake_pe(self.dir / "spring.exe")
        table = {"%x" % start: chain for start, chain in FUNCS.values()}
        (self.dir / "dwarf.json").write_text(json.dumps(table))
        tool = self.dir / "fake-addr2line"
        tool.write_text(f"""#!{sys.executable}
import json, sys
table = {{int(k, 16): v for k, v in json.load(open({str(self.dir / 'dwarf.json')!r})).items()}}
assert {{'-f', '-C', '-a', '-i', '-e'}} <= set(sys.argv[1:]), sys.argv
for line in sys.stdin:
    a = int(line, 16)
    print("0x%016x" % a)
    chain = next((c for s, c in table.items() if s <= a < s + {FUNC_SIZE}), [["??", "??", 0]])
    for fn, file, ln in chain:
        print(fn)
        print("%s:%s" % (file, ln) + (" (discriminator 3)" if ln == 380 else ""))
""")
        tool.chmod(0o755)
        folded = self.dir / "out.folded"
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = sampler.main([str(self.file), "--exe", str(self.dir / "spring.exe"), "--addr2line", str(tool),
                               "--lua", "--top", "5", "--folded", str(folded)])
        out = buf.getvalue()
        self.assertEqual(rc, 0)
        self.assertIn("samples   %d" % TOTAL, out)
        self.assertIn("waiting   %.1f%%" % (100.0 * 30 / TOTAL), out)
        self.assertIn("LuaUI/Widgets/gui_a.lua:10", out)
        self.assertIn("LuaRules synced", out)
        self.assertIn("float3::dot(float3 const&) const", out)
        self.assertIn("main thread suspended min/avg/max 15.0/30.0/90.0 us", out)
        lines = folded.read_text().splitlines()
        self.assertIn("main;CGame::Update();CSim::Update();float3::dot(float3 const&) const 50", lines)
        self.assertEqual(sum(int(x.rsplit(" ", 1)[1]) for x in lines), TOTAL)

    @unittest.skipUnless(shutil.which("x86_64-w64-mingw32-gcc") and shutil.which("x86_64-w64-mingw32-addr2line")
                         and shutil.which("x86_64-w64-mingw32-nm"), "mingw toolchain not installed")
    def test_real_addr2line(self):
        src = self.dir / "t.c"
        src.write_text("""
static inline __attribute__((always_inline)) int sq(int x) { return x * x + 1; }
__attribute__((noinline)) int leaf_fn(int x) { int s = 0; for (int i = 0; i < x; ++i) s += sq(i); return s; }
__attribute__((noinline)) int mid_fn(int x) { return leaf_fn(x) + 1; }
int main(int argc, char** argv) { (void)argv; return mid_fn(argc); }
""")
        exe = self.dir / "t.exe"
        subprocess.run(["x86_64-w64-mingw32-gcc", "-O2", "-g", "-o", str(exe), str(src)], check=True)
        syms = {}
        for line in subprocess.run(["x86_64-w64-mingw32-nm", str(exe)], capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                syms[parts[2]] = int(parts[0], 16)
        pref, size = sampler.pe_layout(exe)
        base = 0x7FF7_2000_0000
        # runtime addresses: leaf inside leaf_fn's loop body, return addresses just after calls
        leaf = base + syms["leaf_fn"] - pref + 8
        mid_ret = base + syms["mid_fn"] - pref + 6
        main_ret = base + syms["main"] - pref + 6
        f = self.dir / "real.bin"
        data = bytearray(struct.pack("<8sIIQQQIIIIQQIIIIQ", b"RCLSMPL\0", 1, 96, QPC_FREQ, 0, 0, 1000, 64, 4,
                                     512 * 1024, base, size, 1, 1, 1, 8, 0))
        data += module(base, size, r"C:\t\t.exe")
        data += sample(100, [leaf, mid_ret, main_ret])
        f.write_bytes(bytes(data))
        cap = sampler.parse(f)
        res = sampler.Resolver(cap, {"t.exe": str(exe)})
        an = sampler.Analysis(cap, res)
        names = [fr.name for fr in an.native_stack(cap.samples[0])]
        self.assertIn("leaf_fn", names)
        self.assertIn("mid_fn", names)
        self.assertIn("main", names)
        self.assertLess(names.index("main"), names.index("leaf_fn"))
        self.assertTrue(any(fr.file.endswith("t.c") for a in sampler.lookup_keys(cap.samples[0])
                            for fr in res.frames(a)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
