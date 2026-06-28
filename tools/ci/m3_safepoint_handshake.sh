#!/bin/sh
# Run the Lua-defined M3 safepoint handshake guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
for required in \
  'static int load_native_feof(lua_State *L, FILE *fp, uint32_t *actionsp)' \
  'static int load_native_ferror(lua_State *L, FILE *fp, uint32_t *actionsp)' \
  'load_native_feof(L, ctx->fp, &actions)' \
  'load_native_ferror(L, ctx.fp, &actions)'
do
  if ! grep -qF "$required" "$ROOT/src/lj_load.c"; then
    printf '%s\n' "loadfile native-state wrapper is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static int load_native_(feof|ferror)\(/ {
    in_wrapper = 1
  }
  !in_wrapper && /(^|[^[:alnum:]_])(feof|ferror)[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
  in_wrapper && /^}/ {
    in_wrapper = 0
  }
' "$ROOT/src/lj_load.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'loadfile FILE state probes must go through native-state wrappers' >&2
  exit 1
fi
for required in \
  'static void load_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'int had_stopreq;' \
  'load_fresh_stopreq(L, actions, ctx->had_stopreq)' \
  'load_checkstop_fresh(L, ctx.actions, ctx.had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lj_load.c"; then
    printf '%s\n' "loadfile fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void load_checkstop_fresh\(lua_State \*L, uint32_t actions,/ {
    in_fresh = 1
  }
  in_fresh && /^}/ { in_fresh = 0; next }
  /^static const char \*reader_file\(lua_State \*L, void \*ud, size_t \*size\)/ {
    in_reader = 1
  }
  in_reader && /^}/ { in_reader = 0; next }
  in_reader && /load_had_stopreq\(L\)/ { print FILENAME ":" FNR ":" $0 }
  /lj_safepoint_checkstop\(L, ctx[.]actions\);/ { print FILENAME ":" FNR ":" $0 }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh {
    print FILENAME ":" FNR ":" $0
  }
' "$ROOT/src/lj_load.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'loadfile native STOPREQ checks must use fresh semantics' >&2
  exit 1
fi
for required in \
  'static int io_had_stopreq(lua_State *L)' \
  'static uint32_t io_poll_pending_stopreq(lua_State *L, uint32_t actions)' \
  'static void io_checkstop_fresh(lua_State *L, uint32_t actions, int had_stopreq)' \
  'io_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lib_io.c"; then
    printf '%s\n' "io native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void io_checkstop_fresh\(lua_State \*L, uint32_t actions, int had_stopreq\)/ {
    in_fresh = 1
  }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh {
    print FILENAME ":" FNR ":" $0
  }
  /lj_safepoint_checkstop\(L, lj_native_leave\(L\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  in_fresh && /^}/ {
    in_fresh = 0
  }
' "$ROOT/src/lib_io.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'io native action checks must use fresh STOPREQ semantics' >&2
  exit 1
fi
for required in \
  'static void package_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'package_fresh_stopreq(L, actions, had_stopreq)' \
  'package_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lib_package.c"; then
    printf '%s\n' "package loader fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void package_checkstop_fresh\(lua_State \*L, uint32_t actions,/ {
    in_fresh = 1
  }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh {
    print FILENAME ":" FNR ":" $0
  }
  in_fresh && /^}/ {
    in_fresh = 0
  }
' "$ROOT/src/lib_package.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'package loader native action checks must use fresh STOPREQ semantics' >&2
  exit 1
fi
for required in \
  'static void os_checkstop_fresh(lua_State *L, uint32_t actions, int had_stopreq,' \
  'os_fresh_stopreq(L, actions, had_stopreq, had_pending_stopreq)' \
  'os_checkstop_fresh(L, actions, had_stopreq, had_pending_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lib_os.c"; then
    printf '%s\n' "os native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void os_checkstop_fresh\(lua_State \*L, uint32_t actions, int had_stopreq,/ {
    in_fresh = 1
  }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh {
    print FILENAME ":" FNR ":" $0
  }
  /lj_safepoint_checkstop\(L, lj_native_leave\(L\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  in_fresh && /^}/ {
    in_fresh = 0
  }
' "$ROOT/src/lib_os.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'os native action checks must use fresh STOPREQ semantics' >&2
  exit 1
fi
for required in \
  'static void print_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'print_fresh_stopreq(L, actions, had_stopreq)' \
  'print_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lib_base.c"; then
    printf '%s\n' "print native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void print_native_write\(lua_State \*L, const char \*str, size_t size\)/ {
    in_write = 1
  }
  in_write && /lj_safepoint_checkstop\(L, lj_native_leave\(L\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  in_write && /^}/ {
    in_write = 0
  }
' "$ROOT/src/lib_base.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'print native writes must use fresh STOPREQ semantics' >&2
  exit 1
fi
for required in \
  'static void debug_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'debug_fresh_stopreq(L, actions, had_stopreq)' \
  'debug_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lib_debug.c"; then
    printf '%s\n' "debug native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static (void|char \*)debug_native_(fputs|fgets)\(/ {
    in_debug_native = 1
  }
  in_debug_native && /lj_safepoint_checkstop\(L, lj_native_leave\(L\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  in_debug_native && /^}/ {
    in_debug_native = 0
  }
' "$ROOT/src/lib_debug.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'debug native I/O must use fresh STOPREQ semantics' >&2
  exit 1
fi
for required in \
  'static void frontend_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'frontend_fresh_stopreq(L, actions, had_stopreq)' \
  'frontend_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/luajit.c"; then
    printf '%s\n' "frontend native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void frontend_(fwrite|fflush)\(/ ||
  /^static char \*frontend_fgets\(/ {
    in_frontend_native = 1
  }
  in_frontend_native && /lj_safepoint_checkstop\(L, lj_native_leave\(L\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  in_frontend_native && /^}/ {
    in_frontend_native = 0
  }
' "$ROOT/src/luajit.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'frontend native I/O must use fresh STOPREQ semantics' >&2
  exit 1
fi
for required in \
  'static void chan_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'chan_poll_pending_stopreq(L, actions)' \
  'chan_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lj_chan.c"; then
    printf '%s\n' "channel native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void chan_checkstop_fresh\(lua_State \*L, uint32_t actions,/ {
    in_fresh = 1
  }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh {
    print FILENAME ":" FNR ":" $0
  }
  /lj_safepoint_checkstop\(L, lj_native_leave\(L\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  in_fresh && /^}/ {
    in_fresh = 0
  }
' "$ROOT/src/lj_chan.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'channel native waits must use fresh STOPREQ semantics' >&2
  exit 1
fi
for required in \
  'static void threading_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'threading_poll_pending_stopreq(L, actions)' \
  'threading_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lib_threading.c"; then
    printf '%s\n' "threading native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done
if hits=$(awk '
  /^static void threading_checkstop_fresh\(lua_State \*L, uint32_t actions,/ {
    in_fresh = 1
  }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh {
    print FILENAME ":" FNR ":" $0
  }
  /lj_safepoint_checkstop\(L, lj_native_leave\(L\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  /lj_safepoint_checkstop\(L, lj_thr_sleep_ns\(L, ns\)\);/ {
    print FILENAME ":" FNR ":" $0
  }
  /lj_safepoint_checkstop\(L, join_actions\);/ {
    print FILENAME ":" FNR ":" $0
  }
  in_fresh && /^}/ {
    in_fresh = 0
  }
' "$ROOT/src/lib_threading.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'threading native waits must use fresh STOPREQ semantics' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*next_tg|&[[:alnum:]_]+->[[:space:]]*next_tg|next_tg[[:space:]]*=' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_safepoint.c" \
    "$ROOT/src/lib_threading.c" \
    "$ROOT/src/lj_tg.c" \
    "$ROOT/tests/t-thr-substrate.c" \
    "$ROOT/tests/t-safepoint-handshake.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TGState next_tg access is forbidden; use lj_tg_next_* helpers' >&2
  exit 1
fi
if ! grep -qE 'static LJ_AINLINE uint32_t gc2_finalizer_owner_acq[[:space:]]*[(]' \
    "$ROOT/src/lj_obj.h"; then
  printf '%s\n' 'gc2_finalizer_owner_acq helper is required for finalizer owner reads' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]finalizer_owner_tid|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]finalizer_owner_tid' \
    "$ROOT/tests/t-safepoint-handshake.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw safepoint fixture finalizer owner access is forbidden; use gc2_finalizer_owner_* helpers' >&2
  exit 1
fi
if hits=$(grep -RInE -- '->[[:space:]]*(poll|reqmask|hs_epoch_ack)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*(poll|reqmask|hs_epoch_ack)([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG safepoint mirror access is forbidden; use lj_tg_* safepoint helpers' >&2
  exit 1
fi
for helper in \
  lj_tg_mark_active_acq \
  lj_tg_mark_active_rel \
  lj_tg_alloc_black_acq \
  lj_tg_alloc_black_rel
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG GC mirror helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*mark_active([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*mark_active([^[:alnum:]_]|$)|->[[:space:]]*alloc[.]alloc_black([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*alloc[.]alloc_black([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw C-side TG GC mirror access is forbidden; use lj_tg_* mirror helpers' >&2
  exit 1
fi
for helper in lj_tg_in_native_acq lj_tg_in_native_rel lj_tg_in_native_store_rlx
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG native-state helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*in_native([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*in_native([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG native-state access is forbidden; use lj_tg_in_native_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  FNR == 1 { fn = "" }
  /^static FILE \*perftools_native_fopen\(lua_State \*L, const char \*fname\)$/ {
    fn = "perftools_native_fopen"
  }
  /^static void perftools_native_fprintf\(lua_State \*L, FILE \*fp, uintptr_t mcode,$/ {
    fn = "perftools_native_fprintf"
  }
  /^static void mcode_native_leave\(lua_State \*L\)$/ {
    fn = "mcode_native_leave"
  }
  /^static void aux_finalizer_error_report\(lua_State \*L, const char \*s\)$/ {
    fn = "aux_finalizer_error_report"
  }
  /^}/ { fn = "" }
  /\(void\)[[:space:]]*lj_native_leave\(L\)[[:space:]]*;/ {
    ok = 0
    if (FILENAME ~ /src\/lj_trace[.]c$/ &&
	(fn == "perftools_native_fopen" ||
	 fn == "perftools_native_fprintf")) ok = 1
    if (FILENAME ~ /src\/lj_mcode[.]c$/ &&
	fn == "mcode_native_leave") ok = 1
    if (FILENAME ~ /src\/lib_aux[.]c$/ &&
	fn == "aux_finalizer_error_report") ok = 1
    if (!ok) print FILENAME ":" FNR ":" $0
  }
' "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'native leaves must route STOPREQ actions or be a documented deferred recorder/finalizer boundary' >&2
  exit 1
fi
for helper in \
  lj_tg_flags_acq \
  lj_tg_flags_store_rlx \
  lj_tg_flags_or_rlx \
  lj_tg_flags_and_rlx \
  lj_tg_flags_test_acq \
  lj_tg_flags_all_acq
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG flag helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*tg_flags([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*tg_flags([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG flag access is forbidden; use lj_tg_flags_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](tg_list|n_threads)([^[:alnum:]_]|$)|gc2[[:space:]]*->[[:space:]]*(tg_list|n_threads)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_safepoint.c" \
    "$ROOT/src/lib_threading.c" \
    "$ROOT/src/lj_tg.c" \
    "$ROOT/src/lj_dispatch.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 TG registry access is forbidden; use gc2_tg_* helpers' >&2
  exit 1
fi
fields='hs_epoch|hs_pending|hs_actions|hs_leader|hs_signal_ns|hs_ack_latency_samples|hs_ack_latency_sum_ns|hs_ack_latency_max_ns|hs_ack_latency_buckets'
if hits=$(grep -nE -- "->[[:space:]]*gc2[.](${fields})([^[:alnum:]_]|$)|gc2[[:space:]]*->[[:space:]]*(${fields})([^[:alnum:]_]|$)" \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 handshake state access is forbidden; use gc2_hs_* helpers' >&2
  exit 1
fi
if ! grep -qE 'LJ_FUNC uint64_t lj_gc2_retire_epoch[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.h"; then
  printf '%s\n' 'lj_gc2_retire_epoch declaration is required for SMR retire epoch ownership' >&2
  exit 1
fi
if ! grep -qE '^uint64_t lj_gc2_retire_epoch[[:space:]]*[(]' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'lj_gc2_retire_epoch definition is required for SMR retire epoch ownership' >&2
  exit 1
fi
if hits=$(grep -nE -- 'gc2_hs_epoch_acq[[:space:]]*[(]' \
    "$ROOT/src/lj_str.c" \
    "$ROOT/src/lj_tab.c" \
    "$ROOT/src/lj_trace.c" \
    "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_clib.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'SMR retire producers must query lj_gc2_retire_epoch instead of raw handshake epoch' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m3_safepoint_handshake
