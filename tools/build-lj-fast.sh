#!/bin/sh
#
# Build a host-specific, semantics-safe LuaJIT executable with native tuning,
# GCC PGO/LTO and, when available, LLVM BOLT post-link optimization.
#
# Controls:
#   FAST_CC=gcc             GCC executable to use.
#   FAST_JOBS=<n>           Parallel build jobs.
#   FAST_BOLT=auto|0|1      Auto-detect, disable, or require BOLT.
#   FAST_BENCH_REPS=<n>     Production benchmark training repetitions.
#   FAST_OPS_REPS=<n>       Operation benchmark training repetitions.
#   FAST_EXTRA_CFLAGS=...   Additional compiler flags.
#   FAST_EXTRA_LDFLAGS=...  Additional linker flags.

set -eu

fail()
{
  echo "build-lj-fast: $*" >&2
  exit 1
}

have()
{
  command -v "$1" >/dev/null 2>&1
}

check_positive_integer()
{
  case $2 in
    ''|*[!0-9]*) fail "$1 must be a positive integer" ;;
    0) fail "$1 must be greater than zero" ;;
  esac
}

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
cd "$repo_dir"

[ "$(uname -s)" = Linux ] ||
  fail "the hyper-optimized build currently supports Linux only"
case $(uname -m) in
  x86_64|amd64) ;;
  *) fail "the hyper-optimized build currently supports x86-64 only" ;;
esac

fast_cc=${FAST_CC:-gcc}
fast_jobs=${FAST_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}
fast_bolt=${FAST_BOLT:-auto}
fast_bench_reps=${FAST_BENCH_REPS:-4}
fast_ops_reps=${FAST_OPS_REPS:-7}
fast_extra_cflags=${FAST_EXTRA_CFLAGS:-}
fast_extra_ldflags=${FAST_EXTRA_LDFLAGS:-}

check_positive_integer FAST_JOBS "$fast_jobs"
check_positive_integer FAST_BENCH_REPS "$fast_bench_reps"
check_positive_integer FAST_OPS_REPS "$fast_ops_reps"

have "$fast_cc" || fail "GCC executable not found: $fast_cc"
have make || fail "make is required"
have install || fail "install is required"
have strip || fail "strip is required"

if "$fast_cc" -dM -E -x c /dev/null |
    grep -q '^#define __clang__ '; then
  fail "FAST_CC must select GCC (Clang PGO uses an incompatible format)"
fi
"$fast_cc" -dM -E -x c /dev/null |
  grep -q '^#define __GNUC__ ' ||
  fail "FAST_CC must select GCC (Clang PGO uses an incompatible format)"
case $("$fast_cc" -dumpmachine) in
  x86_64*) ;;
  *) fail "$fast_cc is not an x86-64 compiler" ;;
esac

use_bolt=0
case $fast_bolt in
  auto)
    if have llvm-bolt && have merge-fdata; then
      use_bolt=1
    fi
    ;;
  0) ;;
  1)
    have llvm-bolt || fail "FAST_BOLT=1 but llvm-bolt was not found"
    have merge-fdata || fail "FAST_BOLT=1 but merge-fdata was not found"
    use_bolt=1
    ;;
  *) fail "FAST_BOLT must be auto, 0, or 1" ;;
esac

tmp_base=${TMPDIR:-/tmp}
work_dir=$(mktemp -d "$tmp_base/simdjit-lj-fast.XXXXXX")
build_ok=0

cleanup()
{
  if [ "$build_ok" -eq 1 ]; then
    case $work_dir in
      "$tmp_base"/simdjit-lj-fast.*) rm -rf -- "$work_dir" ;;
    esac
  else
    echo "build-lj-fast: retained failed build state in $work_dir" >&2
  fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

profile_dir=$work_dir/gcc-profile
mkdir -p "$profile_dir"

common_cflags="-O3 -march=native -mtune=native \
-fomit-frame-pointer -fno-pie -fno-plt -fno-stack-protector \
-fno-semantic-interposition -flto=auto -flto-partition=one \
-fno-fat-lto-objects -fipa-pta $fast_extra_cflags"
common_ldflags="-no-pie -flto=auto -flto-partition=one \
-Wl,-O2,--as-needed $fast_extra_ldflags"
generate_cflags="$common_cflags \
-fprofile-generate=$profile_dir -fprofile-update=atomic"
generate_ldflags="$common_ldflags -fprofile-generate=$profile_dir"

echo "==== Building instrumented native PGO executable ===="
make clean
make -j"$fast_jobs" BUILDMODE=static CC="$fast_cc" TARGET_STRIP=true \
  XCFLAGS="$generate_cflags" LDFLAGS="$generate_ldflags"

train_binary()
{
  training_binary=$1
  log_prefix=$2
  "$training_binary" test/simd/run.lua \
    > "$work_dir/$log_prefix-suite.log"
  "$training_binary" test/simd/bench.lua "$fast_bench_reps" \
    > "$work_dir/$log_prefix-bench.log"
  "$training_binary" test/simd/bench_ops.lua "$fast_ops_reps" \
    > "$work_dir/$log_prefix-ops.log"
  "$training_binary" test/simd/test_noregress.lua \
    > "$work_dir/$log_prefix-noregress.log"
}

echo "==== Training GCC PGO on SIMD tests and workloads ===="
train_binary "$repo_dir/src/luajit" gcc

use_cflags="$common_cflags -fprofile-use=$profile_dir \
-fprofile-correction -Wno-missing-profile"
use_ldflags="$common_ldflags -fprofile-use=$profile_dir \
-fprofile-correction"
if [ "$use_bolt" -eq 1 ]; then
  use_cflags="$use_cflags -fno-reorder-blocks-and-partition"
  use_ldflags="$use_ldflags -Wl,--emit-relocs"
fi

echo "==== Rebuilding with native PGO and LTO ===="
make clean
make -j"$fast_jobs" BUILDMODE=static CC="$fast_cc" TARGET_STRIP=true \
  XCFLAGS="$use_cflags" LDFLAGS="$use_ldflags"
cp "$repo_dir/src/luajit" "$work_dir/pgo-lto"

final_binary=$work_dir/pgo-lto
if [ "$use_bolt" -eq 1 ]; then
  bolt_profile=$work_dir/bolt-profile.fdata
  bolt_instrumented=$work_dir/bolt-instrumented

  echo "==== Instrumenting the PGO/LTO executable with LLVM BOLT ===="
  if ! llvm-bolt "$work_dir/pgo-lto" -o "$bolt_instrumented" \
      --instrument --instrumentation-file="$bolt_profile" \
      --instrumentation-file-append-pid \
      > "$work_dir/bolt-instrument.log" 2>&1; then
    tail -80 "$work_dir/bolt-instrument.log" >&2
    fail "LLVM BOLT instrumentation failed"
  fi

  echo "==== Training BOLT on SIMD tests and workloads ===="
  train_binary "$bolt_instrumented" bolt
  set -- "$bolt_profile".*
  [ -e "$1" ] || fail "BOLT training did not produce profile data"
  merge-fdata "$bolt_profile".* > "$work_dir/bolt-merged.fdata"

  echo "==== Applying BOLT code-layout optimization ===="
  if ! llvm-bolt "$work_dir/pgo-lto" -o "$work_dir/bolt-optimized" \
      -data="$work_dir/bolt-merged.fdata" \
      -reorder-blocks=ext-tsp -reorder-functions=cdsort \
      -split-functions -split-all-cold -split-strategy=cdsplit \
      -icf=safe -peepholes=all -jump-tables=move \
      > "$work_dir/bolt-optimize.log" 2>&1; then
    tail -80 "$work_dir/bolt-optimize.log" >&2
    fail "LLVM BOLT optimization failed"
  fi
  final_binary=$work_dir/bolt-optimized
fi

strip --strip-all "$final_binary" -o "$work_dir/lj-fast"

echo "==== Verifying optimized candidate ===="
"$work_dir/lj-fast" test/simd/run.lua \
  > "$work_dir/final-suite.log"
"$work_dir/lj-fast" test/simd/test_noregress.lua \
  > "$work_dir/final-noregress.log"
cmp "$work_dir/gcc-noregress.log" "$work_dir/final-noregress.log"
install -m 0755 "$work_dir/lj-fast" "$repo_dir/lj-fast"
"$repo_dir/lj-fast" -v
file "$repo_dir/lj-fast"
sha256sum "$repo_dir/lj-fast"

# Do not leave PGO/LTO objects masquerading as a normal incremental build.
make clean
build_ok=1
echo "==== Hyper-optimized executable ready: $repo_dir/lj-fast ===="
