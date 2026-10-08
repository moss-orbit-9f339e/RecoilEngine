#!/bin/bash
# Timing: bench.sh <runs> <outdirA> <outdirB> [workloads...]
# Thread CPU time (Linux) or thread cycles / 3.2 GHz (Windows build) of each workload, A and B
# interleaved; prints median and minimum of each and the A/B ratios.
H=$(cd "$(dirname "$0")" && pwd)
cd $H
RUNS=$1; A=$(cd "$2" && pwd); B=$(cd "$3" && pwd); shift 3
W=${*:-"fields globals arrays calls pairs concat unitid iterablemap lups callins"}
median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
minv() { sort -n | head -1; }
t() { $1/luadrv -q -t $H/lua/bench/b_$2.lua $H/lua/zk 2>&1 >/dev/null | tr -d '\r' | awk '/TIME/{print $2}'; }
printf "%-12s %8s %8s %8s %8s %8s %8s\n" workload A_med B_med A_min B_min med_x min_x
for w in $W; do
  ta=(); tb=()
  for r in $(seq $RUNS); do ta+=($(t $A $w)); tb+=($(t $B $w)); done
  ma=$(printf "%s\n" "${ta[@]}" | median); mb=$(printf "%s\n" "${tb[@]}" | median)
  na=$(printf "%s\n" "${ta[@]}" | minv); nb=$(printf "%s\n" "${tb[@]}" | minv)
  printf "%-12s %8.4f %8.4f %8.4f %8.4f %7.3fx %7.3fx\n" $w $ma $mb $na $nb $(echo "$ma / $mb" | bc -l) $(echo "$na / $nb" | bc -l)
done
