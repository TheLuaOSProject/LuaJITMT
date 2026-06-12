#!/bin/sh
# run.sh — benchmark driver for LuaJIT-MT (13_testing_and_benchmarks.md).
# Usage:
#   ./run.sh baseline <luajit-binary>          # record single-thread CSVs
#   ./run.sh compare  <old-binary> <new-binary># geomean regression check
#   ./run.sh scaling  <luajit-mt-binary>       # 1/2/4/8-thread curves (M4+)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
mode=${1:?mode}; shift

geomean_gate() {
  # stdin: lines "name old_ns new_ns"; gate: geomean(new/old) <= 1.10
  awk '{ r=$3/$2; s+=log(r); n++; printf "  %-18s %8.2f -> %8.2f  (%.3fx)\n",$1,$2,$3,r }
       END { g=exp(s/n); printf "geomean: %.4fx  ", g;
             if (g<=1.10) print "PASS (<=1.10)"; else { print "FAIL (>1.10)"; exit 1 } }'
}

bench_ns() { # $1=binary $2=jitflag -> "name ns" lines
  "$1" $2 "$HERE/bench.lua" | awk 'NR>1 { print $1, $3 }'
}

case "$mode" in
baseline)
  bin=${1:?binary}
  bench_ns "$bin" ""      > "$HERE/baseline_jit_$(hostname).csv"
  bench_ns "$bin" "-joff" > "$HERE/baseline_interp_$(hostname).csv"
  echo "wrote baseline CSVs for $(hostname)"
  ;;
compare)
  old=${1:?old}; new=${2:?new}
  bench_ns "$old" "" > /tmp/_old.$$; bench_ns "$new" "" > /tmp/_new.$$
  join /tmp/_old.$$ /tmp/_new.$$ | geomean_gate
  rm -f /tmp/_old.$$ /tmp/_new.$$
  ;;
scaling)
  bin=${1:?binary}
  for n in 1 2 4 8; do
    echo "== $n threads =="
    "$bin" "$HERE/bench_mt.lua" "$n"
  done
  ;;
*) echo "unknown mode $mode" >&2; exit 2 ;;
esac
