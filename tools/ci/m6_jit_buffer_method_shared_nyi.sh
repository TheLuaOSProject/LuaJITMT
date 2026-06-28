#!/bin/sh
# Run the M6 JIT string.buffer shared-method NYI guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if ! awk '
  /UDTYPE_BUFFER/ { in_block = 1 }
  in_block && /lj_tv_load_acq\(&valv, val\)/ { saw_acq = 1 }
  in_block && /lj_record_constify\(J, &valv\)/ { saw_constify = 1 }
  in_block && /^    }$/ { in_block = 0 }
  END { exit(saw_acq && saw_constify ? 0 : 1) }
' "$ROOT/src/lj_record.c"; then
  printf '%s\n' 'buffer method recorder lookup must snapshot method slots before constify' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_buffer_method_shared_nyi
