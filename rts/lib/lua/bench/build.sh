#!/bin/bash
# Standalone build of the Lua VM + benchmark/equality driver (not part of the engine build).
#
#   build.sh <lua-root> <outdir> [cxx] [extra flags...]
#
# <lua-root> is a directory containing src/ and include/ (e.g. rts/lib/lua, or an export of
# another revision: git archive <rev> rts/lib/lua | tar -x -C /tmp/old). Flags follow the
# engine's release build (-O3, x86-64 baseline, SSE math). The per-source flag the Lua
# CMakeLists.txt sets for lvm.cpp (-fno-crossjumping, GCC only) is mirrored when present.
# With a mingw cxx (x86_64-w64-mingw32-g++) a static Windows luadrv.exe is produced.
set -e
H=$(cd "$(dirname "$0")" && pwd)
LUA=$(cd "$1" && pwd); OUT=$2; CXX=${3:-g++}; shift 3 || shift $#
mkdir -p $OUT/obj
FLAGS="-O3 -g -std=c++23 -march=x86-64 -mtune=generic -msse2 -mfpmath=sse -mieee-fp -frounding-math -fno-strict-aliasing -pipe -DNDEBUG $*"
case $CXX in *mingw*) EXE=.exe ;; *) EXE=; FLAGS="$FLAGS -DLUA_USE_MKSTEMP" ;; esac
INC="-I$H/stub -I$LUA/include -I$LUA/src"
pids=(); objs=()
for f in $LUA/src/*.cpp $H/stub/LuaUserStub.cpp; do
  b=$(basename $f .cpp)
  [ "$b" = print ] && continue
  objs+=($OUT/obj/$b.o)
  EXTRA=""
  if [ "$b" = lvm ] && grep -q 'src/lvm.cpp PROPERTIES COMPILE_OPTIONS "-fno-crossjumping"' $LUA/CMakeLists.txt 2>/dev/null; then
    case $CXX in *clang*) ;; *) EXTRA="-fno-crossjumping" ;; esac
  fi
  $CXX $FLAGS $EXTRA $INC -c $f -o $OUT/obj/$b.o &
  pids+=($!)
done
for p in ${pids[@]}; do wait $p; done
case $CXX in
  *mingw*) $CXX $FLAGS $INC $H/driver/driver.cpp ${objs[@]} -static -static-libgcc -static-libstdc++ -o $OUT/luadrv$EXE
           cp $OUT/luadrv$EXE $OUT/luadrv ;;  # run through WSL interop like the native one
  *) $CXX $FLAGS $INC $H/driver/driver.cpp ${objs[@]} -o $OUT/luadrv ;;
esac
echo "built $OUT/luadrv$EXE"
