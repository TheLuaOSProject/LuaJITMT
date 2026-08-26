#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
objdump=${OBJDUMP:-objdump}
readelf=${READELF:-readelf}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-signal-artifacts.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

extract_symbol()
{
  object=$1
  symbol=$2
  case "$object" in
  *.so)
    "$objdump" -d --disassemble="$symbol" "$object"
    return
    ;;
  esac
  "$objdump" -dr "$object" | awk -v symbol="$symbol" '
    $0 ~ "<" symbol "(@@[^>]*)?>:" { copying = 1 }
    copying && $0 ~ /^[[:xdigit:]]+[[:space:]]+<[^>]+>:/ &&
      $0 !~ "<" symbol "(@@[^>]*)?>:" { exit }
    copying { print }
    copying && NF == 0 { exit }
  '
}

check_getter()
{
  object=$1
  symbol=$2
  body=$3
  extract_symbol "$object" "$symbol" >"$body"
  test -s "$body"
  if grep -E \
      '__tls_get_addr|tlv_get_addr|pthread_getspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|pthread_atfork|sched_yield' \
      "$body" >/dev/null; then
    echo "signal getter contains a resolver/registration call: $object" >&2
    exit 1
  fi
  calls=$(grep -Ec '[[:space:]]call[q]?[[:space:]]+\*' "$body" || true)
  test "$calls" -eq 2
}

check_handler_object()
{
  object=$1
  body=$2
  extract_symbol "$object" luaJIT_profile_timer_test_signal_entry >"$body"
  test -s "$body"
  if grep -E \
      '__tls_get_addr|tlv_get_addr|pthread_getspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|sched_yield|pthread_atfork|lj_thr_get_tg([^_]|$)|profile_trigger|lj_dispatch_update|profile_tg_sethook' \
      "$body" >/dev/null; then
    echo "SIGPROF entry reaches non-publication work: $object" >&2
    exit 1
  fi
  grep -E 'R_X86_64_PLT32[[:space:]]+__errno_location' "$body" >/dev/null
  grep -E 'R_X86_64_PLT32[[:space:]]+lj_thr_get_tg_profile_signal' \
    "$body" >/dev/null
  test "$(grep -Ec 'R_X86_64_PLT32' "$body" || true)" -eq 2
}

for object in "$root/src/lj_thr.o" "$root/src/lj_thr_dyn.o"; do
  body="$tmpdir/$(basename "$object").signal"
  check_getter "$object" lj_thr_get_tg_signal "$body"
  check_getter "$object" lj_thr_get_tg_profile_signal \
    "$tmpdir/$(basename "$object").profile-signal"
done

check_handler_object "$root/src/lj_profile.o" "$tmpdir/profile.signal"
check_handler_object "$root/src/lj_profile_dyn.o" "$tmpdir/profile-dyn.signal"

final_handler="$tmpdir/final-handler.signal"
final_getter="$tmpdir/final-getter.signal"
extract_symbol "$root/src/libluajit.so" \
  luaJIT_profile_timer_test_signal_entry >"$final_handler"
test -s "$final_handler"
if grep -E \
    '__tls_get_addr|pthread_getspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|sched_yield|pthread_atfork|lj_dispatch_update|profile_tg_sethook' \
    "$final_handler" >/dev/null; then
  echo "final DSO SIGPROF entry reaches unsafe work" >&2
  exit 1
fi
grep -E 'call[q]?[[:space:]].*__errno_location@plt' "$final_handler" >/dev/null
grep -E 'call[q]?[[:space:]].*luaJIT_thr_tg_profile_signal_test_get' \
  "$final_handler" >/dev/null
test "$(grep -Ec '[[:space:]]call[q]?[[:space:]]' "$final_handler" || true)" -eq 2
check_getter "$root/src/libluajit.so" luaJIT_thr_tg_signal_test_get \
  "$final_getter"
check_getter "$root/src/libluajit.so" luaJIT_thr_tg_profile_signal_test_get \
  "$tmpdir/final-profile-getter.signal"

relocs="$tmpdir/relocs"
"$readelf" -rW "$root/src/lj_thr.o" >"$relocs"
grep -E 'R_X86_64_64[[:space:]].*getpid' "$relocs" >/dev/null
grep -E 'R_X86_64_64[[:space:]].*pthread_self' "$relocs" >/dev/null

hot="$tmpdir/hot"
extract_symbol "$root/src/lj_thr.o" lj_thr_get_tg >"$hot"
test "$(grep -c '%fs:' "$hot" || true)" -eq 1
if grep -E 'pthread_|malloc|sched_yield' "$hot" >/dev/null; then
  echo "ordinary TG getter hot path changed" >&2
  exit 1
fi

grep -F '|.define TGPOLL, qword [DISPATCH+DISPATCH_TG(poll)]' \
  "$root/src/vm_x64.dasc" >/dev/null
grep -F 'lj_profile_owner_poll(L);' "$root/src/lj_safepoint.c" >/dev/null
grep -F 'lj_safepoint_owner_poll_pending(L)' \
  "$root/src/lj_safepoint.c" >/dev/null
grep -F 'lj_tg_profile_request_acq(tg) != 0' \
  "$root/src/lj_safepoint.h" >/dev/null
grep -F '|| lj_profile_poll_required(g)' "$root/src/lj_opt_loop.c" >/dev/null
grep -F 'if (rec_needs_xpoll(J))' "$root/src/lj_record.c" >/dev/null
grep -F '|| lj_profile_poll_required(g)' "$root/src/lj_record.c" >/dev/null
grep -F 'ctx->status = lj_trace_flushall_hs_noevent(L);' \
  "$root/src/lj_profile.c" >/dev/null
grep -F 'int lj_trace_flushall_hs_noevent(lua_State *L)' \
  "$root/src/lj_trace.c" >/dev/null
grep -F 'XOg_CMP|REX_64' "$root/src/lj_asm_x86.h" >/dev/null

# A fully static main has no dynamic link map for dladdr/dlopen. The Linux
# PT_LOAD proof must recognize it before either loader operation is needed.
static_fixture="$tmpdir/static-main"
"${CC:-cc}" -std=gnu11 -O2 -mcx16 -static \
  -DLJ_THR_SIGNAL_TEST_HELPERS -DLJ_PROFILE_TIMER_TEST_HELPERS \
  -I"$root/src" "$root/tests/t-posix-signal-safety.c" \
  "$root/src/libluajit.a" -lm -ldl -pthread -o "$static_fixture"
"${TIMEOUT:-timeout}" 30s "$static_fixture" >/dev/null

echo "m4_posix_signal_artifacts OK: final handler is atomic-publication-only"
