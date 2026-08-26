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

case $(lipo -archs "$object" 2>/dev/null || true) in
  x86_64)
    arch=x86_64
    branch_reloc=X86_64_RELOC_BRANCH
    call_re='[[:space:]]callq?[[:space:]]+'
    indirect_call_re='[[:space:]]callq?[[:space:]]+\*'
    cell_reloc_count=1
    ;;
  arm64)
    arch=arm64
    branch_reloc=ARM64_RELOC_BRANCH26
    call_re='[[:space:]]blr?[[:space:]]+'
    indirect_call_re='[[:space:]]blr[[:space:]]+x[0-9]+'
    cell_reloc_count=2
    ;;
  *)
    echo "Mach-O signal artifact requires a thin x86_64 or arm64 object" >&2
    exit 1
    ;;
esac

forbidden='TLV|tlv_get_addr|__tls_get_addr|pthread_getspecific|pthread_setspecific|malloc|calloc|free|mmap|madvise|munmap|dlopen|dladdr|pthread_atfork|sched_yield|__atomic|__sync|libatomic|__aarch64_|stack_chk|chkstk|__asan|__ubsan|__tsan|__sanitizer'

extract_symbol()
{
  input=$1
  symbol=$2
  output=$3
  "$objdump" --disassemble-symbols="$symbol" --reloc "$input" >"$output"
}

check_signal_getter()
{
  input=$1
  symbol=$2
  output=$3
  require_cells=${4:-0}
  extract_symbol "$input" "$symbol" "$output"
  test -s "$output"
  if grep -E "$forbidden" "$output" >/dev/null; then
    echo "Mach-O signal getter contains TLS, allocation or runtime-helper work: $symbol" >&2
    exit 1
  fi
  # Both process and owner identities are called through eagerly initialized
  # function-pointer cells. No direct or compiler-generated helper edge is
  # permitted in the complete getter body.
  test "$(grep -Ec "$call_re" "$output" || true)" -eq 2
  test "$(grep -Ec "$indirect_call_re" "$output" || true)" -eq 2
  test "$(grep -Ec "$branch_reloc" "$output" || true)" -eq 0
  if test "$require_cells" = 1; then
    test "$(grep -Fc '_lj_thr_signal_getpid_fn' "$output" || true)" \
      -eq "$cell_reloc_count"
    test "$(grep -Fc '_lj_thr_signal_pthread_self_fn' "$output" || true)" \
      -eq "$cell_reloc_count"
  fi
}

check_signal_getter "$object" _lj_thr_get_tg_signal "$body" 1
check_signal_getter "$object" _lj_thr_get_tg_profile_signal \
  "$tmpdir/profile-getter" 1

profile_count=0
for profile in "$root/src/lj_profile.o" "$root/src/lj_profile_dyn.o"; do
  test -f "$profile" || continue
  profile_count=$((profile_count + 1))
  if test "$(lipo -archs "$profile" 2>/dev/null || true)" != "$arch"; then
    echo "Mach-O profiler object architecture does not match signal getter" >&2
    exit 1
  fi
  pbody="$tmpdir/$(basename "$profile").handler"
  extract_symbol "$profile" _luaJIT_profile_timer_test_signal_entry "$pbody"
  test -s "$pbody"
  if grep -E \
      "$forbidden|_lj_thr_get_tg([^_]|$)|profile_trigger|lj_dispatch_update|profile_tg_sethook" \
      "$pbody" >/dev/null; then
    echo "Mach-O SIGPROF entry reaches non-publication work: $profile" >&2
    exit 1
  fi
  grep -E "$branch_reloc[[:space:]]+___error$" "$pbody" >/dev/null
  grep -E "$branch_reloc[[:space:]]+_lj_thr_get_tg_profile_signal$" \
    "$pbody" >/dev/null
  test "$(grep -Ec "$branch_reloc" "$pbody" || true)" -eq 2
  test "$(grep -Ec "$call_re" "$pbody" || true)" -eq 2
done
test "$profile_count" -ge 1

final="$root/src/libluajit.so"
if test "$(lipo -archs "$final" 2>/dev/null || true)" != "$arch"; then
  echo "final Mach-O profiler image architecture does not match objects" >&2
  exit 1
fi
final_handler="$tmpdir/final-handler"
final_getter="$tmpdir/final-getter"
extract_symbol "$final" _luaJIT_profile_timer_test_signal_entry \
  "$final_handler"
test -s "$final_handler"
if grep -E \
    "$forbidden|lj_dispatch_update|profile_tg_sethook" \
    "$final_handler" >/dev/null; then
  echo "final Mach-O SIGPROF entry reaches unsafe work" >&2
  exit 1
fi
# The object relocation gate above identifies these calls as ___error and the
# profile getter. `strip -x` removes the hidden getter's name, and linked Mach-O
# symbol stubs are labelled inconsistently across LLVM versions, but the
# non-LTO final link preserves exactly those two call edges.
test "$(grep -Ec "$call_re" "$final_handler" || true)" -eq 2
check_signal_getter "$final" _luaJIT_thr_tg_signal_test_get "$final_getter"
check_signal_getter "$final" _luaJIT_thr_tg_profile_signal_test_get \
  "$tmpdir/final-profile-getter"

"$objdump" --macho --reloc "$object" >"$relocs"
unsigned='UNSIGND|UNSIGNED|X86_64_RELOC_UNSIGNED|ARM64_RELOC_UNSIGNED'
test "$(grep -Ec "($unsigned)[[:space:]].*_getpid$" "$relocs" || true)" -eq 1
test "$(grep -Ec "($unsigned)[[:space:]].*_pthread_self$" "$relocs" || true)" -eq 1

for artifact in "$object" "$root/src/lj_profile.o" \
                "$root/src/lj_profile_dyn.o" "$final"; do
  test -f "$artifact" || continue
  if nm -u "$artifact" | grep -E \
      '(__atomic|__sync|libatomic|__aarch64_|stack_chk|chkstk|__asan|__ubsan|__tsan|__sanitizer)' \
      >/dev/null; then
    echo "Mach-O signal artifact imports an atomic/runtime helper: $artifact" >&2
    exit 1
  fi
done

grep -F 'lj_profile_owner_poll(L);' "$root/src/lj_safepoint.c" >/dev/null
grep -F 'lj_tg_profile_request_acq(tg) == 0)' \
  "$root/src/lj_safepoint.c" >/dev/null
if test "$arch" = x86_64; then
  grep -F '|.define TGPOLL, qword [DISPATCH+DISPATCH_TG(poll)]' \
    "$root/src/vm_x64.dasc" >/dev/null
  grep -F '|| lj_profile_poll_required(g)' "$root/src/lj_opt_loop.c" >/dev/null
  test "$(grep -Fc \
    'emitir_raw(IRTG(IR_XPOLL, IRT_NIL), rec_needs_xpoll(J), 0);' \
    "$root/src/lj_record.c" || true)" -eq 2
  grep -F '|| lj_profile_poll_required(g)' "$root/src/lj_record.c" >/dev/null
  grep -F 'ctx->status = lj_trace_flushall_hs_noevent(L);' \
    "$root/src/lj_profile.c" >/dev/null
  grep -F 'int lj_trace_flushall_hs_noevent(lua_State *L)' \
    "$root/src/lj_trace.c" >/dev/null
  grep -F 'XOg_CMP|REX_64' "$root/src/lj_asm_x86.h" >/dev/null
  echo "m4_posix_signal_macho_artifacts OK: x86_64 handler is publication-only"
else
  grep -F '.macro arm64_vm_poll_acq, dst, tmp' \
    "$root/src/vm_arm64.dasc" >/dev/null
  grep -F 'add ATMP, DISPATCH, #DISPATCH_TG(profile_request)' \
    "$root/src/vm_arm64.dasc" >/dev/null
  echo "m4_posix_signal_macho_artifacts OK: arm64 handler/getters are publication-only; interpreter poll only, no JIT/XPOLL claim"
fi
