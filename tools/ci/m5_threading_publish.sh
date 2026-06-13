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

for needle in \
  'gcref_acq(child->mt_thread)' \
  'gcref_acq(L->mt_thread)' \
  'mt = gcref_acq(th->mt_thread)' \
  'gcref_acq(g->gcroot[GCROOT_THREADING_ENV])'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_threading.c" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
    echo "guardrail: mt_thread readers must acquire-load publication: $needle" >&2
    exit 1
  fi
done

if rg -n '\bgcref\([^)]*mt_thread' \
    "$ROOT/src/lib_threading.c" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
  echo "guardrail: mt_thread readers must use gcref_acq" >&2
  exit 1
fi

if rg -n '\bgcref\(g->gcroot\[GCROOT_THREADING_ENV\]\)' \
    "$ROOT/src/lib_threading.c"; then
  echo "guardrail: threading root readers must use gcref_acq" >&2
  exit 1
fi

live_table_hits=$(rg -n 'THREADING_THREADS_KEY|threading_live_table|threading_live_root|lj_tab_next\(live|lj_tab_set\(L, live|lj_tab_storethread\(L, lj_tab_set\(L, live|GCROOT_THREADING\b' \
  "$ROOT/src/lib_threading.c" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_thr.h" || true)
if [ -n "$live_table_hits" ]; then
  echo "guardrail: live thread registry must not use the old shared GCtab root:" >&2
  echo "$live_table_hits" >&2
  exit 1
fi

for needle in \
  'LJThreadLive *threading_live' \
  'struct LJThreadLive' \
  'LJThreadLive *live_node' \
  'threading_live_new(lua_State *L, GCudata *ud)' \
  'threading_live_publish(global_State *g, LJThread *th,' \
  'la_casptr((void **)&g->threading_live' \
  'la_xchgptr_acqrel((void **)&g->threading_live' \
  'la_storeptr_rel((void **)&th->live_node, NULL)' \
  'setgcrefrel(node->ud, obj2gco(ud))' \
  'setgcrefrel(node->ud, NULL)' \
  'lj_gc_barrierroot(L, &tv)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_threading.c" \
      "$ROOT/src/lj_obj.h" "$ROOT/src/lj_thr.h"; then
    echo "guardrail: live thread registry must use native lockless roots: $needle" >&2
    exit 1
  fi
done

for file in "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; do
  for needle in 'g->threading_live' 'gcref_acq(node->ud)'; do
    if ! rg -F -q "$needle" "$file"; then
      echo "guardrail: GC must scan native live thread roots: $file: $needle" >&2
      exit 1
    fi
  done
done

for needle in \
  'lj_gc_threshold_load(global_State *g)' \
  'lj_gc_threshold_store(global_State *g, GCSize threshold)' \
  'lj_gc_mt_threshold_load(global_State *g)' \
  'lj_gc_mt_threshold_store(global_State *g,' \
  'lj_gc_mt_threshold_store(g, lj_gc_threshold_load(g))' \
  'lj_gc_threshold_store(g, lj_gc_mt_threshold_load(g))' \
  'api_gc_setlogical(global_State *g, GCSize threshold)' \
  'if (la_load32_acq(&g->mt_live) == 0)' \
  'lj_gc_threshold_store(g, threshold)' \
  'uint32_t mt_gc_exclusive' \
  'api_gc_enterexclusive(global_State *g)' \
  'la_cas32(&g->mt_gc_exclusive' \
  'la_store32_rel(&g->mt_gc_exclusive, 0)' \
  'la_futex_wake(&g->mt_gc_exclusive, INT_MAX)' \
  'while ((exclusive = la_load32_acq(&g->mt_gc_exclusive)) != 0)' \
  'la_futex_wait(&g->mt_gc_exclusive, exclusive' \
  'if (la_load32_acq(&g->mt_gc_exclusive) == 0)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.h" "$ROOT/src/lj_obj.h" \
      "$ROOT/src/lib_threading.c" "$ROOT/src/lj_api.c"; then
    echo "guardrail: GC threshold handoff must use atomic helpers: $needle" >&2
    exit 1
  fi
done

threshold_hits=$(rg -n 'gc\.threshold|mt_gc_threshold' \
  "$ROOT/src" -g '*.c' -g '*.h' -g '!**/host/*' |
  rg -v 'src/lj_gc\.h|src/lj_obj\.h|src/lj_asm_.*\.h|offsetof\(global_State, gc\.threshold\)' || true)
if [ -n "$threshold_hits" ]; then
  echo "guardrail: C-side GC threshold access must use atomic helpers" >&2
  echo "$threshold_hits" >&2
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

if ! awk '
  /LJLIB_CF\(threading_channel_recv\)/ { infn = 1; next }
  infn && /setnilV\(&out\)/ { init = 1 }
  infn && /lj_chan_recv_timeout_gc\(L, ch, &out, ns\)/ { recv = 1 }
  infn && /threading_push_recv\(L, rc, &out\)/ { finish = 1 }
  infn && /lj_chan_recv_timeout\(L, ch, &out/ { bad = 1 }
  infn && /^}/ { exit(init && recv && finish && !bad ? 0 : 1) }
  END { if (!init || !recv || !finish || bad) exit 1 }
' "$ROOT/src/lib_threading.c"; then
  echo "guardrail: channel recv must use GC-aware receive" >&2
  exit 1
fi

for needle in \
  'chan_storetv_rel(slot, tv)' \
  'chan_loadtv_acq(out, slot)' \
  'chan_cleartv_rel(slot)' \
  'tv_rawstore_rel(out, tv_rawload_acq(&slot->tv))' \
  'lj_gc_barrierroot(L, out)' \
  'lj_chan_recv_timeout_gc'
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

if rg -n 'out->u64 = tv_rawload_acq|lj_chan_recv_timeout\(L, ch, &out' \
    "$ROOT/src/lj_chan.c" "$ROOT/src/lib_threading.c"; then
  echo "guardrail: channel recv must use GC-aware atomic receive helpers" >&2
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

if ! awk '
  /static void gc_traverse_thread/ { infn = 1; seen = 1; next }
  infn && /lj_tv_load_acq\(&tv, o\)/ { load = 1 }
  infn && /gc_marktv\(g, &tv\)/ { mark = 1 }
  infn && /gc_marktv\(g, o\)/ { bad = 1 }
  infn && /^}/ { exit(seen && load && mark && !bad ? 0 : 1) }
  END { if (!seen || !load || !mark || bad) exit 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: legacy GC thread stack scan must snapshot stack slots" >&2
  exit 1
fi

for fn in gc2_scan_thread_roots gc2_traverse_thread; do
  if ! awk -v fn="$fn" '
    $0 ~ "static void " fn { infn = 1; seen = 1; next }
    infn && /lj_tv_load_acq\(&tv, o\)/ { load = 1 }
    infn && /gc2_mark_tv(_worker)?\(g, &tv\)/ { mark = 1 }
    infn && /gc2_mark_tv(_worker)?\(g, o\)/ { bad = 1 }
    infn && /^}/ { exit(seen && load && mark && !bad ? 0 : 1) }
    END { if (!seen || !load || !mark || bad) exit 1 }
  ' "$ROOT/src/lj_gc2.c"; then
    echo "guardrail: GC2 thread stack scan must snapshot stack slots: $fn" >&2
    exit 1
  fi
done

echo "M5 threading publication guard passed"
