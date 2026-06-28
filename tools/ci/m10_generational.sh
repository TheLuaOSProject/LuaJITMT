#!/bin/sh
# Run the Lua-defined M10 generational-mode guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

fields='generational|force_major|cycle_minor_requested|cycle_sweep_minor|cycle_roots_minor|minor_sweep_enabled|minor_roots_enabled|minor_survival_pct|minor_survival_threshold_pct'
if hits=$(grep -nE -- "->[[:space:]]*gc2[.](${fields})([^[:alnum:]_]|$)|gc2[[:space:]]*->[[:space:]]*(${fields})([^[:alnum:]_]|$)" \
  "$ROOT/src/lj_api.c" \
  "$ROOT/src/lj_gc.c" \
  "$ROOT/src/lj_gc2.c" \
  "$ROOT/src/lj_tg.c" \
  "$ROOT/src/lib_base.c" || true); then
  if [ -n "$hits" ]; then
    printf '%s\n' \
      "raw GC2 generational/minor gate state access is forbidden; use gc2_* mode/gate helpers" \
      "$hits" >&2
    exit 1
  fi
fi

if ! grep -qE 'LJ_FUNC int lj_gc2_minor_roots_skip_bridge_mark[[:space:]]*[(]' \
  "$ROOT/src/lj_gc2.h"; then
  printf '%s\n' \
    "lj_gc2_minor_roots_skip_bridge_mark declaration is required for minor-root bridge policy" >&2
  exit 1
fi
if ! grep -qE '^int lj_gc2_minor_roots_skip_bridge_mark[[:space:]]*[(]' \
  "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' \
    "lj_gc2_minor_roots_skip_bridge_mark definition is required for minor-root bridge policy" >&2
  exit 1
fi
if ! grep -qE 'LJ_FUNC int lj_gc2_minor_roots_active[[:space:]]*[(]' \
  "$ROOT/src/lj_gc2.h"; then
  printf '%s\n' \
    "lj_gc2_minor_roots_active declaration is required for minor-root cycle ownership" >&2
  exit 1
fi
if ! grep -qE '^int lj_gc2_minor_roots_active[[:space:]]*[(]' \
  "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' \
    "lj_gc2_minor_roots_active definition is required for minor-root cycle ownership" >&2
  exit 1
fi
if hits=$(awk '
  /gc2_phase_acq[[:space:]]*[(][[:space:]]*g[[:space:]]*[)][[:space:]]*==[[:space:]]*LJ_GC2_MARK/ {
    phase_line = FNR
    phase_text = $0
  }
  phase_line && /gc2_cycle_roots_minor_acq[[:space:]]*[(][[:space:]]*g[[:space:]]*[)]/ {
    print FILENAME ":" phase_line ":" phase_text
    print FILENAME ":" FNR ":" $0
    phase_line = 0
  }
  /^}/ { phase_line = 0 }
' "$ROOT/src/lj_gc.c" || true); then
  if [ -n "$hits" ]; then
    printf '%s\n' \
      "minor-root bridge mark policy must stay behind lj_gc2_minor_roots_skip_bridge_mark" \
      "$hits" >&2
    exit 1
  fi
fi
if ! grep -qF 'lj_gc2_minor_roots_skip_bridge_mark(g)' "$ROOT/src/lj_gc.c"; then
  printf '%s\n' \
    "arena mark bridge must query lj_gc2_minor_roots_skip_bridge_mark" >&2
  exit 1
fi
if ! grep -qF 'lj_gc2_minor_roots_active(g)' "$ROOT/src/lj_gc.c"; then
  printf '%s\n' \
    "GC paranoia minor-root skip must query lj_gc2_minor_roots_active" >&2
  exit 1
fi
if ! grep -qF 's->cycle_roots_minor = lj_gc2_minor_roots_active(g);' \
    "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' \
    "GC stats snapshot must query lj_gc2_minor_roots_active for the minor-root cycle state" >&2
  exit 1
fi
if hits=$(grep -nE -- 'gc2_cycle_roots_minor_acq[[:space:]]*[(][[:space:]]*g[[:space:]]*[)]' \
  "$ROOT/src/lj_gc.c" || true); then
  if [ -n "$hits" ]; then
    printf '%s\n' \
      "legacy GC must not read raw GC2 minor-root cycle latch" \
      "$hits" >&2
    exit 1
  fi
fi
exec "$ROOT/tools/ci/lua_test.sh" m10_generational
