#!/bin/sh
# Guard M5 userdata type publication and snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_udata_udtype_acq(const GCudata *ud)' \
  'la_load8_acq(&ud->udtype)' \
  'lj_udata_udtype_rel(GCudata *ud, uint8_t udtype)' \
  'la_store8_rel(&ud->udtype, udtype)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h"; then
    echo "guardrail: missing userdata type acquire/release helper: $needle" >&2
    exit 1
  fi
done

hits=$(rg -n --glob '*.c' --glob '*.h' -- \
  '->udtype\s*=|->udtype\s*(==|!=)|->udtype\b' "$ROOT/src" | \
  rg -v '^.+/src/lj_obj\.h:' || true)
if [ -n "$hits" ]; then
  echo "guardrail: GCudata.udtype must use acquire/release helpers:" >&2
  echo "$hits" >&2
  exit 1
fi

check_order() {
  file=$1
  start=$2
  shift 2
  awk -v start="$start" -v needles="$(printf '%s\n' "$@")" '
    BEGIN {
      n = split(needles, seq, "\n")
      pos = 1
    }
    $0 ~ start { infn = 1; seen = 1 }
    infn && pos <= n && index($0, seq[pos]) { pos++ }
    infn && /^}/ { exit(seen && pos > n ? 0 : 1) }
    END { if (!seen || pos <= n) exit 1 }
  ' "$file"
}

check_order "$ROOT/src/lib_buffer.c" 'LJLIB_CF\(buffer_new\)' \
  'setgcrefmt(ud->metatable, obj2gco(env));' \
  'lj_gc_pubobjobj(L, ud, env);' \
  'lj_bufx_init(L, sbx);' \
  'setgcrefrel(sbx->dict_str, obj2gco(dict_str));' \
  'lj_gc_pubobjobj(L, ud, dict_str);' \
  'setgcrefrel(sbx->dict_mt, obj2gco(dict_mt));' \
  'lj_gc_pubobjobj(L, ud, dict_mt);' \
  'lj_udata_udtype_rel(ud, UDTYPE_BUFFER);' \
  'if (sz > 0) lj_buf_need2((SBuf *)sbx, sz);'

check_order "$ROOT/src/lib_threading.c" 'static GCudata \*threading_new_thread_ud' \
  'setgcrefmt(ud->metatable, obj2gco(env));' \
  'lj_gc_pubobjobj(L, ud, env);' \
  'th->ud = ud;' \
  'lj_udata_udtype_rel(ud, UDTYPE_THREAD);'

check_order "$ROOT/src/lib_threading.c" 'LJLIB_CF\(threading_mutex\)' \
  'setgcrefmt(ud->metatable, obj2gco(env));' \
  'lj_gc_pubobjobj(L, ud, env);' \
  'm->state = LJ_MUTEX_UNLOCKED;' \
  'lj_udata_udtype_rel(ud, UDTYPE_MUTEX);'

check_order "$ROOT/src/lib_threading.c" 'LJLIB_CF\(threading_channel\)' \
  'setgcrefmt(ud->metatable, obj2gco(env));' \
  'lj_gc_pubobjobj(L, ud, env);' \
  'lj_chan_init((LJChan *)uddata(ud), cap);' \
  'lj_udata_udtype_rel(ud, UDTYPE_CHANNEL);'

check_order "$ROOT/src/lib_io.c" 'static IOFileUD \*io_file_new' \
  'setgcrefmt(ud->metatable, obj2gco(mt));' \
  'lj_gc_pubobjobj(L, ud, mt);' \
  'iof->fp = NULL;' \
  'iof->type = IOFILE_TYPE_FILE;' \
  'lj_udata_udtype_rel(ud, UDTYPE_IO_FILE);'

check_order "$ROOT/src/lib_io.c" 'static GCobj \*io_std_new' \
  'setgcrefmt(ud->metatable, obj2gco(mt));' \
  'lj_gc_pubobjobj(L, ud, mt);' \
  'iof->fp = fp;' \
  'iof->type = IOFILE_TYPE_STDF;' \
  'lj_udata_udtype_rel(ud, UDTYPE_IO_FILE);' \
  'lua_setfield(L, -2, name);'

check_order "$ROOT/src/lj_clib.c" 'static CLibrary \*clib_new' \
  'cl->cache = t;' \
  'setgcrefmt(ud->metatable, obj2gco(mt));' \
  'lj_gc_pubobjobj(L, ud, mt);' \
  'lj_udata_udtype_rel(ud, UDTYPE_FFI_CLIB);'

make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local ffi = require"ffi"
ffi.cdef"int puts(const char *);"
assert(type(ffi.C.puts) == "cdata")

local f = io.tmpfile()
assert(io.type(f) == "file")
f:close()
assert(io.type(f) == "closed file")

local ok, buffer = pcall(require, "string.buffer")
if ok then
  for i = 1, 32 do
    local b = buffer.new(i % 8)
    collectgarbage("collect")
    assert(type(b) == "userdata")
  end
end

local th = require"threading"
local m = th.mutex()
assert(m:trylock() == true)
assert(m:trylock() == false)
assert(m:unlock() == nil)
local ch = th.channel(2)
assert(ch:send("x") == true)
local v, okrecv = ch:recv()
assert(v == "x" and okrecv == true)
local me = th.current()
assert(type(me:id()) == "number")
collectgarbage("collect")
collectgarbage("collect")
print("udtype-publish-smoke OK")
'

echo "M5 userdata type publication guard passed"
