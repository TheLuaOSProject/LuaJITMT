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

echo "M5 metatable publication guard passed"
