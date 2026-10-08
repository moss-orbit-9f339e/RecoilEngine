#!/bin/bash
# Instruction counts (valgrind cachegrind, no cache simulation; deterministic):
#   irbench.sh <outdirA> <outdirB> [workloads...]   (native Linux builds only)
H=$(cd "$(dirname "$0")" && pwd)
A=$(cd "$1" && pwd); B=$(cd "$2" && pwd); shift 2
W=${*:-"fields globals arrays calls pairs concat unitid iterablemap lups callins"}
T=$(mktemp -d)
for w in $W; do for x in A B; do
  bin=$A; [ $x = B ] && bin=$B
  ( valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null $bin/luadrv -q $H/lua/bench/b_$w.lua $H/lua/zk 2>&1 >/dev/null | awk '/I *refs/{gsub(",","",$4); print $4}' > $T/$w.$x ) &
done; done
wait
printf "%-12s %14s %14s %8s\n" workload Ir_A Ir_B ratio
for w in $W; do a=$(cat $T/$w.A); b=$(cat $T/$w.B); printf "%-12s %14s %14s %7.3fx\n" $w $a $b $(echo "$a / $b" | bc -l); done
rm -rf $T
