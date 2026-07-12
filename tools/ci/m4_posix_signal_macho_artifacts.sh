#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
objdump=${LLVM_OBJDUMP:-llvm-objdump}
object=${1:-"$root/src/lj_thr.o"}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-signal-macho.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

disasm="$tmpdir/disasm"
body="$tmpdir/body"
relocs="$tmpdir/relocs"

extract_symbol()
{
  input=$1
  symbol=$2
  output=$3
  "$objdump" --disassemble-symbols="$symbol" --reloc "$input" >"$output"
}

extract_symbol "$object" _lj_thr_get_tg_signal "$body"
test -s "$body"
if grep -E 'TLV|tlv_get_addr|pthread_getspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|pthread_atfork|sched_yield' \
    "$body" >/dev/null; then
  echo "Mach-O signal getter contains a TLV/registration call" >&2
  exit 1
fi
test "$(grep -Ec '[[:space:]]callq?[[:space:]]+\*' "$body" || true)" -eq 2
extract_symbol "$object" _lj_thr_get_tg_profile_signal \
  "$tmpdir/profile-getter"
test "$(grep -Ec '[[:space:]]callq?[[:space:]]+\*' \
  "$tmpdir/profile-getter" || true)" -eq 2

for profile in "$root/src/lj_profile.o" "$root/src/lj_profile_dyn.o"; do
  test -f "$profile" || continue
  pbody="$tmpdir/$(basename "$profile").handler"
  extract_symbol "$profile" _luaJIT_profile_timer_test_signal_entry "$pbody"
  test -s "$pbody"
  if grep -E \
      'TLV|tlv_get_addr|pthread_getspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|sched_yield|pthread_atfork|_lj_thr_get_tg([^_]|$)|profile_trigger|lj_dispatch_update|profile_tg_sethook' \
      "$pbody" >/dev/null; then
    echo "Mach-O SIGPROF entry reaches non-publication work: $profile" >&2
    exit 1
  fi
  grep -E 'X86_64_RELOC_BRANCH[[:space:]]+___error$' "$pbody" >/dev/null
  grep -E 'X86_64_RELOC_BRANCH[[:space:]]+_lj_thr_get_tg_profile_signal$' \
    "$pbody" >/dev/null
  test "$(grep -Ec 'X86_64_RELOC_BRANCH' "$pbody" || true)" -eq 2
done

final="$root/src/libluajit.so"
final_handler="$tmpdir/final-handler"
final_getter="$tmpdir/final-getter"
extract_symbol "$final" _luaJIT_profile_timer_test_signal_entry \
  "$final_handler"
test -s "$final_handler"
if grep -E \
    'TLV|tlv_get_addr|pthread_getspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|sched_yield|pthread_atfork|lj_dispatch_update|profile_tg_sethook' \
    "$final_handler" >/dev/null; then
  echo "final Mach-O SIGPROF entry reaches unsafe work" >&2
  exit 1
fi
# The object relocation gate above identifies these calls as ___error and the
# profile getter. `strip -x` removes the hidden getter's name, and linked Mach-O
# symbol stubs are labelled inconsistently across LLVM versions, but the
# non-LTO final link preserves exactly those two call edges.
test "$(grep -Ec '[[:space:]]callq?[[:space:]]' "$final_handler" || true)" -eq 2
extract_symbol "$final" _luaJIT_thr_tg_signal_test_get "$final_getter"
test "$(grep -Ec '[[:space:]]callq?[[:space:]]+\*' "$final_getter" || true)" -eq 2
if grep -E 'TLV|tlv_get_addr|pthread_getspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|pthread_atfork|sched_yield' \
    "$final_getter" >/dev/null; then
  echo "final Mach-O signal getter contains unsafe work" >&2
  exit 1
fi
extract_symbol "$final" _luaJIT_thr_tg_profile_signal_test_get \
  "$tmpdir/final-profile-getter"
test "$(grep -Ec '[[:space:]]callq?[[:space:]]+\*' \
  "$tmpdir/final-profile-getter" || true)" -eq 2

"$objdump" --macho --reloc "$object" >"$relocs"
unsigned='UNSIGND|UNSIGNED|X86_64_RELOC_UNSIGNED'
grep -E "($unsigned)[[:space:]].*_getpid$" "$relocs" >/dev/null
grep -E "($unsigned)[[:space:]].*_pthread_self$" "$relocs" >/dev/null

grep -F '|.define TGPOLL, qword [DISPATCH+DISPATCH_TG(poll)]' \
  "$root/src/vm_x64.dasc" >/dev/null
grep -F 'lj_profile_owner_poll(L);' "$root/src/lj_safepoint.c" >/dev/null
grep -F 'lj_tg_profile_request_acq(tg) == 0)' \
  "$root/src/lj_safepoint.c" >/dev/null
grep -F '|| lj_profile_poll_required(g)' "$root/src/lj_opt_loop.c" >/dev/null
grep -F 'if (rec_needs_xpoll(J))' "$root/src/lj_record.c" >/dev/null
grep -F '|| lj_profile_poll_required(g)' "$root/src/lj_record.c" >/dev/null
grep -F 'ctx->status = lj_trace_flushall_hs_noevent(L);' \
  "$root/src/lj_profile.c" >/dev/null
grep -F 'int lj_trace_flushall_hs_noevent(lua_State *L)' \
  "$root/src/lj_trace.c" >/dev/null
grep -F 'XOg_CMP|REX_64' "$root/src/lj_asm_x86.h" >/dev/null

echo "m4_posix_signal_macho_artifacts OK: final handler is publication-only"
