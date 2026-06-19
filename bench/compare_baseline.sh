#!/bin/sh
# Compare two bench/run_baseline.sh CSVs with the M6/M9 JIT geomean gate.
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <baseline.csv> <current.csv>" >&2
  exit 2
fi

BASE=$1
CUR=$2
COLUMN=${BENCH_COLUMN:-jit_ns_per_op}
MAX=${BENCH_GEOMEAN_MAX:-1.10}

awk -v column="$COLUMN" -v max="$MAX" '
BEGIN {
  FS = ",";
  printf "benchmark,baseline,current,ratio\n";
}
FNR == NR {
  if (FNR == 1) {
    for (i = 1; i <= NF; i++) if ($i == column) base_col = i;
    if (!base_col) {
      printf "missing column in baseline: %s\n", column > "/dev/stderr";
      exit 2;
    }
    next;
  }
  if (NF < base_col || $1 == "") next;
  base[$1] = $base_col + 0;
  order[++norder] = $1;
  next;
}
FNR == 1 {
  for (i = 1; i <= NF; i++) if ($i == column) cur_col = i;
  if (!cur_col) {
    printf "missing column in current: %s\n", column > "/dev/stderr";
    exit 2;
  }
  next;
}
NF >= cur_col && $1 != "" {
  name = $1;
  if (!(name in base)) {
    printf "current has no pinned baseline for %s\n", name > "/dev/stderr";
    bad = 1;
    next;
  }
  cur[name] = $cur_col + 0;
  if (base[name] <= 0 || cur[name] <= 0) {
    printf "non-positive benchmark value for %s\n", name > "/dev/stderr";
    bad = 1;
    next;
  }
  ratio[name] = cur[name] / base[name];
}
END {
  for (i = 1; i <= norder; i++) {
    name = order[i];
    if (!(name in cur)) {
      printf "current is missing benchmark %s\n", name > "/dev/stderr";
      bad = 1;
      continue;
    }
    printf "%s,%.6g,%.6g,%.6f\n", name, base[name], cur[name], ratio[name];
    sum += log(ratio[name]);
    n++;
  }
  if (bad || n == 0) exit 1;
  geo = exp(sum / n);
  printf "geomean,%.6g,%.6g,%.6f\n", 1, geo, geo;
  if (geo > max + 0) {
    printf "FAIL: geomean %.6f > %.6f\n", geo, max + 0 > "/dev/stderr";
    exit 1;
  }
  printf "PASS: geomean %.6f <= %.6f\n", geo, max + 0;
}
' "$BASE" "$CUR"
