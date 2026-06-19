#!/bin/sh
# Guard M5 CType.name publication and acquire snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'ctype_name_acq(const CType *ct)' \
  'gcref_acq(ct->name)' \
  'setgcrefrel(ct->name, obj2gco(s))' \
  'ctype_clearname(CType *ct)' \
  'setgcrefnullrel(ct->name)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_ctype.h"; then
    echo "guardrail: missing CType.name acquire/release helper: $needle" >&2
    exit 1
  fi
done

hits=$(rg -n --glob '*.c' --glob '*.h' -- \
  'gcref\([^)]*->name|gcref\(ct->name|setgcref\([^)]*->name|setgcref\(ct->name|setgcrefnull\(ct->name|gco2str\(gcref\([^)]*->name' \
  "$ROOT/src/lj_ctype.h" \
  "$ROOT/src/lj_ctype.c" \
  "$ROOT/src/lj_cparse.c" \
  "$ROOT/src/lj_cconv.c" \
  "$ROOT/src/lj_clib.c" \
  "$ROOT/src/lj_crecord.c" \
  "$ROOT/src/lib_ffi.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: CType.name readers/writers must use ctype_name_acq()/ctype_setname():" >&2
  echo "$hits" >&2
  exit 1
fi

if ! rg -F -q 'm5_ctype_name_publish.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: m5_ctype_name_publish.sh is not wired into the M5 aggregate" >&2
  exit 1
fi

make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local ffi = require"ffi"
for i = 1, 40 do
  ffi.cdef(([[typedef struct { int a; double b; } lj_ctype_name_s_%d;
typedef enum { LJ_CTYPE_NAME_E_%d = %d } lj_ctype_name_e_%d;]]):format(i, i, i, i))
  local ct = ffi.typeof(("lj_ctype_name_s_%d"):format(i))
  local x = ct(i, i + 0.5)
  assert(x.a == i and x.b == i + 0.5)
  local et = ffi.typeof(("lj_ctype_name_e_%d"):format(i))
  assert(tonumber(et(i)) == i)
  collectgarbage("collect")
end
local mt = ffi.metatype("struct { int x; }", {
  __index = { value = function(self) return self.x end }
})
assert(mt(7):value() == 7)
print("ctype-name-publish-smoke OK")
'

echo "M5 CType.name publication guard passed"
