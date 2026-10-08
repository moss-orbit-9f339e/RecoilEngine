#!/bin/bash
# Equality check between two builds of build.sh (e.g. old and new VM):
#
#   [DRVOPT="-g -k 97"] [FUZZLEN=400] compare.sh <outdirA> <outdirB> [nseeds=200]
#
# Runs every benchmark (full output), every lua/test/t_*.lua and nseeds random programs of
# t_fuzz.lua (seed-dependent GC and hook modes) with both drivers and diffs the complete
# outputs, which include the allocator trace hash (identical sequence of every
# allocation/free => identical GC behaviour) and, through the fixed-address allocator,
# reproducible pointer-keyed traversal orders. DRVOPT is passed to the driver
# (-g: aggressive incremental GC, -k N: count hook every N instructions).
H=$(cd "$(dirname "$0")" && pwd)
cd $H  # Windows (mingw) drivers resolve absolute /... paths against the cwd's drive
A=$(cd "$1" && pwd); B=$(cd "$2" && pwd); NSEEDS=${3:-200}; DRVOPT=${DRVOPT:-}
O=$(mktemp -d); mkdir -p $O/A $O/B
jobs_list=()
add() { jobs_list+=("$*"); }
for t in $H/lua/bench/b_*.lua; do b=$(basename $t .lua); add "bench_$b $t $H/lua/zk"; done
for t in $H/lua/test/t_*.lua; do b=$(basename $t .lua); [ $b = t_fuzz ] && continue; add "$b $t $H/lua/test"; done
for seed in $(seq 1 $NSEEDS); do
  gc=$((seed % 3)); hook=0
  [ $((seed % 10)) = 7 ] && hook=1; [ $((seed % 25)) = 3 ] && hook=2; [ $((seed % 25)) = 13 ] && hook=3
  add "fuzz_${seed} $H/lua/test/t_fuzz.lua $H/lua $seed ${FUZZLEN:-400} $gc $hook"
done
run() { local name=$1; shift; timeout 300 $A/luadrv $DRVOPT "$@" > $O/A/$name.txt 2>&1; echo "exit=$?" >> $O/A/$name.txt; timeout 300 $B/luadrv $DRVOPT "$@" > $O/B/$name.txt 2>&1; echo "exit=$?" >> $O/B/$name.txt; }
export -f run; export A B O DRVOPT
printf "%s\n" "${jobs_list[@]}" | xargs -P ${JOBS:-16} -I{} bash -c 'run {}'
nd=0; n=0
for f in $O/A/*.txt; do n=$((n+1)); b=$(basename $f); if ! cmp -s $f $O/B/$b; then nd=$((nd+1)); echo "DIFF: $b"; diff $f $O/B/$b | head -5; fi; done
lines=$(cat $O/A/*.txt | wc -l)
echo "compared $n runs ($lines output lines): $nd differ"
[ -n "$KEEP" ] && echo "outputs kept in $O" || rm -rf $O
[ $nd = 0 ]
