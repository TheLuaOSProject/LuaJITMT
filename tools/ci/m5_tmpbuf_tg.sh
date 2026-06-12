#!/bin/sh
# Guard M5 per-TG temporary string buffer routing.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if ! awk '
  /static LJ_AINLINE SBuf \*lj_buf_tmp_/ { infn = 1; next }
  infn && /L2TG\(L\)->tmpbuf/ { tg = 1 }
  infn && /lj_buf_reset\(sb\)/ { reset = 1 }
  infn && /^}/ { exit(tg && reset ? 0 : 1) }
  END { if (!tg || !reset) exit 1 }
' "$ROOT/src/lj_buf.h"; then
  echo "guardrail: lj_buf_tmp_ must route through per-TG tmpbuf" >&2
  exit 1
fi

if ! awk '
  /TValue \*lj_meta_cat/ { infn = 1; next }
  infn && /lj_buf_tmp_\(L\)/ { cat = 1 }
  infn && /setstrV\(L, top, lj_buf_str\(L, sb\)\)/ { result = 1 }
  infn && /^}/ { exit(cat && result ? 0 : 1) }
  END { if (!cat || !result) exit 1 }
' "$ROOT/src/lj_meta.c"; then
  echo "guardrail: lj_meta_cat must concatenate via per-TG tmpbuf" >&2
  exit 1
fi

if ! awk '
  /const char \*lj_strfmt_pushvf/ { infn = 1; next }
  infn && /lj_buf_tmp_\(L\)/ { fmt = 1 }
  infn && /^}/ { exit(fmt ? 0 : 1) }
  END { if (!fmt) exit 1 }
' "$ROOT/src/lj_strfmt.c"; then
  echo "guardrail: formatted push helpers must use per-TG tmpbuf" >&2
  exit 1
fi

hits=$(rg -n -g '*.c' -g '*.h' \
  -g '!lj_gc.c' -g '!lj_gc2.c' -g '!lj_state.c' -g '!lj_tg.c' \
  -- "(^|[^A-Za-z0-9_])g->tmpbuf" "$ROOT/src" || true)
if [ -n "$hits" ]; then
  echo "guardrail: global tmpbuf access is limited to init/GC ownership code:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -g '*.c' -g '*.h' -- "&L2TG\\(L\\)->tmpbuf" "$ROOT/src" \
  | rg -v '/src/(lj_buf\.c|lj_buf\.h|lj_serialize\.c):' || true)
if [ -n "$hits" ]; then
  echo "guardrail: ordinary runtime tmpbuf access must go through lj_buf_tmp_:" >&2
  echo "$hits" >&2
  exit 1
fi

echo "M5 per-TG tmpbuf guard passed"
