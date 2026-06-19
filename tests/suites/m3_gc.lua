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

local function make_clean(t)
  t:make({ "clean" }, { quiet = true, jobs = false })
end

local function make_default(t, opts)
  opts = opts or {}
  t:make(opts.args, { quiet = true, jobs = opts.jobs })
end

local function compile_luajit_fixture(t, out, cfile, opts)
  opts = opts or {}
  local libs = { "-lm", "-ldl" }
  if opts.pthread ~= false then
    libs[#libs + 1] = opts.pthread or "-pthread"
  end
  t:cc(out, { t:path("tests", cfile) }, {
    cflags = opts.cflags,
    link_luajit = true,
    libs = libs
  })
end

local function run_lua_test(t, name)
  t:run({ t:path("tools", "ci", "lua_test.sh"), name })
end

local function run_stock_tests(t, ...)
  local argv = {
    t:path("tools", "ci", "run_stock_tests.sh"),
    t:path("src", "luajit")
  }
  for i = 1, select("#", ...) do
    argv[#argv + 1] = select(i, ...)
  end
  t:run(argv)
end

local function run_case(cases, t, name)
  io.stderr:write("== " .. name .. " ==\n")
  cases[name].run(t)
  io.stderr:write("ok " .. name .. "\n")
end

local function gc2_scaffold_sources(t)
  return {
    t:path("src", "lj_gc.c"),
    t:path("src", "lj_gc.h"),
    t:path("src", "lj_gc2.c"),
    t:path("src", "lj_gc2.h"),
    t:path("src", "lj_obj.h"),
    t:path("src", "lj_atomic.h"),
    t:path("src", "lj_safepoint.c"),
    t:path("src", "lj_trace.c"),
    t:path("src", "lj_thr.c"),
    t:path("src", "lj_thr.h"),
    t:path("src", "lj_tg.c"),
    t:path("src", "lj_tg.h"),
    t:path("src", "lj_emit_x86.h"),
    t:path("src", "vm_x64.dasc"),
    t:path("src", "lj_tab.c"),
    t:path("src", "lj_cdata.c"),
    t:path("src", "lib_ffi.c"),
    t:path("src", "lj_api.c"),
    t:path("tests", "t-gc2-traverse.c")
  }
end

local GC2_SCAFFOLD_MARKERS = lines([=[
uint64_t fixpoint_rounds
uint64_t fixpoint_hits
uint64_t mark_complete_runs
uint64_t mark_complete_hits
uint64_t mark_complete_peer_waits
uint64_t mark_to_weak
uint64_t weak_complete_runs
uint64_t weak_complete_progress
uint64_t weak_to_sweep
uint64_t sweep_to_idle
uint64_t preserve_abort_to_idle
GCRef *weak_stack
uint8_t *weak_ready
uint64_t weak_count
uint64_t weak_tables_seen
uint64_t weak_tables_weakkey
uint64_t weak_tables_weakval
uint64_t weak_tables_allweak
uint64_t weak_tables_queued
uint64_t weak_tables_overflow
uint64_t weak_scan_cursor
uint64_t weak_scan_runs
uint64_t weak_scan_tables
uint64_t weak_scan_slots
uint64_t weak_scan_clearable
uint64_t weak_clear_cursor
uint64_t weak_clear_runs
uint64_t weak_clear_tables
uint64_t weak_clear_slots
uint64_t weak_clear_cleared
uint64_t weak_legacy_skipped
uint64_t weak_legacy_fallbacks
uint32_t worker_active
uint64_t worker_runs
uint64_t worker_grey_drained
uint64_t worker_ssb_converted
uint64_t worker_weak_drained
uint64_t worker_idle_declares
uint64_t worker_busy_retries
void *worker_thread
uint32_t n_workers
uint32_t worker_stop
uint32_t worker_wake
uint32_t worker_started
uint32_t worker_exited
uint64_t worker_wakes
uint64_t worker_parks
uint64_t worker_async_progress
uint64_t tg_thread_roots
uint64_t tg_cur_roots
uint64_t tg_trace_roots
uint64_t thread_scan_claims
uint64_t thread_scan_busy
uint64_t thread_scan_requeues
uint64_t thread_scan_owner_scans
uint64_t thread_scan_needscan
uint64_t thread_scan_owner_needscans
uint64_t thread_scan_dirty_misses
uint64_t sweep_owner_runs
uint64_t sweep_owner_arenas
uint64_t sweep_owner_live_cells
uint64_t sweep_live_updates
uint64_t sweep_live_huge_bytes
uint64_t live_estimate
uint64_t smr_reclaim_runs
uint64_t smr_reclaimed
LJ_GC2_WEAK_DRAIN_BATCH
LJ_GC2_WORKER_DRAIN_BATCH
LJ_GC2_SWEEP_BATCH
uint64_t finreg_cdata_sets
uint64_t finreg_cdata_clears
uint64_t finreg_cdata_queued
uint64_t finreg_cdata_sweep_queued
uint64_t finreg_udata_sets
uint64_t finreg_udata_clears
uint64_t finreg_udata_queued
uint32_t finalizer_active
uint32_t finalizer_owner_tid
uint64_t finalizer_enters
uint64_t finalizer_leaves
uint64_t finalizer_sweep_blocks
uint64_t finalizer_spawn_deferrals
uint64_t weak_keys_marked
uint64_t weak_values_marked
gc2_weak_record(global_State *g, GCtab *t)
gc2_weak_next_capacity(MSize cap, uint64_t need)
05 section 5.8 adaptive weak snapshot
lj_gc2_weak_snapshot_count(global_State *g)
lj_gc2_weak_snapshot_tab(global_State *g
lj_gc2_weak_snapshot_scan(global_State *g, uint32_t limit)
lj_gc2_weak_snapshot_clear(global_State *g, uint32_t limit)
lj_gc2_weak_drain(global_State *g, uint32_t limit)
lj_gc2_weak_snapshot_covers_legacy(global_State *g
lj_gc2_weak_legacy_result(global_State *g, int skipped)
gc2_queue_slot_store_rel(GCRef *slot, GCobj *o)
gc2_queue_slot_load_acq(const GCRef *slot)
gc2_queue_slot_clear_rel(GCRef *slot)
la_store8_rel(&g->gc2.weak_ready
la_load8_acq(&g->gc2.weak_ready
la_load64_acq(&g->gc2.weak_count)
la_load64_acq(&g->gc2.weak_clear_cursor)
la_cas64(&g->gc2.weak_scan_cursor
la_cas64(&g->gc2.weak_clear_cursor
lj_gc2_finreg_cdata_set(global_State *g, GCobj *o, int enabled)
gc2_finreg_queue_mark(global_State *g, GCobj *o)
05 section 5.8 FINREG resurrection
lj_gc2_finreg_cdata_queue(global_State *g, GCobj *o)
lj_gc2_finreg_cdata_set(g, obj2gco(cd), 1)
lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0)
lj_gc2_finreg_cdata_set(g, o, 0)
lj_gc2_finreg_cdata_queue(g, obj2gco(cd))
lj_gc2_finreg_udata_set(global_State *g, GCobj *o, int enabled)
lj_gc2_finreg_udata_register(lua_State *L, global_State *g,
lj_gc2_finreg_udata_queue(global_State *g, GCobj *o)
void *finreg_udata_head
lj_gc2_finreg_udata_set(g, obj2gco(ud), 1)
lj_gc2_finreg_udata_set(g, obj2gco(ud), 0)
lj_gc2_finreg_udata_queue(g, o)
gc2_weak_mayclear(global_State *g, cTValue *o, int val,
int markstr)
g->gc.state == GCSatomic && iswhite(gcV(o))
05 section 5.8: legacy-color weak oracle bridge
gc2_tab_is_ffi_fin(global_State *g, GCtab *t)
FFI finalizer registry is owned by FINREG
lj_gc2_markobj(g, gcV(o))
gc2_note_weak_table(global_State *g, GCtab *t, int weak)
lj_gc2_barrier_tvn_g(global_State *g, cTValue *tv
lj_gc2_legacy_weak_begin(global_State *g)
lj_gc2_barrier_weak_key(lua_State *L, GCtab *t
lj_gc2_barrier_weak_write(lua_State *L, GCtab *t
gc2_tab_weak_mode(global_State *g, GCtab *t
lj_gc2_fixpoint_round(global_State *g, lua_State *L
lj_gc2_fixpoint_run(global_State *g, lua_State *L
lj_gc2_mark_complete(global_State *g, lua_State *L
lj_gc2_mark_to_weak(global_State *g)
la_cas32(&g->gc2.phase, &expect, LJ_GC2_WEAK
lj_gc2_weak_complete(global_State *g, GCobj *legacy
lj_gc2_weak_to_sweep(global_State *g)
la_cas32(&g->gc2.phase, &expect, LJ_GC2_SWEEP
la_store32_rel(&g->gc2.phase, LJ_GC2_MARK)
phase = la_load32_acq(&g->gc2.phase)
la_xchg64_acqrel(&g->gc2.marks_this_round, 0)
05 section 5.7.1 scheduler-owned mark completion bridge
la_add64_rlx(&g->gc2.mark_to_weak
05 section 5.8 scheduler-owned weak completion bridge
la_add64_rlx(&g->gc2.weak_to_sweep
la_add64_rlx(&g->gc2.sweep_to_idle
la_add64_rlx(&g->gc2.preserve_abort_to_idle
LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB
gc2_traverse_trace(g, &J->cur)
05 section 5.7.4 current trace root
weakdrain = lj_gc2_worker_drain(g, drain_limit)
la_load32_acq(&g->gc2.worker_active) == 0
05 section 5.8: peer drain must finish before fallback
lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH)
05 section 5.6.3 bounded worker step bridge
lj_gc2_fixpoint_round(g, L, LJ_GC2_WORKER_DRAIN_BATCH)
05 section 5.7.1 bounded propagation fixpoint bridge
05 section 5.6.3 temporary single-worker bridge
la_cas32(&g->gc2.worker_active
la_store32_rel(&g->gc2.worker_active, 0)
la_add64_rlx(&g->gc2.worker_idle_declares
la_add64_rlx(&g->gc2.worker_busy_retries
lj_gc2_worker_drain(g
lj_gc2_worker_start(global_State *g)
lj_gc2_worker_stop(global_State *g)
lj_gc2_worker_wake(global_State *g)
static void *gc2_worker_main(void *arg)
la_futex_wait(&g->gc2.worker_wake, wake, -1)
la_futex_wake(&g->gc2.worker_wake, 1)
05 section 5.6.3 parked worker scheduler
lj_state_gcscan_claim(lua_State *L, LJStateClaim *claim)
lj_state_gcscan_claim(th, &claim)
gc2_thread_owner_scans(global_State *g, lua_State *th)
gc2_thread_set_needscan(global_State *g, lua_State *L)
gc2_scan_owned_needscan(global_State *g, lua_State *owner_L)
LJ_GC_NEEDSCAN
la_and8_rlx
05 section 5.7.2: owner scan or retry preserves work
la_store64_rel(&L->scan_epoch, g->gc2.cycle)
scan_epoch != g->gc2.cycle
uint64_t scan_dirty_epoch
la_store64_rel(&L->scan_dirty_epoch
la_load64_acq(&th->scan_dirty_epoch)
la_add64_rlx(&g->gc2.thread_scan_dirty_misses
DISPATCH_TG(stack_dirty_epoch)
la_add64_rlx(&tg->stack_dirty_epoch
la_add64_rlx(&g->gc2.thread_scan_needscan
la_add64_rlx(&g->gc2.thread_scan_owner_needscans
lj_tg_load_thread_L(tg)
lj_tg_load_cur_L(tg)
la_add64_rlx(&g->gc2.tg_thread_roots
la_add64_rlx(&g->gc2.tg_cur_roots
int32_t vmstate
tg->vmstate = ~LJ_VMST_INTERP
la_load32_acq((uint32_t *)&tg->vmstate)
la_add64_rlx(&g->gc2.tg_trace_roots
emit_movmroi(as, RID_DISPATCH, DISPATCH_TG(vmstate)
mov dword [DISPATCH+DISPATCH_TG(vmstate)]
mov RAd, dword [DISPATCH+DISPATCH_TG(vmstate)]
mt = gcref_acq(L->mt_thread)
la_add64_rlx(&g->gc2.thread_scan_claims
la_add64_rlx(&g->gc2.thread_scan_busy
la_add64_rlx(&g->gc2.thread_scan_requeues
la_add64_rlx(&g->gc2.thread_scan_owner_scans
05 section 5.6.3 total worker progress contract
phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK &&
la_add64_rlx(&g->gc2.mark_complete_peer_waits
gc2_worker_sweep_progress(global_State *g, uint32_t limit)
sweep = gc2_worker_sweep_progress(g, limit)
lj_gc2_worker_drain(g, LJ_GC2_SWEEP_BATCH)
05 section 5.6.3 worker-owned sweep bridge
weak = lj_gc2_weak_drain(g, limit - work)
la_add64_rlx(&g->gc2.worker_weak_drained
gc_arena_sweep_pending(global_State *g)
la_loadptr_acq((void *const *)&g->gc2.tg_list)
lj_gc2_sweep_tg_ready(TGState *tg)
lj_gc2_sweep_needs_prepare(global_State *g)
lj_gc2_sweep_pending(global_State *g)
lj_gc2_sweep_to_idle(global_State *g)
lj_gc2_finalizer_queue_pending(global_State *g)
lj_gc2_finalizer_pending(global_State *g)
lj_gc2_finalizer_sweep_pending(global_State *g)
void *finalizer_tail
gc2_mark_finalizer_stack(global_State *g, GCobj *o)
gc2_mark_finalizer_ring(global_State *g, GCobj *tail)
gc2_sweep_blocked_by_finalizer(global_State *g)
lj_gc2_sweep_owner_progress(global_State *g, TGState *tg
la_add64_rlx(&g->gc2.sweep_owner_arenas
la_add64_rlx(&g->gc2.finalizer_sweep_blocks
la_cas32(&g->gc2.worker_active, &expect, 1
lj_gc2_sweep_live_aggregate(global_State *g)
lj_arena_hugetab_live_bytes(&tg->huge
LJ_HUGEF_MARK|LJ_HUGEF_TRAVERSABLE
la_store64_rel(&g->gc2.sweep_live_huge_bytes
la_store64_rel(&g->gc2.live_estimate
la_add64_rlx(&g->gc2.sweep_live_updates
gc2_live = la_load64_acq(&g->gc2.live_estimate)
05 section 5.8 boundary-lazy traversable sweep bridge
lj_gc2_reclaim_retired(global_State *g, uint64_t epoch)
la_add64_rlx(&g->gc2.smr_reclaimed
lj_gc2_reclaim_retired(g, epoch)
lj_gc2_paranoia_legacy_diff(global_State *g)
lj_gc2_ssb_empty(g)
la_loadptr_acq((void *const *)&tg->ssb_next)
la_storeptr_rel((void **)&tg->ssb_next
lj_gc2_mark_complete(g, L, 64, ~(uint32_t)0)
lj_gc2_weak_complete(g, gcref(g->gc.weak), LJ_GC2_WEAK_DRAIN_BATCH)
gc_clearweak(g, gcref(g->gc.weak))
05 section 5.8 conditional legacy weak fallback
]=])

local WORKER_SCHEDULER_MARKERS = lines([=[
void *worker_thread
uint32_t n_workers
uint32_t worker_stop
uint32_t worker_wake
uint32_t worker_started
uint32_t worker_exited
uint64_t worker_wakes
uint64_t worker_parks
uint64_t worker_async_progress
uint32_t finalizer_active
uint32_t finalizer_owner_tid
uint64_t finalizer_sweep_blocks
lj_gc2_worker_start(global_State *g)
lj_gc2_worker_stop(global_State *g)
lj_gc2_worker_wake(global_State *g)
LUA_GCWORKERS
\5stats\7workers
lj_gc2_worker_start(g)
lj_gc2_worker_stop(g)
collectgarbage("workers", 1)
lj_gc2_finalizer_pending(global_State *g)
lj_gc2_finalizer_sweep_pending(global_State *g)
static void *gc2_worker_main(void *arg)
la_futex_wait(&g->gc2.worker_wake, wake, -1)
la_futex_wake(&g->gc2.worker_wake, 1)
05 section 5.6.3 parked worker scheduler
assert(lj_gc2_worker_start(g) == 1)
test_async_sweep_and_stop
wait_until_marked
assert(lj_gc2_finalizer_pending(g))
lj_gc2_worker_wake(g);
]=])

local PARANOIA_MARKERS = lines([=[
gc_arena_verify_sweep_boundary(global_State *g)
gc2_paranoia_check_roots(global_State *g)
gc2_legacy_has_base(global_State *g, void *p)
for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o))
]=])

local SAFEPOINT_SOURCE_MARKERS = lines([=[
TGState *self = lj_thr_get_tg()
Leader self-ack is a real poll
remote native ack
lj_gc2_reclaim_retired(g, epoch)
]=])

local TG_ATTACH_MARKERS = lines([=[
uint32_t phase = la_load32_acq(&g->gc2.phase)
phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK
phase == LJ_GC2_SWEEP
assert_attach_phase(L, g, tg, LJ_GC2_WEAK, 1, 1)
assert_attach_phase(L, g, tg, LJ_GC2_SWEEP, 0, 1)
]=])

local LIB_OS_NATIVE_MARKERS = lines([=[
lj_safepoint_checkstop(L, lj_native_leave(L));
lj_safepoint_checkstop(L, actions);
]=])

local LIB_OS_TMPNAME_MARKERS = lines([=[
os_native_mkstemp(lua_State *L, char *buf)
actions = lj_native_leave(L);
remove(buf);
lj_safepoint_checkstop(L, actions);
]=])

local LIB_IO_FFLUSH_MARKERS = lines([=[
io_native_fflush(lua_State *L, FILE *fp)
lj_safepoint_checkstop(L, lj_native_leave(L));
]=])

local LIB_IO_FOPEN_MARKERS = lines([=[
io_native_fopen(lua_State *L, const char *fname,
io_fresh_stopreq(lua_State *L, uint32_t actions, int had_stopreq)
io_fopen_checkstop(lua_State *L, IOFileUD *iof, uint32_t actions,
int had_stopreq)
(!had_stopreq && tg && (la_load8_acq(&tg->tg_flags) & TGF_STOPREQ));
int had_stopreq = tg && (la_load8_acq(&tg->tg_flags) & TGF_STOPREQ);
iof->fp = io_native_fopen(L, fname, mode, &actions);
io_fopen_checkstop(L, iof, actions, had_stopreq);
(void)fclose(fp);
lj_safepoint_checkstop(L, actions);
]=])

local LIB_IO_PCLOSE_MARKERS = lines([=[
io_native_pclose(lua_State *L, FILE *fp, uint32_t *actionsp)
*actionsp = lj_native_leave(L);
stat = io_native_pclose(L, iof->fp, &actions);
io_checkstop_fresh(L, actions, had_stopreq);
]=])

local LIB_IO_POPEN_MARKERS = lines([=[
io_native_popen(lua_State *L, const char *fname,
const char *mode, uint32_t *actionsp)
*actionsp = lj_native_leave(L);
iof->fp = io_native_popen(L, fname, mode, &actions);
if (io_fresh_stopreq(L, actions, had_stopreq))
iof->fp = NULL;
(void)io_native_pclose(L, fp, &close_actions);
actions |= close_actions;
lj_safepoint_checkstop(L, actions);
]=])

local LIB_IO_FWRITE_MARKERS = lines([=[
io_native_fwrite(lua_State *L, const void *buf, size_t size,
size_t n, FILE *fp, uint32_t *actionsp)
*actionsp = lj_native_leave(L);
io_native_fwrite(L, p, 1, len, fp, &actions)
io_checkstop_fresh(L, actions, had_stopreq);
]=])

local LIB_IO_FCLOSE_MARKERS = lines([=[
io_native_fclose(lua_State *L, FILE *fp, uint32_t *actionsp)
*actionsp = lj_native_leave(L);
ok = (io_native_fclose(L, iof->fp, &actions) == 0);
iof->fp = NULL;
io_checkstop_fresh(L, actions, had_stopreq);
]=])

local LIB_IO_READ_MARKERS = lines([=[
io_native_fscanf_num(lua_State *L, FILE *fp, lua_Number *dp)
io_native_fgets(lua_State *L, char *buf, int size, FILE *fp)
io_native_fread(lua_State *L, void *buf, size_t size,
io_native_getc(lua_State *L, FILE *fp, uint32_t *actionsp)
*actionsp = lj_native_leave(L);
lj_safepoint_checkstop(L, actions);
lj_safepoint_checkstop(L, lj_native_leave(L));
]=])

local LIB_IO_TMPFILE_MARKERS = lines([=[
LJLIB_CF(io_tmpfile)
actions = lj_native_leave(L);
(void)fclose(fp);
lj_safepoint_checkstop(L, actions);
]=])

local LIB_IO_SEEK_MARKERS = lines([=[
LJLIB_CF(io_method_seek)
res = fseeko(fp, ofs, opt);
ofs = ftello(fp);
lj_safepoint_checkstop(L, actions);
]=])

local LJ_CCALL_MARKERS = lines([=[
actions = lj_native_leave(L);
lj_safepoint_checkstop(L, actions);
]=])

local LJ_CCALLBACK_MARKERS = lines([=[
actions = lj_native_leave(L);
if (actions & LJ_GC2_HS_STOPREQ)
callback_frame_top(cb)->was_native = 0;
lj_safepoint_checkstop(L, actions);
]=])

local SAFEPOINT_OS_COVERAGE_MARKERS = lines([=[
publish_stopreq()
mkfifo_test(fifo)
start_fifo_stopreq(fifo)
join_fifo_stopreq()
start_native_stopreq()
join_native_stopreq()
os.execute(':')
os.tmpname()
io.tmpfile()
io.open(fifo, 'r')
io.lines(fifo)
f:flush()
f:seek('set', 0)
f:read('*n')
f:read('*l')
f:read(1)
f:read('*a')
f:read(0)
io.popen('sleep 0.2', 'r')
pipe:close()
sh -c 'sleep 0.2; cat >/dev/null'
write_pipe:write(big)
thread interrupted: VM shutdown
]=])

local SAFEPOINT_FFI_COVERAGE_MARKERS = lines([=[
ffi_stopreq_ptr
ffi_call_callback_stopreq_ptr
ffi.cast('stopreq_t', ffi_stopreq_ptr)
return stopreq()
ffi.cast('call_cb_stopreq_t', ffi_call_callback_stopreq_ptr)
assert(not entered)
]=])

return function(add)
  local cases = {}

  local function register(test)
    cases[test.name] = test
    add(test)
  end

  register({
    name = "m3_gc2_worker_scheduler",
    description = "staged GC2 parked-worker scheduler guard and fixtures",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })

      t:assert_all_any_contains({
        t:path("src", "lj_gc2.c"),
        t:path("src", "lj_gc2.h"),
        t:path("src", "lj_obj.h"),
        t:path("src", "lib_base.c"),
        t:path("tests", "t-gc2-worker-scheduler.c"),
        t:path("tests", "t-gc-workers.lua")
      }, WORKER_SCHEDULER_MARKERS)

      compile_luajit_fixture(t, t:tmp("lj_t-gc2-worker-scheduler"),
                             "t-gc2-worker-scheduler.c")
      t:run({ t:tmp("lj_t-gc2-worker-scheduler") })
      t:luajit({ "-joff", t:path("tests", "t-gc-workers.lua") })
      t:luajit({ t:path("tests", "t-gc-workers.lua") })

      print("M3 GC2 worker scheduler test passed")
    end
  })

  register({
    name = "m3_safepoint_handshake",
    description = "C-level safepoint handshake guard and fixture",
    run = function(t)
      local pthread = os.getenv("PTHREAD") or "-pthread"
      local lib_io = t:path("src", "lib_io.c")

      make_clean(t)
      make_default(t)
      compile_luajit_fixture(t, t:tmp("lj_t_safepoint_handshake"),
                             "t-safepoint-handshake.c", {
        cflags = pthread,
        pthread = pthread
      })
      t:run({ t:tmp("lj_t_safepoint_handshake") })

      t:assert_all_any_contains({
        t:path("src", "lj_safepoint.c")
      }, SAFEPOINT_SOURCE_MARKERS)
      t:assert_not_contains(t:path("src", "lj_safepoint.c"),
                            "Deterministic single-mutator scaffold")
      t:assert_all_any_contains({
        t:path("src", "lj_tg.c"),
        t:path("tests", "t-safepoint-handshake.c")
      }, TG_ATTACH_MARKERS)
      t:assert_all_any_contains({ t:path("src", "lib_os.c") },
                                LIB_OS_NATIVE_MARKERS)
      t:assert_all_any_contains({ t:path("src", "lib_os.c") },
                                LIB_OS_TMPNAME_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_FFLUSH_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_FOPEN_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_PCLOSE_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_POPEN_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_FWRITE_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_FCLOSE_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_READ_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_TMPFILE_MARKERS)
      t:assert_all_any_contains({ lib_io }, LIB_IO_SEEK_MARKERS)
      t:assert_all_any_contains({ t:path("src", "lj_ccall.c") },
                                LJ_CCALL_MARKERS)
      t:assert_all_any_contains({ t:path("src", "lj_ccallback.c") },
                                LJ_CCALLBACK_MARKERS)
      t:assert_all_any_contains({ t:path("tests", "t-safepoint-handshake.c") },
                                SAFEPOINT_OS_COVERAGE_MARKERS)
      t:assert_all_any_contains({ t:path("tests", "t-safepoint-handshake.c") },
                                SAFEPOINT_FFI_COVERAGE_MARKERS)

      print("M3 safepoint handshake tests passed")
    end
  })

  register({
    name = "m3_vm_safepoint",
    description = "focused x64 VM safepoint poll fixture",
    run = function(t)
      make_clean(t)
      make_default(t)
      compile_luajit_fixture(t, t:tmp("lj_t_vm_safepoint"),
                             "t-vm-safepoint.c")
      t:run({ t:tmp("lj_t_vm_safepoint") }, { timeout = "20s" })
    end
  })

  register({
    name = "m3_gc2_paranoia",
    description = "GC2 paranoia build, oracle fixtures, and stock tests",
    run = function(t)
      local gc_sources = {
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c")
      }

      t:assert_all_any_contains(gc_sources, PARANOIA_MARKERS)
      assert_no_lines(t, "GC2 diagnostic root walks must acquire-load root links",
                      gc_sources, function(line)
        return contains(line,
          "for (o = gcref(g->gc.root); o != NULL; o = gcnext(o))")
      end)

      make_clean(t)
      make_default(t, {
        args = { "XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" }
      })
      for _, name in ipairs({
        "t-gc2-paranoia",
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      }) do
        local out = t:tmp("lj_" .. name .. "_paranoia")
        compile_luajit_fixture(t, out, name .. ".c", {
          cflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1"
        })
        t:run({ out })
      end
      run_stock_tests(t, "--quiet")

      make_clean(t)
      make_default(t, {
        args = {
          "BUILDMODE=static",
          "XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -DLUAJIT_DISABLE_JIT"
        }
      })
      run_stock_tests(t, "--quiet", "-jit")
    end
  })

  register({
    name = "m3_gc2_scaffold",
    description = "focused M3 GC2 scaffold tests and dependent gates",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })

      t:assert_all_any_contains(gc2_scaffold_sources(t), GC2_SCAFFOLD_MARKERS)
      assert_no_lines(t, "GC2 queue slots must use acquire/release helpers",
                      { t:path("src", "lj_gc2.c") }, function(line)
        return contains(line, "setgcref(g->gc2.grey_stack") or
               contains(line, "setgcref(g->gc2.weak_stack") or
               contains(line, "setgcref(*next") or
               contains(line, "setgcrefnull(*slot") or
               contains(line, "gcref(g->gc2.grey_stack") or
               contains(line, "gcref(g->gc2.weak_stack") or
               contains(line, "gcref(*slot)") or
               (contains(line, "g->gc2.grey_stack[") and
                contains(line, "] = oldstack"))
      end)

      for _, name in ipairs({
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      }) do
        local out = t:tmp("lj_" .. name)
        compile_luajit_fixture(t, out, name .. ".c")
        t:run({ out })
      end

      run_case(cases, t, "m3_gc2_worker_scheduler")
      run_case(cases, t, "m3_safepoint_handshake")
      run_case(cases, t, "m3_vm_safepoint")
      run_case(cases, t, "m3_gc2_paranoia")
      run_lua_test(t, "m2_arena_all")

      make_clean(t)
      make_default(t, { jobs = false })
      make_clean(t)
      t:make({ "amalg" }, { quiet = true, jobs = false })

      t:run({ t:path("tools", "ci", "m0_matrix.sh") })
      print("M3 GC2 scaffold tests passed")
    end
  })
end
