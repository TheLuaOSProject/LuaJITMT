/*
** Focused dormant jit.attach() publication-clock substrate regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lj_vmevent.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "t-jit-attachment-clock requires LJ_GC2_TEST_HELPERS"
#endif

static void expect_snapshot_zero(const LJJitEventAttachmentSnapshot *snapshot)
{
  assert(snapshot->sequence == 0);
  assert(snapshot->next_generation == 0);
  assert(snapshot->generation == 0);
}

#if LJ_HASJIT
static void clock_store(LJJitEventAttachmentClock *clock, uint64_t sequence,
			uint64_t next_generation, uint64_t generation)
{
  la_store64_rel(&clock->next_generation, next_generation);
  la_store64_rel(&clock->generation, generation);
  la_store64_rel(&clock->sequence, sequence);
}

static void expect_clock_bytes_zero(const TGState *tg)
{
  uint32_t slot;
  for (slot = 0; slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS; slot++) {
    const LJJitEventAttachmentClock *clock = &tg->jit_event_attachment[slot];
    assert(la_load64_acq(&clock->sequence) == 0);
    assert(la_load64_acq(&clock->next_generation) == 0);
    assert(la_load64_acq(&clock->generation) == 0);
  }
}

static void expect_retry(global_State *g, uint32_t slot)
{
  LJJitEventAttachmentSnapshot snapshot;
  memset(&snapshot, 0xa5, sizeof(snapshot));
  assert(lj_jit_event_attachment_snapshot(g, slot, &snapshot) ==
	 LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY);
  expect_snapshot_zero(&snapshot);
}

static void expect_initial(global_State *g, uint32_t slot)
{
  LJJitEventAttachmentSnapshot snapshot;
  memset(&snapshot, 0xa5, sizeof(snapshot));
  assert(lj_jit_event_attachment_snapshot(g, slot, &snapshot) ==
	 LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_INITIAL);
  expect_snapshot_zero(&snapshot);
}

static void expect_published(global_State *g, uint32_t slot,
			     uint64_t sequence, uint64_t generation)
{
  LJJitEventAttachmentSnapshot snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  assert(lj_jit_event_attachment_snapshot(g, slot, &snapshot) ==
	 LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_PUBLISHED);
  assert(snapshot.sequence == sequence);
  assert(snapshot.next_generation == generation);
  assert(snapshot.generation == generation);
}

static void test_snapshot_shape(global_State *g)
{
  LJJitEventAttachmentClock *clock = &g->main_tg->jit_event_attachment[3];

  expect_initial(g, 3);

  /* Odd is an in-progress writer, including the terminal odd value. */
  clock_store(clock, 1, 0, 0);
  expect_retry(g, 3);
  clock_store(clock, UINT64_MAX, UINT64_MAX, UINT64_MAX);
  expect_retry(g, 3);

  /* A completed publication must advance both identities together. */
  clock_store(clock, 2, 2, 1);
  expect_retry(g, 3);
  clock_store(clock, 2, 0, 0);
  expect_retry(g, 3);
  clock_store(clock, 0, 1, 1);
  expect_retry(g, 3);

  clock_store(clock, 2, 1, 1);
  expect_published(g, 3, 2, 1);

  /* Final even sequence and generation values remain observable.  Future
  ** writers refuse before attempting to wrap either scalar. */
  clock_store(clock, UINT64_MAX - 1u, UINT64_MAX, UINT64_MAX);
  expect_published(g, 3, UINT64_MAX - 1u, UINT64_MAX);

  clock_store(clock, 0, 0, 0);
  expect_initial(g, 3);
}

static void test_main_only_authority(global_State *g)
{
  TGState *secondary = (TGState *)calloc(1, sizeof(*secondary));
  LJJitEventAttachmentClock *main_clock;
  LJJitEventAttachmentClock *secondary_clock;
  assert(secondary != NULL);
  lj_jit_event_sessions_init(secondary);
  expect_clock_bytes_zero(secondary);

  main_clock = &g->main_tg->jit_event_attachment[5];
  secondary_clock = &secondary->jit_event_attachment[5];

  /* A valid-looking secondary clock is never a universe authority. */
  clock_store(secondary_clock, 2, 41, 41);
  expect_initial(g, 5);

  /* Conversely, corrupt secondary storage cannot poison the main clock. */
  clock_store(main_clock, 4, 42, 42);
  clock_store(secondary_clock, 3, 7, 7);
  expect_published(g, 5, 4, 42);

  clock_store(main_clock, 0, 0, 0);
  free(secondary);
}

static void run_jit_tests(lua_State *L)
{
  global_State *g = G(L);
  uint32_t slot;
  assert(g->main_tg != NULL);
  assert(LJ_JIT_EVENT_ATTACHMENT_SLOTS == 8u);
  expect_clock_bytes_zero(g->main_tg);

  for (slot = 0; slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS; slot++)
    expect_initial(g, slot);
  expect_retry(g, LJ_JIT_EVENT_ATTACHMENT_SLOTS);
  expect_retry(g, UINT32_MAX);
  assert(lj_jit_event_attachment_snapshot(g, 0, NULL) ==
	 LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY);

  test_snapshot_shape(g);
  test_main_only_authority(g);
  for (slot = 0; slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS; slot++)
    expect_initial(g, slot);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
#if LJ_HASJIT
  run_jit_tests(L);
#else
  LJJitEventAttachmentSnapshot snapshot;
  memset(&snapshot, 0xa5, sizeof(snapshot));
  assert(lj_jit_event_attachment_snapshot(G(L), 0, &snapshot) ==
	 LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY);
  expect_snapshot_zero(&snapshot);
#endif
  lua_close(L);
  puts("JIT attachment-clock substrate passed");
  return 0;
}
