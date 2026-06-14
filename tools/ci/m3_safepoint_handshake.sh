#!/bin/sh
# Build and run the focused C-level safepoint handshake test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t_safepoint_handshake

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-safepoint-handshake.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -o "$OUT"
"$OUT"

for needle in \
  'TGState *self = lj_thr_get_tg()' \
  'Leader self-ack is a real poll' \
  'remote native ack' \
  'lj_gc2_reclaim_retired(g, epoch)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_safepoint.c"; then
    echo "guardrail: missing safepoint handshake marker: $needle" >&2
    exit 1
  fi
done

if rg -F -q 'Deterministic single-mutator scaffold' "$ROOT/src/lj_safepoint.c"; then
  echo "guardrail: non-native TGs must not be remotely acked" >&2
  exit 1
fi

for needle in \
  'uint32_t phase = la_load32_acq(&g->gc2.phase)' \
  'phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK' \
  'phase == LJ_GC2_SWEEP' \
  'assert_attach_phase(L, g, tg, LJ_GC2_WEAK, 1, 1)' \
  'assert_attach_phase(L, g, tg, LJ_GC2_SWEEP, 0, 1)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tg.c" \
      "$ROOT/tests/t-safepoint-handshake.c"; then
    echo "guardrail: missing TG attach phase marker: $needle" >&2
    exit 1
  fi
done

for needle in \
  'lj_safepoint_checkstop(L, lj_native_leave(L));' \
  'lj_safepoint_checkstop(L, actions);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_os.c"; then
    echo "guardrail: lib_os native leave must propagate STOPREQ: $needle" >&2
    exit 1
  fi
done

for needle in \
  'os_native_mkstemp(lua_State *L, char *buf)' \
  'actions = lj_native_leave(L);' \
  'remove(buf);' \
  'lj_safepoint_checkstop(L, actions);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_os.c"; then
    echo "guardrail: os.tmpname native leave must propagate STOPREQ: $needle" >&2
    exit 1
  fi
done

for needle in \
  'io_native_fflush(lua_State *L, FILE *fp)' \
  'lj_safepoint_checkstop(L, lj_native_leave(L));'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_io.c"; then
    echo "guardrail: lib_io fflush native leave must propagate STOPREQ: $needle" >&2
    exit 1
  fi
done

for needle in \
  'io_native_fscanf_num(lua_State *L, FILE *fp, lua_Number *dp)' \
  'io_native_fgets(lua_State *L, char *buf, int size, FILE *fp)' \
  'io_native_fread(lua_State *L, void *buf, size_t size,' \
  'lj_safepoint_checkstop(L, lj_native_leave(L));'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_io.c"; then
    echo "guardrail: lib_io read native leave must propagate STOPREQ: $needle" >&2
    exit 1
  fi
done

for needle in \
  'LJLIB_CF(io_tmpfile)' \
  'actions = lj_native_leave(L);' \
  '(void)fclose(fp);' \
  'lj_safepoint_checkstop(L, actions);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_io.c"; then
    echo "guardrail: io.tmpfile native leave must cleanup and propagate STOPREQ: $needle" >&2
    exit 1
  fi
done

for needle in \
  'LJLIB_CF(io_method_seek)' \
  'res = fseeko(fp, ofs, opt);' \
  'ofs = ftello(fp);' \
  'lj_safepoint_checkstop(L, actions);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_io.c"; then
    echo "guardrail: lib_io seek native leave must propagate STOPREQ: $needle" >&2
    exit 1
  fi
done

for needle in \
  'actions = lj_native_leave(L);' \
  'lj_safepoint_checkstop(L, actions);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_ccall.c"; then
    echo "guardrail: FFI native leave must propagate STOPREQ after cleanup: $needle" >&2
    exit 1
  fi
done

for needle in \
  'publish_stopreq()' \
  "os.execute(':')" \
  'os.tmpname()' \
  'io.tmpfile()' \
  'f:flush()' \
  "f:seek('set', 0)" \
  "f:read('*n')" \
  "f:read('*l')" \
  'f:read(1)' \
  "f:read('*a')" \
  'thread interrupted: VM shutdown'
do
  if ! rg -F -q "$needle" "$ROOT/tests/t-safepoint-handshake.c"; then
    echo "guardrail: missing lib_os STOPREQ coverage marker: $needle" >&2
    exit 1
  fi
done

for needle in \
  'ffi_stopreq_ptr' \
  "ffi.cast('stopreq_t', ffi_stopreq_ptr)" \
  'return stopreq()'
do
  if ! rg -F -q "$needle" "$ROOT/tests/t-safepoint-handshake.c"; then
    echo "guardrail: missing FFI STOPREQ coverage marker: $needle" >&2
    exit 1
  fi
done

echo "M3 safepoint handshake tests passed"
