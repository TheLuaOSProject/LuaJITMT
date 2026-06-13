#!/bin/sh
# Guard M5 release publication for runtime metatable stores.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

hits=$(
  cd "$ROOT" && \
  awk '
    function startfn() {
      active = 1
      depth = 0
      saw_body = 0
    }
    function brace_delta(line, nopen, nclose) {
      nopen = gsub(/\{/, "{", line)
      nclose = gsub(/\}/, "}", line)
      return nopen - nclose
    }
    /^LUA_API int lua_setmetatable\(/ { startfn() }
    /^LJLIB_ASM\(setmetatable\)/ { startfn() }
    active {
      if ($0 ~ /setgcref\([^;]*metatable/)
	print FILENAME ":" FNR ":" $0
      delta = brace_delta($0)
      if (delta != 0)
	saw_body = 1
      depth += delta
      if (saw_body && depth == 0)
	active = 0
    }
  ' src/lj_api.c src/lib_base.c
)

if [ -n "$hits" ]; then
  printf '%s\n' "$hits"
  echo "guardrail: runtime metatable publications must use setgcrefmt()" >&2
  exit 1
fi

for needle in \
  '#define tabref_acq(r)' \
  'gcref_acq((r))'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h"; then
    echo "guardrail: shared metatable/env readers must acquire-load: $needle" >&2
    exit 1
  fi
done

reader_hits=$(rg -n '\btabref\((tabV\([^)]*\)->metatable|udataV\([^)]*\)->metatable|basemt_obj|t->metatable|gco2ud\(o\)->metatable|gco2ud\(o\)->env|mainthread\(g\)->env|L->env|L1->env|th->env|ud->metatable|ud->env|fn->[cl]\.env|funcV\(o\)->c\.env|udataV\(o\)->env|curr_func\(L\)->c\.env|parent->env|J->fn->l\.env|sbx->dict)|\bgcref\((sbx->cowref|sbx->dict_str|sbx->dict_mt|t->metatable)' \
  "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_meta.c" \
  "$ROOT/src/lj_serialize.c" "$ROOT/src/lib_ffi.c" \
  "$ROOT/src/lj_cdata.c" "$ROOT/src/lib_threading.c" \
  "$ROOT/src/lj_api.c" "$ROOT/src/lib_base.c" \
  "$ROOT/src/lib_debug.c" "$ROOT/src/lib_buffer.c" \
  "$ROOT/src/lib_jit.c" "$ROOT/src/lj_lib.c" \
  "$ROOT/src/lj_load.c" "$ROOT/src/lj_func.c" \
  "$ROOT/src/lj_record.c" "$ROOT/src/lj_ffrecord.c" || true)
if [ -n "$reader_hits" ]; then
  echo "guardrail: shared metatable/env readers must use acquire helpers:" >&2
  echo "$reader_hits" >&2
  exit 1
fi

env_store_hits=$(rg -n '\bsetgcref\(L->env' \
  "$ROOT/src/lj_api.c" "$ROOT/src/lib_base.c" || true)
if [ -n "$env_store_hits" ]; then
  echo "guardrail: current-thread env replacement must use release stores:" >&2
  echo "$env_store_hits" >&2
  exit 1
fi

echo "M5 metatable publication guard passed"
