local function lines(s)
  local out = {}
  for line in (s .. "\n"):gmatch("(.-)\n") do
    if line ~= "" then out[#out + 1] = line end
  end
  return out
end

local function contains(s, needle)
  return s:find(needle, 1, true) ~= nil
end

local function assert_no_lines(t, label, paths, pred)
  local hits = {}
  for i = 1, #paths do
    local path = paths[i]
    local n = 0
    for line in (t:read(path) .. "\n"):gmatch("(.-)\n") do
      n = n + 1
      if pred(line, path, n) then
        hits[#hits + 1] = path .. ":" .. n .. ": " .. line
      end
    end
  end
  if #hits > 0 then
    error(label .. ":\n" .. table.concat(hits, "\n"), 2)
  end
end

local function assert_block_contains(label, block, needle)
  if not contains(block, needle) then
    error(label .. ": missing expected text: " .. needle, 2)
  end
end

local function assert_block_absent(label, block, needle)
  if contains(block, needle) then
    error(label .. ": forbidden text present: " .. needle, 2)
  end
end

local function source_and_test_files(t)
  return {
    t:path("src", "lj_gc2.c"),
    t:path("src", "lj_gc2.h"),
    t:path("src", "lj_gc.c"),
    t:path("src", "lj_gc.h"),
    t:path("src", "lj_meta.c"),
    t:path("src", "lj_lib.c"),
    t:path("src", "lib_ffi.c"),
    t:path("src", "lib_jit.c"),
    t:path("src", "lib_table.c"),
    t:path("src", "lib_base.c"),
    t:path("src", "lj_tab.c"),
    t:path("src", "lj_obj.h"),
    t:path("src", "lj_ctype.c"),
    t:path("src", "lj_ctype.h"),
    t:path("src", "lj_cdata.c"),
    t:path("src", "lj_state.c"),
    t:path("src", "lib_threading.c"),
    t:path("src", "lj_udata.c"),
    t:path("tests", "t-gc2-phase.c"),
    t:path("tests", "t-gc2-traverse.c"),
    t:path("tests", "t-m8-ffi-weak-newindex.c"),
    t:path("tests", "t-m8-close-finalizers.c"),
    t:path("tests", "t-m8-finalizer-state.c"),
    t:path("tests", "t-m8-finalizer-spawn-live.lua"),
    t:path("tests", "t-weak-modes.lua"),
    t:path("tests", "t-ffi-gc-finreg.lua")
  }
end

local function source_files(t)
  return t:files(t:path("src"), {
    extensions = { ".c", ".h", ".dasc" }
  })
end

local function compile_luajit_fixture(t, out, cfile, opts)
  opts = opts or {}
  t:cc(out, { t:path("tests", cfile) }, {
    cflags = opts.cflags,
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
end

local function run_c_fixtures(t, suffix, cflags)
  for _, name in ipairs({
    "t-gc2-phase",
    "t-gc2-traverse",
    "t-m8-ffi-weak-newindex",
    "t-m8-close-finalizers",
    "t-m8-finalizer-state"
  }) do
    local out = t:tmp("lj_" .. name .. suffix)
    compile_luajit_fixture(t, out, name .. ".c", { cflags = cflags })
    t:run({ out })
  end
end

local M8_MARKERS = lines([=[
gc2_weak_mayclear(global_State *g, cTValue *o, int val,
g->gc.state == GCSatomic && iswhite(gcV(o))
lj_gc2_barrier_weak_key(lua_State *L, GCtab *t
lj_gc2_barrier_weak_write(lua_State *L, GCtab *t
gc2_tab_weak_barrier_mode(global_State *g, GCtab *t)
use captured P_WEAK mode
test_weak_key_write_barrier
test_vm_weak_key_write_barrier
test_peer_weak_key_write_barrier
Peer TG performs the P_WEAK table write
test_jit_weak_table_store_helper_barrier
existing weak table stores trace through a P_WEAK-aware helper
test_jit_weak_array_store_helper_barrier
existing weak-value array stores trace through the array helper
test_vm_weak_value_hash_key_barrier
test_vm_weak_value_array_barrier
test_table_insert_weak_value_array_barrier
table.insert weak-value array write
lj_meta_tset_owner(lua_State *L, cTValue *o, cTValue *k,
test_capi_weak_newindex_target_write_barrier
test_capi_newindex_target_parent_barrier
lj_meta_tset_owner(L, tv, base+1, &owner)
test_ffi_weak_newindex_target_write_barrier
test_ffi_newindex_target_parent_barrier
t-m8-ffi-weak-newindex OK
ffi_register_module(lua_State *L)
ffi_loaded_store(lua_State *L, GCtab *t, GCstr *name,
FFI module registry saw FORWARD after lookup.
ffi_loaded_store(L, t, name, L->top-1);
lj_gc2_barrier_weak_write(L, t, &key, L->top-1);
test_ffi_loaded_weak_value_barrier
lib_weak_write_str(lua_State *L, GCtab *tab
lib_storefunc_str(lua_State *L, GCtab *tab, GCstr *key,
Library string store saw FORWARD after lookup.
lib_storetv_key(lua_State *L, GCtab *tab, cTValue *key,
Library generic store saw FORWARD after lookup.
lib_storetv_key(L, tab, L->top+1, L->top);
test_lib_register_weak_value_barrier
test_jit_profile_registry_weak_barrier
lj_gc2_barrier_weak_write(L, registry, &key, &tv);
lj_gc2_barrier_tv_pair(L, obj2gco(owner), o);
test_weak_clear_marks_string_slots
lj_obj_cleargcflags_atomic(gcV(o), LJ_GC_WHITES);
assert(!iswhite(obj2gco(modestr)));
test_weak_drain_uses_captured_mode
test_weak_pre_clear_late_write_survives_drain
P_WEAK late write before weak drain
GC2 late weak write mark wins over legacy white
test_weak_post_clear_resurrection_write
test_vm_weak_post_clear_existing_key_write
weak-kv table kept a one-cycle hash entry
test_capi_rawset_weak_write_barrier
C API raw hash weak write
C API raw array weak write
lj_gc2_barrier_weak_key(L, t, k);
weak-table key write
test_weak_complete_bridge
lj_gc2_weak_complete(g, gcref(g->gc.weak), 1)
gc2_weak_backfill_legacy(global_State *g, GCobj *legacy)
weak_legacy_backfills
weak_legacy_backfill_tables
int weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK
capture traversal-time weak mode
gc2_weak_paranoia_zero_diff(global_State *g, GCobj *legacy)
lj_gc2_finalizer_try_enter(global_State *g)
peer finalizer dispatch backs off
void *finalizer_tail
lj_gc_cdata_fin_pending(global_State *g)
!lj_gc_cdata_fin_pending(g)
gc_cdata_finalizer_candidate_close(gcV(&key))
gc_queue_cdata_finalizer(global_State *g, GCobj *o)
gc_cdata_finalizer_candidate_close(GCobj *o)
!(lj_obj_gcflags(o) & LJ_GC_FINALIZED)
gc_separate_cdata_finalizers(global_State *g)
lj_gc_finalize_cdata_disable(global_State *g)
m8_close_chain_cdata
cdata_finalized == 4
ALTERNATING_CLOSE_CHAIN
m8_close_chain_alternating_cdata
alternating_cdata_finalized == ALTERNATING_CLOSE_CHAIN
CLOSE_FINREG_GROWTH_BATCH
m8_close_register_growth_cdata
growth_cdata_finalized == CLOSE_FINREG_GROWTH_BATCH
close_error_cdata_finalizer
close_error_udata_finalizer
error_cdata_finalized == 1
after_error_cdata_finalized == 1
error_udata_finalized == 1
after_error_udata_finalized == 1
close_clear_suppressed_cdata_finalizer
m8_close_clear_suppressed_cdata
ffi.gc(keep.suppressed, nil)
suppressed_cdata_finalized == 0
close_nested_gc_cdata_finalizer
close_nested_gc_udata_finalizer
run_nested_collect
nested_gc_cdata_finalized == 1
nested_gc_udata_finalized == 1
m8_close_probe_shutdown_spawn
close_shutdown_spawn_cdata_finalizer
close_shutdown_spawn_udata_finalizer
VM shutdown in progress
shutdown_spawn_cdata_finalized == 1
shutdown_spawn_udata_finalized == 1
threading_rehome_unstarted_stack(lua_State *L, lua_State *L1,
if (la_load32_acq(&G(L)->mt_shutdown) != 0)
threading_rehome_unstarted_stack(L, L1, tg);
threading_thread___gc
threading_tg_is_registered(global_State *g, TGState *target)
lj_tg_fini_thread(g, th->tg);
lj_gc2_finreg_udata_register(L, g, obj2gco(ud));
order_cdata_finalized[0] == 3
order_cdata_finalized[1] == 2
order_cdata_finalized[2] == 1
lua_close drains alternating cdata/userdata finalizers to fixed point
lj_gc2_finalizer_queue_pending(global_State *g)
lj_gc2_finalizer_pending(global_State *g)
lj_gc2_finalizer_sweep_pending(global_State *g)
lj_gc2_finalizer_enqueue(global_State *g, GCobj *o)
lj_gc2_finalizer_drain(global_State *g)
lj_gc2_finalizer_dequeue(global_State *g)
void *finalizer_mpsc
uint64_t finalizer_queued
uint64_t finalizer_dequeued
uint64_t finalizer_mpsc_drained
la_casptr((void **)&g->gc2.finalizer_mpsc
la_xchgptr_acqrel((void **)&g->gc2.finalizer_mpsc
la_storeptr_rel((void **)&g->gc2.finalizer_tail
gc_mark_finalizers(global_State *g)
gc2_mark_finalizer_stack(global_State *g, GCobj *o)
gc2_mark_finalizer_ring(global_State *g, GCobj *tail)
g->gc2.finalizer_mpsc));
g->gc2.finalizer_tail));
test_finalizer_consumer_ring
test_finalizer_mpsc_concurrent_producers
finalizer_enqueue_worker
la_loadptr_acq((void *const *)&g->gc2.finalizer_tail) != NULL
finalizer_mpsc_drained) ==
assert(lj_gc2_finalizer_dequeue(g) == a)
assert(n == NTOTAL)
runtime finalizer queues must not use legacy mmudata
finalizer_queued0 = la_load64_acq(&g->gc2.finalizer_queued)
finalizer_dequeued0 = la_load64_acq(&g->gc2.finalizer_dequeued)
assert(!lj_gc2_finalizer_queue_pending(g))
assert(lj_gc2_finalizer_queue_pending(g))
assert(la_load32_acq(&g->gc2.finalizer_owner_tid) ==
finalizer_enters0 + 1u
finalizer_leaves0 + 1u
gc_finalizer_mt_release_exclusive(global_State *g)
gc_finalizer_mt_reclaim_exclusive(global_State *g)
gc_finalizer_restore_threshold(global_State *g, GCSize oldt)
gc_fullgc_deferred_by_finalizer(global_State *g)
uint64_t finalizer_spawn_deferrals
finalizer_spawn_deferrals, 0
test_finalizer_spawn_deferred_state
gc2_spawn_defer_t
finalizer_spawn_deferrals) > deferrals0
finrc <= 0
Keep GCSfinalize open until spawned TG exits.
finalizer-spawn outlived callback
oldt = la_load32_acq(&g->mt_live) != 0 ? lj_gc_mt_threshold_load(g) :
lj_gc_mt_threshold_store(g, oldt)
threshold == LJ_MAX_MEM && g->gc.state == GCSfinalize
collectgarbage("step", 1000000)
newproxy(true)
userdata explicit step
timeout 10s "$ROOT/src/luajit" "$ROOT/tests/t-m8-finalizer-spawn-live.lua"
GC step completed while finalizer-spawned worker was live
finalizer-spawned worker can outlive callback
worker finalizer notification timed out
worker finalizer fired more times than registered
finalized_by_thread[tid] == expected_by_thread[tid]
gc_finalize_cdata_slot_owned(lua_State *L, GCobj *o, cTValue *key)
lj_state_checkstack(cbL, 2+LJ_FR2+LUA_MINSTACK);
oldtop = savestack(cbL, cbL->top);
cbL->top = restorestack(cbL, oldtop);
lj_ctype_fin_get(L, cts, key, &t)
gc_queue_cdata_finalizers_pweak(lua_State *L, global_State *g)
gc_separate_cdata_finalizers_ordered(global_State *g,
FINREG ordered close-time cdata scan
typedef struct FinRegOrderNode
fin_order_head
FINREG ordered P_WEAK cdata scan
root unlink after ordered FINREG claim
queue claimed cdata in FINREG order
ordered FINREG identity is enough for P_WEAK
ordered FINREG identity is enough for close-time
gc2_rootless_order_fin_t
gc2_close_rootless_order_fin_t
uint64_t finreg_cdata_order_queued
uint64_t finreg_cdata_order_fallbacks
uint64_t finreg_cdata_pweak_root_fallbacks
uint64_t finreg_cdata_pending_order_hits
gc_cdata_fin_pending_ordered(global_State *g, CTState *cts)
FINREG ordered close-time cdata pending scan
finreg_cdata_pending_order_hits, 0
!gcref_acq(t->metatable)
ft == t && ft && gcref_acq(ft->metatable)
test_finreg_disabled_ordered_pending
gc2_disabled_pending_fin_t
collectgarbage('collect')
GCRef obj;
fin_order_obj_acq(FinRegOrderNode *ord)
fin_order_obj_rel(FinRegOrderNode *ord, GCobj *o)
fin_order_obj_clear(FinRegOrderNode *ord)
gc2_finreg_udata_obj_acq(GC2FinRegUDataNode *node)
gc2_finreg_udata_obj_rel(GC2FinRegUDataNode *node,
gc2_finreg_udata_obj_clear(GC2FinRegUDataNode *node)
gc2_finreg_cdata_preclaim_ready(global_State *g)
gc2_finreg_cdata_preclaim_obj_acq(global_State *g,
gc2_finreg_cdata_preclaim_fin_acq(global_State *g,
fin_order_obj_rel(ord, o);
fin_order_obj_acq(ord)
gc_order_cdata_object(FinRegOrderNode *ord, GCtab *t,
gc_marktv(g, &fin);
lj_gc2_finreg_cdata_preclaim(L, g, o, &fin)
lj_gc2_test_finreg_cdata_preclaim_fail(global_State *g,
Test-only side-vector failure injection.
gc2_order_fail_fin_t
finreg_cdata_preclaim_overflow) == overflow2 + 1u
finreg_cdata_pweak_claimed) == claimed2 + 2u
finreg_cdata_order_fallbacks) == orderfallback2 + 1u
gc_finalize_cdata_claim_preclaimed(global_State *g, GCobj *o)
P_WEAK preclaim suppressed by later ffi.gc(cd, nil)
gc2_preclaim_clear_fin_t
gc2_finclaim_publish(lua_State *L, global_State *g, MSize idx,
copyTVrel(L, &g->gc2.finreg_cdata_preclaim_fin[idx], fin);
setgcrefrel(g->gc2.finreg_cdata_preclaim_obj[idx], o);
finalizer value is visible before object ready marker
gc2_finclaim_clear(lua_State *L, global_State *g, MSize idx)
setgcrefnullrel(g->gc2.finreg_cdata_preclaim_obj[idx]);
gc2_finclaim_copy_slot(lua_State *L, GCRef *newobj, TValue *newfin,
lj_tv_load_acq(&fin, &oldfin[src]);
gc2_finclaim_next_capacity(cap, count + 1u)
lj_gc2_finreg_cdata_preclaim_take(L, g, o, &fin)
gc2_finreg_cdata_preclaim_fin_acq(g, i, fin)
dispatch order may differ from FINREG scan
gc_mark_finreg_cdata_preclaims(global_State *g)
gc2_mark_finreg_cdata_preclaims(global_State *g)
gc2_finreg_cdata_preclaim_fin_acq(g, i, &fin)
ordered FINREG P_WEAK cdata discovery
uint64_t finreg_cdata_pweak_queued
uint64_t finreg_cdata_sweep_queued
la_add64_rlx(&g->gc2.finreg_cdata_sweep_queued, 1)
uint64_t finreg_cdata_pweak_claimed
uint64_t finreg_cdata_preclaim_dispatched
finreg_cdata_pweak_queued) == pweak1 + 1u
finreg_cdata_pweak_claimed) == claimed1 + 1u
finreg_cdata_sweep_queued) ==
finreg_cdata_pweak_queued) == pweak2 + 3u
finreg_cdata_pweak_claimed) == claimed2 + 3u
finreg_cdata_order_queued) == orderq2 + 3u
finreg_cdata_order_fallbacks) ==
finreg_cdata_pweak_root_fallbacks) ==
finreg_cdata_pending_order_hits) == pendingorder2 + 1u
gc2_regorder_fin_t
gc2_cdata_order[1] == 1
gc2_rereg_fin_t
gc2_close_order_fin_t
lj_gc_finalize_cdata(L)
finreg_cdata_order_queued) == orderq2 + 2u
gc2_cdata_order[0] == 3
gc2_cdata_order[1] == 2
gc2_cdata_order[2] == 1
finalizer_dequeued) == finalizerd1 + 1u
finalizer_mpsc_drained) == mpscd1 + 1u
finalizer_mpsc_drained) == mpscd2 + 3u
finalizer_mpsc_drained) == mpscd2 + 2u
finreg_cdata_preclaim_dispatched) ==
gc2_cdata_bulk_finalizer
gc2_bulk_fin_t
finreg_cdata_preclaim_overflow) == overflow2
finreg_cdata_preclaim_capacity >= (MSize)bulk_n
gc2_cdata_finalized == finalized0 + 1
LJ_GC_UDATA_FINREG == LJ_GC_WEAKVAL
old | LJ_GC_UDATA_FINREG
void *finreg_udata_head
lj_gc2_finreg_udata_register_mt(lua_State *L, global_State *g,
lj_gc2_finreg_udata_register_mt(L, G(L), ud
debug.getmetatable(m).__gc = gc2_internal_udata_finalizer
immediate + lazy
ffi.C default namespace
lj_gc2_finreg_udata_register(lua_State *L, global_State *g,
GC2-owned userdata FINREG discovery
GC2 userdata FINREG identity is enough
test_unlink_udata_object
Side-list no-finalizer userdata is done
return gc_separateudata_registered(g, all);
lj_gc2_finreg_udata_set(g, o, 1);
lj_gc2_finreg_udata_set(g, o, 0);
gc_unlink_udata_object(global_State *g, GCobj *target)
gc_unlink_root_object(global_State *g, GCobj *target)
gc_chain_splice(GCRef *p, GCobj *o)
LA_ACQ_REL, LA_ACQ);
gc_chain_splice(p, o)
lj_gc_linkobj_after(GCobj *anchor, GCobj *o)
lj_gc_linkobj_after(obj2gco(mainthread(g)), obj2gco(ud));
lj_gc_linkobj_after(obj2gco(mainthread(g)), o);
test_finreg_internal_userdata_telemetry
finreg_udata_registered) == registered0 + 4u
finreg_udata_discovered) == discovered0 + 2u
sets0 + 4u
finreg_udata_queued) == queued0 + 2u
finreg_udata_clears) == clears0 + 4u
test_finreg_userdata_inplace_finalizer_behavior
gc2_counting_finalizer
09 section 9.6: finalizer may spawn while GC is paused.
lj_state_tryclaim(cbL, lj_thr_current_id(g), &claim)
lua_State *oldL
gc_call_finalizer must not use shared vmthread callback stack
]=])

local M8_MARKER_EXCEPTIONS = {
  ["gc_cdata_finalizer_candidate_close(gcV(&key))"] = true,
  ["runtime finalizer queues must not use legacy mmudata"] = true,
  ["timeout 10s \"$ROOT/src/luajit\" \"$ROOT/tests/t-m8-finalizer-spawn-live.lua\""] = true,
  ["gc_separate_cdata_finalizers_ordered(global_State *g,"] = true,
  ["finreg_cdata_preclaim_overflow) == overflow2 + 1u"] = true,
  ["finreg_cdata_order_fallbacks) == orderfallback2 + 1u"] = true,
  ["finreg_cdata_pending_order_hits) == pendingorder2 + 1u"] = true,
  ["finreg_cdata_preclaim_overflow) == overflow2"] = true,
  ["finreg_udata_registered) == registered0 + 4u"] = true,
  ["finreg_udata_discovered) == discovered0 + 2u"] = true
}

local function assert_source_markers(t)
  local files = source_and_test_files(t)
  for i = 1, #M8_MARKERS do
    local marker = M8_MARKERS[i]
    if not M8_MARKER_EXCEPTIONS[marker] then
      t:assert_any_contains(files, marker)
    end
  end

  local lj_gc = t:path("src", "lj_gc.c")
  local traverse = t:path("tests", "t-gc2-traverse.c")
  t:assert_contains(lj_gc, "gc_cdata_finalizer_candidate_close(GCobj *o)")
  t:assert_contains(lj_gc, "gc_separate_cdata_finalizers_ordered(global_State *g)")
  t:assert_text_ordered("ordered close-time FINREG scan", t:read(lj_gc), {
    "gc_separate_cdata_finalizers_ordered(global_State *g)",
    "gc_cdata_finalizer_candidate_close(o)"
  })
  t:assert_text_ordered("preclaim overflow increment", t:read(traverse), {
    "finreg_cdata_preclaim_overflow) ==",
    "overflow2 + 1u"
  })
  t:assert_text_ordered("FINREG order fallback increment", t:read(traverse), {
    "finreg_cdata_order_fallbacks) ==",
    "orderfallback2 + 1u"
  })
  t:assert_text_ordered("FINREG pending-order hit increment", t:read(traverse), {
    "finreg_cdata_pending_order_hits) ==",
    "pendingorder2 + 1u"
  })
  t:assert_text_ordered("FINREG preclaim overflow unchanged", t:read(traverse), {
    "finreg_cdata_preclaim_overflow) ==",
    "overflow2"
  })
  t:assert_text_ordered("userdata FINREG registration count", t:read(traverse), {
    "finreg_udata_registered) ==",
    "registered0 + 4u"
  })
  t:assert_text_ordered("userdata FINREG discovery count", t:read(traverse), {
    "finreg_udata_discovered) ==",
    "discovered0 + 2u"
  })
end

local function assert_preclaim_consumers(t)
  for _, item in ipairs({
    { t:path("src", "lj_gc.c"),
      "static void gc_mark_finreg_cdata_preclaims(global_State *g)" },
    { t:path("src", "lj_gc2.c"),
      "static void gc2_mark_finreg_cdata_preclaims(global_State *g)" },
    { t:path("src", "lj_gc2.c"),
      "int lj_gc2_finreg_cdata_preclaim_take(lua_State *L, global_State *g," }
  }) do
    local label = item[2]
    local block = t:c_block(item[1], item[2])
    assert_block_contains(label, block, "gc2_finreg_cdata_preclaim_ready(g)")
    if not (contains(block, "gc2_finreg_cdata_preclaim_obj_acq(g, i)") or
            contains(block, "gc2_finreg_cdata_preclaim_obj_acq(g, head)")) then
      error(label .. ": missing preclaim object acquire helper", 2)
    end
    if not (contains(block, "gc2_finreg_cdata_preclaim_fin_acq(g, i, &fin)") or
            contains(block, "gc2_finreg_cdata_preclaim_fin_acq(g, i, fin)")) then
      error(label .. ": missing preclaim finalizer acquire helper", 2)
    end
    assert_block_absent(label, block,
      "gcref_acq(g->gc2.finreg_cdata_preclaim_obj[")
    assert_block_absent(label, block,
      "lj_tv_load_acq(&fin, &g->gc2.finreg_cdata_preclaim_fin[")
  end
end

local function assert_chain_unlinks(t)
  local lj_gc = t:path("src", "lj_gc.c")
  local splice = t:c_block(lj_gc, "static int gc_chain_splice")
  if not splice:find("la_cas%d+%(&p%->gcptr") then
    error("gc_chain_splice must CAS-splice root/finalizer lists", 2)
  end

  for _, start in ipairs({
    "static int gc_unlink_udata_object(global_State *g, GCobj *target)",
    "static int gc_unlink_root_object(global_State *g, GCobj *target)"
  }) do
    local block = t:c_block(lj_gc, start)
    assert_block_contains(start, block, "gcref_acq(*p)")
    assert_block_contains(start, block, "gc_chain_splice(p, o)")
    assert_block_absent(start, block,
      "setgcrefr(*p, *lj_obj_gcwref(o))")
    assert_block_absent(start, block,
      "setgcrefrel(*p, *lj_obj_gcwref(o))")
  end

  assert_no_lines(t, "userdata chain publications must use lj_gc_linkobj_after()",
                  { t:path("src", "lj_gc.c"), t:path("src", "lj_udata.c") },
                  function(line)
    return contains(line, "setgcref(*lj_obj_gcwref(obj2gco(mainthread(g)))") or
           contains(line, "lj_obj_setgcwr(obj2gco(ud), *lj_obj_gcwref(obj2gco(mainthread(g)))") or
           contains(line, "lj_obj_setgcwr(o, *lj_obj_gcwref(obj2gco(mainthread(g)))")
  end)

  local link_after = t:c_block(lj_gc, "void lj_gc_linkobj_after")
  assert_block_contains("lj_gc_linkobj_after", link_after, "gcref_acq(*p)")
  if not link_after:find("la_cas%d+%(&p%->gcptr") then
    error("lj_gc_linkobj_after must CAS-insert after anchor", 2)
  end
end

local function assert_library_registration(t)
  local lj_lib = t:path("src", "lj_lib.c")
  local str = t:c_block(lj_lib, "static TValue *lib_storefunc_str")
  assert_block_absent("lib_storefunc_str", str, "lj_tab_storefunc(L, dst, fn)")
  assert_block_contains("lib_storefunc_str", str, "for (;;) {")
  assert_block_contains("lib_storefunc_str", str, "setfuncV(L, &tv, fn)")
  assert_block_contains("lib_storefunc_str", str, "lj_tab_setstr(L, tab, key)")
  assert_block_contains("lib_storefunc_str", str,
                        "lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK")
  assert_block_contains("lib_storefunc_str", str,
                        "Library string store saw FORWARD after lookup.")

  local generic = t:c_block(lj_lib, "static TValue *lib_storetv_key")
  assert_block_absent("lib_storetv_key", generic, "copyTVrel(L, dst, val)")
  assert_block_contains("lib_storetv_key", generic, "for (;;) {")
  assert_block_contains("lib_storetv_key", generic, "lj_tab_set(L, tab, key)")
  assert_block_contains("lib_storetv_key", generic,
                        "lj_tab_trystoretv_cas(L, dst, val) == LJ_TAB_STORE_CAS_OK")
  assert_block_contains("lib_storetv_key", generic,
                        "Library generic store saw FORWARD after lookup.")

  local read_lfunc = t:c_block(lj_lib, "static const uint8_t *lib_read_lfunc")
  assert_block_absent("lib_read_lfunc", read_lfunc,
                      "lj_tab_storefunc(L, lj_tab_setstr(L, tab, name), fn)")
  assert_block_contains("lib_read_lfunc", read_lfunc,
                        "lib_storefunc_str(L, tab, name, fn)")
  assert_block_contains("lib_read_lfunc", read_lfunc,
                        "lib_weak_write_str(L, tab, name, slot)")

  local register = t:c_block(lj_lib, "void lj_lib_register")
  assert_block_absent("lj_lib_register", register,
                      "lj_tab_storefunc(L, lj_tab_setstr(L, tab,")
  assert_block_absent("lj_lib_register", register,
                      "copyTVrel(L, lj_tab_set(L, tab, L->top+1), L->top)")
  assert_block_contains("lj_lib_register", register,
                        "lib_storefunc_str(L, tab, key, fn)")
  assert_block_contains("lj_lib_register", register,
                        "lib_storetv_key(L, tab, L->top+1, L->top)")
  assert_block_contains("lj_lib_register", register,
                        "lj_gc2_barrier_weak_write(L, tab, L->top+1, L->top)")
  assert_block_contains("lj_lib_register", register,
                        "lib_weak_write_str(L, tab, key, slot)")
end

local function assert_ffi_registration(t)
  local lib_ffi = t:path("src", "lib_ffi.c")
  local store = t:c_block(lib_ffi, "static TValue *ffi_loaded_store")
  assert_block_absent("ffi_loaded_store", store, "copyTVrel(L, dst, src)")
  assert_block_contains("ffi_loaded_store", store, "for (;;) {")
  assert_block_contains("ffi_loaded_store", store, "lj_tab_setstr(L, t, name)")
  assert_block_contains("ffi_loaded_store", store,
                        "lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK")
  assert_block_contains("ffi_loaded_store", store,
                        "FFI module registry saw FORWARD after lookup.")

  local register = t:c_block(lib_ffi, "static void ffi_register_module")
  assert_block_absent("ffi_register_module", register,
                      "copyTVrel(L, lj_tab_setstr(L, t, name), L->top-1)")
  assert_block_contains("ffi_register_module", register,
                        "ffi_loaded_store(L, t, name, L->top-1)")
  assert_block_contains("ffi_register_module", register,
                        "lj_gc2_barrier_weak_write(L, t, &key, L->top-1)")
  assert_block_contains("ffi_register_module", register, "lj_gc_pubtab(L, t)")
end

local function assert_finreg_preclaim_order(t)
  local lj_gc2 = t:path("src", "lj_gc2.c")
  local publish = t:c_block(lj_gc2, "static void gc2_finclaim_publish")
  t:assert_text_ordered("gc2_finclaim_publish", publish, {
    "copyTVrel(L, &g->gc2.finreg_cdata_preclaim_fin[idx], fin);",
    "setgcrefrel(g->gc2.finreg_cdata_preclaim_obj[idx], o);"
  })
end

local function assert_finalizer_dispatch(t)
  local lj_gc = t:path("src", "lj_gc.c")
  local call = t:c_block(lj_gc, "static int gc_call_finalizer")
  assert_no_lines(t, "gc_call_finalizer must not assign vmthread(g)",
                  { lj_gc }, function(line)
    return call:find(line, 1, true) and
           line:match("lua_State%s+%*[^=]+=%s*vmthread%(%s*g%s*%)")
  end)

  local close = t:c_block(t:path("src", "lj_state.c"),
                          "LUA_API void lua_close")
  t:assert_text_ordered("lua_close finalizer drain", close, {
    "lj_vm_cpcall(L, NULL, NULL, cpfinalize) == LUA_OK",
    "!lj_gc2_finalizer_queue_pending(g)",
    "!lj_gc_cdata_fin_pending(g)",
    "break;"
  })

  local cdata = t:c_block(lj_gc, "if (o->gch.gct == ~LJ_TCDATA)")
  assert_block_contains("cdata finalizer dispatch", cdata,
                        "gc_finalize_cdata_slot_owned(L, o, &key)")
  assert_block_absent("cdata finalizer dispatch", cdata,
                      "gc_call_finalizer(g, L,")

  local close_cdata = t:c_block(lj_gc, "void lj_gc_finalize_cdata")
  assert_block_absent("lj_gc_finalize_cdata", close_cdata,
                      "gc_call_finalizer(g, L,")
end

local function assert_source_predicates(t)
  assert_no_lines(t, "ordered FINREG object payload must use acquire/release helpers",
                  { t:path("src", "lj_ctype.c"), t:path("src", "lj_gc.c") },
                  function(line)
    return contains(line, "setgcref(ord->obj") or
           contains(line, "setgcrefnull(ord->obj") or
           contains(line, "gcref_acq(ord->obj")
  end)

  assert_no_lines(t, "userdata FINREG object payload must use acquire/release helpers",
                  { t:path("src", "lj_gc.c"), t:path("src", "lj_gc2.c") },
                  function(line)
    return contains(line, "gcref(node->obj") or
           contains(line, "gcref_acq(node->obj") or
           contains(line, "setgcref(node->obj") or
           contains(line, "setgcrefrel(node->obj") or
           contains(line, "setgcrefnull(node->obj") or
           contains(line, "setgcrefnullrel(node->obj")
  end)

  assert_preclaim_consumers(t)
  assert_chain_unlinks(t)
  assert_library_registration(t)
  assert_ffi_registration(t)

  assert_no_lines(t, "FINREG ordered discovery must not retain generation/root pending scans",
                  { t:path("src", "lj_gc.c") }, function(line)
    return contains(line, "gc_preclaim_cdata_finalizers_pweak_finreg") or
           contains(line, "gc_preclaim_cdata_finalizers_pweak_tab") or
           contains(line, "gc_cdata_finreg_pending_scan") or
           contains(line, "gc_cdata_fin_pending_tab") or
           contains(line, "gc_separate_cdata_finalizers_root") or
           contains(line, "gc_claim_cdata_finalizer_pweak") or
           line:match("finreg_cdata_pweak_root_fallbacks,%s*1") or
           line:match("ord%s*==%s*NULL")
  end)

  assert_finreg_preclaim_order(t)
  assert_finalizer_dispatch(t)

  assert_no_lines(t, "runtime finalizer queues must not use legacy mmudata",
                  source_files(t), function(line)
    return contains(line, "mmudata")
  end)
end

local function run_default_matrix(t)
  t:build({ clean = true, quiet = true })
  t:luajit({ "-joff", t:path("tests", "t-weak-modes.lua") })
  t:luajit({ t:path("tests", "t-weak-modes.lua") })
  t:luajit({ "-joff", t:path("tests", "t-m8-finalizer-spawn-live.lua") },
            { timeout = "10s" })
  t:luajit({ t:path("tests", "t-m8-finalizer-spawn-live.lua") },
            { timeout = "10s" })
  t:luajit({ "-joff", t:path("tests", "t-ffi-gc-finreg.lua"), "3", "72" })
  t:luajit({ t:path("tests", "t-ffi-gc-finreg.lua"), "3", "72" })
  run_c_fixtures(t, "_m8")
end

local function run_paranoia_matrix(t)
  local xcflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1"
  t:build({ clean = true, quiet = true, xcflags = xcflags })
  t:luajit({ "-joff", t:path("tests", "t-weak-modes.lua") }, {
    env = {
      LJ_M8_WEAK_RACE_ITERS = "0",
      LJ_M8_FINALIZER_SPAWN = "0"
    }
  })
  run_c_fixtures(t, "_m8_paranoia", xcflags)
end

return function(add)
  add({
    name = "m8_weak",
    description = "M8 weak-table/finalizer semantic gates",
    run = function(t)
      assert_source_markers(t)
      assert_source_predicates(t)
      run_default_matrix(t)
      run_paranoia_matrix(t)
      print("M8 weak/finalizer semantic gates passed")
    end
  })
end
