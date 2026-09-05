/* Fixture-only observation. Never calls a collector, installs work or enters
** an ownership gate. Global fields are individual atomic samples, not one
** coherent state. Only the current actor's private SSB slots are followed;
** remote TG private lists/bodies are inspected only with all threads stopped
** in the separate debugger diagnostic. */
#include "lj_thr.h"
global_State *sweep_probe_global;
uint64_t sweep_probe_ticks;
uint32_t sweep_probe_round;
static uint32_t sweep_probe_last_phase = ~(uint32_t)0;
static uint32_t sweep_probe_last_cycle = ~(uint32_t)0;

__attribute__((noinline)) void sweep_probe_checkpoint(const char *where)
{
  __asm__ __volatile__("" : : "r"(where) : "memory");
}

static void sweep_probe_report(lua_State *L, const char *where)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCRef *base = lj_tg_ssb_base_acq(tg);
  GCRef *next = lj_tg_ssb_next_acq(tg);
  uint32_t phase = gc2_phase_acq(g), cycle = gc2_cycle_acq(g);
  uint32_t count = base && next && next >= base && (size_t)(next - base) <= TG_GC2_SSB_SLOTS ?
                   (uint32_t)(next - base) : ~(uint32_t)0;
  uint32_t i;
  sweep_probe_global = g;
  printf("{\"probe\":\"%s\",\"tick\":%" PRIu64 ",\"round\":%u,"
         "\"g\":\"%p\",\"own_tg\":\"%p\",\"own_tid\":%u,"
         "\"target\":%" PRIu64 ",\"phase_before\":%u,\"cycle_before\":%u",
         where, sweep_probe_ticks, sweep_probe_round, (void *)g, (void *)tg,
         tg->tid, automatic_target, phase, cycle);
#define PROBE32(n) printf(",\"" #n "\":%u", la_load32_acq(&g->gc2.n))
#define PROBE64(n) printf(",\"" #n "\":%" PRIu64, la_load64_acq(&g->gc2.n))
#define PROBEP(n) printf(",\"" #n "\":\"%p\"", la_loadptr_acq((void *const *)&g->gc2.n))
  PROBE64(cycle_starts); PROBE64(sweep_to_idle); PROBE32(cycle_leader);
  PROBE32(n_workers); PROBE32(worker_active); PROBE64(worker_runs);
  PROBE64(worker_async_progress); PROBE64(worker_idle_declares);
  PROBE64(worker_busy_retries); PROBE64(worker_parks); PROBE64(deferred_epoch);
  PROBE32(sweep_bridge_ready); PROBE32(sweep_root_scanned);
  PROBE32(sweep_root_done); PROBEP(sweep_root_cursor);
  PROBE32(sweep_grace_needed); PROBE64(sweep_owner_runs);
  PROBE64(sweep_owner_arenas); PROBE64(sweep_owner_live_cells);
  PROBE32(jit_phase_gate); PROBE32(jit_mark_resume);
  PROBE32(jit_sweep_displaced); PROBE64(jit_sweep_yield_until_ns);
  PROBE64(hs_epoch); PROBE32(hs_pending); PROBE32(hs_actions); PROBE32(hs_leader);
  PROBEP(ssb_head); PROBEP(ssb_drain); PROBE32(ssb_consumer_active);
  PROBE32(ssb_published); PROBE32(ssb_drained);
  PROBE64(ssb_items_published); PROBE64(ssb_items_drained);
  PROBE64(grey_top); PROBE64(grey_bottom); PROBE64(grey_pushed);
  PROBE64(grey_drained); PROBE64(marks_this_round);
  PROBE64(recovery_items); PROBE64(recovery_huge_items);
  PROBE32(recovery_failed); PROBE64(recovery_published); PROBE64(recovery_drained);
  PROBE32(thread_scan_needscan_pending); PROBE32(table_rescan_pending);
  PROBE32(smr_readers); PROBE32(smr_reclaiming);
  PROBE32(weak_drain_active); PROBE32(weak_write_active);
  PROBE32(finalizer_active);
  printf(",\"finalizer_owner\":%u", gc2_finalizer_owner_acq(g));
  PROBE32(finalizer_spawn_latch); PROBEP(finalizer_mpsc);
  PROBE64(alloc_since_trigger); PROBE64(trigger_bytes); PROBE64(hard_bytes);
#undef PROBE32
#undef PROBE64
#undef PROBEP
  printf(",\"own_native\":%u,\"own_ssb_active\":\"%p\","
         "\"own_ssb_base\":\"%p\",\"own_ssb_next\":\"%p\","
         "\"own_ssb_count\":%u,\"own_ssb_refs\":%u,"
         "\"own_reqmask\":%u,\"own_hs_ack\":%" PRIu64 ","
         "\"own_local_total\":%" PRIu64 ",\"own_flags\":%u,"
         "\"own_traversable_owned_count\":%u,\"own_traversable_needsweep_count\":%u,"
         "\"own_strtab_depth\":%u,\"own_strq_depth\":%u,\"own_tab_read_depth\":%u,"
         "\"auto_flags\":%u,\"phase_after\":%u,\"cycle_after\":%u,\"own_ssb_slots\":[",
         lj_tg_in_native_acq(tg), (void *)lj_tg_ssb_active_acq(tg),
         (void *)base, (void *)next, count, la_load32_acq(&tg->ssb_refs),
         la_load32_acq(&tg->reqmask), la_load64_acq(&tg->hs_epoch_ack),
         lj_tg_local_total_acq(tg), (unsigned)lj_tg_flags_acq(tg),
         la_load32_acq(&tg->alloc.owned_count[0]),
         la_load32_acq(&tg->alloc.needsweep_count[0]),
         la_load32_acq(&tg->strtab_active_depth),
         la_load32_acq(&tg->strq_active_depth),la_load32_acq(&tg->tab_read_depth),
         lj_gc_auto_flags_load(g), gc2_phase_acq(g), gc2_cycle_acq(g));
  for (i = 0; i < count && i < TG_GC2_SSB_SLOTS; i++)
    printf("%s\"%p\"", i ? "," : "", (void *)gcref(base[i]));
  puts("]}");
  sweep_probe_checkpoint(where);
}

static void sweep_probe_auto(lua_State *L)
{
  uint32_t phase = gc2_phase_acq(G(L));
  uint32_t cycle = gc2_cycle_acq(G(L));
  ++sweep_probe_ticks;
  if (phase != sweep_probe_last_phase || cycle != sweep_probe_last_cycle ||
      sweep_probe_ticks % 9u == 1u) {
    sweep_probe_report(L, "automatic_check");
    sweep_probe_last_phase = phase;
    sweep_probe_last_cycle = cycle;
  }
}
