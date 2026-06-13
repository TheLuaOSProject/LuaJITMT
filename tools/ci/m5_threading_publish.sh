#!/bin/sh
# Guard M5 threading publication stores and barrier call-site migration.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

hits=$(rg -n "lj_gc_(objbarrier|objbarriert|anybarriert|barrieruv|barriert|barrier)\\b" \
  "$ROOT/src/lib_threading.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: lib_threading must use M5 publication wrappers:" >&2
  echo "$hits" >&2
  exit 1
fi

if ! awk '
  /static void threading_state_set_ud/ { infn = 1; next }
  infn && /setgcrefrel\(L1->mt_thread/ { rel = 1 }
  infn && /lj_gc_pubobjobj\(L, L1, ud\)/ { pub = 1 }
  infn && /^}/ { exit(rel && pub ? 0 : 1) }
  END { if (!rel || !pub) exit 1 }
' "$ROOT/src/lib_threading.c"; then
  echo "guardrail: threading_state_set_ud must release-publish mt_thread" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(threading_channel_send\)/ { infn = 1; next }
  infn && /lj_gc_pubobjtv\(L, ud, tv\)/ { pub = 1 }
  infn && /lj_chan_send_timeout/ { send = 1 }
  infn && /^}/ { exit(pub && send ? 0 : 1) }
  END { if (!pub || !send) exit 1 }
' "$ROOT/src/lib_threading.c"; then
  echo "guardrail: channel send must publish payload before enqueue" >&2
  exit 1
fi

for needle in \
  'chan_storetv_rel(slot, tv)' \
  'chan_loadtv_acq(out, slot)' \
  'chan_cleartv_rel(slot)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_chan.c"; then
    echo "guardrail: missing channel payload atomic marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'slot->tv = \*tv|\*out = slot->tv|setnilV\(&slot->tv\)' \
    "$ROOT/src/lj_chan.c"; then
  echo "guardrail: channel payload slots must use atomic TValue helpers" >&2
  exit 1
fi

for file in "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; do
  if ! rg -F -q 'lj_tv_load_acq(&tv, &ch->slot[i].tv)' "$file"; then
    echo "guardrail: GC channel traversal must snapshot slot payloads: $file" >&2
    exit 1
  fi
done

if rg -n 'gc_marktv\(g, &ch->slot\[i\]\.tv\)|gc2_mark_tv_worker\(g, &ch->slot\[i\]\.tv\)' \
    "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
  echo "guardrail: GC channel traversal must not mark shared slots directly" >&2
  exit 1
fi

echo "M5 threading publication guard passed"
