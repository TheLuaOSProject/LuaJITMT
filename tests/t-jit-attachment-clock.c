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
#include "lj_thr.h"
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

static uint32_t event_hash_len(const uint8_t *p, uint32_t len)
{
  uint32_t h = len;
  while (len-- != 0 && *p)
    h = h ^ (lj_rol(h, 6) + *p++);
  return h;
}

static int32_t event_key(const char *name)
{
  return VMEVENT_HASHIDX(event_hash_len(
    (const uint8_t *)name, (uint32_t)strlen(name)));
}

static void expect_event_lane(const char *name, VMEvent event,
			      uint32_t keybits, uint32_t lane)
{
  uint32_t slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
  int32_t key = event_key(name);
  assert((uint32_t)key == keybits);
  assert((uint32_t)(int32_t)event == (keybits | lane));
  assert(key == VMEVENT_HASH(event));
  assert(lj_jit_event_attachment_clock_slot(key, &slot));
  assert(slot == lane);
  assert(slot == ((uint32_t)event & 7u));
}

static void test_event_key_lanes(void)
{
  static const uint8_t embedded_nul[] = {
    't', 'r', 'a', 'c', 'e', 0, 'x'
  };
  int32_t embedded_key;
  uint32_t slot = 17;
  expect_event_lane("bc", LJ_VMEVENT_BC, 0x0001c418u, 0);
  expect_event_lane("trace", LJ_VMEVENT_TRACE, 0x96c8a338u, 1);
  expect_event_lane("record", LJ_VMEVENT_RECORD, 0x9425fa78u, 2);
  expect_event_lane("texit", LJ_VMEVENT_TEXIT, 0x94ef9580u, 3);
  expect_event_lane("errfin", LJ_VMEVENT_ERRFIN, 0x96c9c440u, 4);

  assert(!lj_jit_event_attachment_clock_slot(event_key("unknown"), &slot));
  assert(slot == LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE);
  slot = 17;
  assert(!lj_jit_event_attachment_clock_slot(INT32_MIN, &slot));
  assert(slot == LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE);
  assert(!lj_jit_event_attachment_clock_slot(event_key("trace"), NULL));

  /* Stock jit.attach hashing seeds with the full Lua string length, but byte
  ** mixing stops at the first embedded NUL. Pin both parts of that behavior:
  ** "trace\0x" is not the ordinary TRACE registry key or clock lane. */
  embedded_key = VMEVENT_HASHIDX(event_hash_len(
    embedded_nul, (uint32_t)sizeof(embedded_nul)));
  assert((uint32_t)embedded_key == 0xa6cab720u);
  assert(embedded_key != event_key("trace"));
  slot = 17;
  assert(!lj_jit_event_attachment_clock_slot(embedded_key, &slot));
  assert(slot == LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE);
}

static void expect_writer_zero(const LJJitEventAttachmentWriter *writer)
{
  assert(writer->g == NULL);
  assert(writer->sequence == 0);
  assert(writer->generation == 0);
  assert(writer->slot == 0);
  assert(writer->claimed == 0);
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

static void expect_claim(global_State *g, uint32_t slot,
			 LJJitEventAttachmentWriter *writer,
			 uint64_t sequence, uint64_t generation)
{
  memset(writer, 0xa5, sizeof(*writer));
  assert(lj_jit_event_attachment_writer_claim(g, slot, writer) ==
	 LJ_JIT_EVENT_ATTACHMENT_WRITER_CLAIMED);
  assert(writer->g == g);
  assert(writer->sequence == sequence);
  assert(writer->generation == generation);
  assert(writer->slot == slot);
  assert(writer->claimed == 1);
  expect_retry(g, slot);
}

static void expect_claim_result(global_State *g, uint32_t slot, int expected)
{
  LJJitEventAttachmentWriter writer;
  memset(&writer, 0xa5, sizeof(writer));
  assert(lj_jit_event_attachment_writer_claim(g, slot, &writer) == expected);
  expect_writer_zero(&writer);
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

static void test_writer_states(global_State *g)
{
  LJJitEventAttachmentClock *clock = &g->main_tg->jit_event_attachment[0];
  LJJitEventAttachmentWriter writer;

  expect_claim_result(NULL, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT);
  expect_claim_result(g, LJ_JIT_EVENT_ATTACHMENT_SLOTS,
		      LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT);
  assert(lj_jit_event_attachment_writer_claim(g, 0, NULL) ==
	 LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT);

  vmevmask_store_rel(g, 0x12u);
  expect_claim(g, 0, &writer, 0, 1);
  assert(la_load64_acq(&clock->sequence) == 1);
  assert(la_load64_acq(&clock->next_generation) == 1);
  assert(la_load64_acq(&clock->generation) == 0);
  assert(vmevmask_load_acq(g) == 0x12u);
  lj_jit_event_attachment_writer_publish(&writer);
  expect_writer_zero(&writer);
  assert(vmevmask_load_acq(g) == VMEVENT_NOCACHE);
  expect_published(g, 0, 2, 1);

  vmevmask_store_rel(g, 0x24u);
  expect_claim(g, 0, &writer, 2, 2);
  lj_jit_event_attachment_writer_publish(&writer);
  assert(vmevmask_load_acq(g) == VMEVENT_NOCACHE);
  expect_published(g, 0, 4, 2);

  /* Odd always means a live writer collision, even when the scalar has its
  ** terminal odd value or its hidden fields are malformed. */
  clock_store(clock, 5, 2, 2);
  expect_claim_result(g, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_BUSY);
  clock_store(clock, UINT64_MAX, 0, UINT64_MAX);
  expect_claim_result(g, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_BUSY);

  clock_store(clock, 2, 2, 1);
  expect_claim_result(g, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT);
  clock_store(clock, 2, 0, 0);
  expect_claim_result(g, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT);
  clock_store(clock, 0, 1, 1);
  expect_claim_result(g, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT);

  /* The final usable sequence transition remains canonical; only the next
  ** claim is exhausted. */
  clock_store(clock, UINT64_MAX - 3u, 41, 41);
  expect_claim(g, 0, &writer, UINT64_MAX - 3u, 42);
  lj_jit_event_attachment_writer_publish(&writer);
  expect_published(g, 0, UINT64_MAX - 1u, 42);
  expect_claim_result(g, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_EXHAUSTED);

  /* Generation saturation is independent and also permits its final value
  ** to be published exactly once. */
  clock_store(clock, 4, UINT64_MAX - 1u, UINT64_MAX - 1u);
  expect_claim(g, 0, &writer, 4, UINT64_MAX);
  lj_jit_event_attachment_writer_publish(&writer);
  expect_published(g, 0, 6, UINT64_MAX);
  expect_claim_result(g, 0, LJ_JIT_EVENT_ATTACHMENT_WRITER_EXHAUSTED);

  clock_store(clock, 0, 0, 0);
  expect_initial(g, 0);
}

static void test_overlapping_lanes(global_State *g)
{
  LJJitEventAttachmentWriter first, same, different;
  LJJitEventAttachmentClock *clock6 =
    &g->main_tg->jit_event_attachment[6];
  LJJitEventAttachmentClock *clock7 =
    &g->main_tg->jit_event_attachment[7];

  clock_store(clock6, 0, 0, 0);
  clock_store(clock7, 0, 0, 0);
  expect_claim(g, 6, &first, 0, 1);
  memset(&same, 0xa5, sizeof(same));
  assert(lj_jit_event_attachment_writer_claim(g, 6, &same) ==
	 LJ_JIT_EVENT_ATTACHMENT_WRITER_BUSY);
  expect_writer_zero(&same);
  /* An odd writer on one lane never obstructs a different lane. */
  expect_claim(g, 7, &different, 0, 1);
  lj_jit_event_attachment_writer_publish(&different);
  lj_jit_event_attachment_writer_publish(&first);
  expect_published(g, 6, 2, 1);
  expect_published(g, 7, 2, 1);
}

typedef struct WriterStressStart {
  uint32_t start;
} WriterStressStart;

typedef struct WriterStressCtx {
  global_State *g;
  WriterStressStart *start;
  uint32_t slot;
  uint32_t rounds;
  uint32_t busy;
} WriterStressCtx;

static void *writer_stress_main(void *arg)
{
  WriterStressCtx *ctx = (WriterStressCtx *)arg;
  uint32_t i;
  while (!la_load32_acq(&ctx->start->start))
    (void)lj_thr_retry_yield(NULL);
  for (i = 0; i < ctx->rounds; i++) {
    for (;;) {
      LJJitEventAttachmentWriter writer;
      int state = lj_jit_event_attachment_writer_claim(
	ctx->g, ctx->slot, &writer);
      if (state == LJ_JIT_EVENT_ATTACHMENT_WRITER_BUSY) {
	ctx->busy++;
	(void)lj_thr_retry_yield(NULL);
	continue;
      }
      assert(state == LJ_JIT_EVENT_ATTACHMENT_WRITER_CLAIMED);
      lj_jit_event_attachment_writer_publish(&writer);
      break;
    }
  }
  return NULL;
}

static void test_writer_contention(global_State *g)
{
  enum { ROUNDS = 1000 };
  WriterStressStart start;
  WriterStressCtx ctx[2];
  LJThr thread[2];
  LJJitEventAttachmentClock *clock6 =
    &g->main_tg->jit_event_attachment[6];
  LJJitEventAttachmentClock *clock7 =
    &g->main_tg->jit_event_attachment[7];
  uint32_t i;

  memset(&start, 0, sizeof(start));
  memset(ctx, 0, sizeof(ctx));
  memset(thread, 0, sizeof(thread));
  clock_store(clock6, 0, 0, 0);
  ctx[0].g = ctx[1].g = g;
  ctx[0].start = ctx[1].start = &start;
  ctx[0].slot = ctx[1].slot = 6;
  ctx[0].rounds = ctx[1].rounds = ROUNDS;
  for (i = 0; i < 2; i++)
    assert(lj_thr_create(&thread[i], writer_stress_main, &ctx[i]) == 0);
  la_store32_rel(&start.start, 1);
  for (i = 0; i < 2; i++)
    assert(lj_thr_join(&thread[i], NULL) == 0);
  expect_published(g, 6, 4u * ROUNDS, 2u * ROUNDS);

  /* Independent lanes never return BUSY when each has exactly one writer. */
  memset(&start, 0, sizeof(start));
  memset(ctx, 0, sizeof(ctx));
  memset(thread, 0, sizeof(thread));
  clock_store(clock6, 0, 0, 0);
  clock_store(clock7, 0, 0, 0);
  for (i = 0; i < 2; i++) {
    ctx[i].g = g;
    ctx[i].start = &start;
    ctx[i].slot = 6u + i;
    ctx[i].rounds = ROUNDS;
    assert(lj_thr_create(&thread[i], writer_stress_main, &ctx[i]) == 0);
  }
  la_store32_rel(&start.start, 1);
  for (i = 0; i < 2; i++) {
    assert(lj_thr_join(&thread[i], NULL) == 0);
    assert(ctx[i].busy == 0);
    expect_published(g, 6u + i, 2u * ROUNDS, ROUNDS);
  }
  clock_store(clock6, 0, 0, 0);
  clock_store(clock7, 0, 0, 0);
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
  test_writer_states(g);
  test_main_only_authority(g);
  test_overlapping_lanes(g);
  test_writer_contention(g);
  for (slot = 0; slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS; slot++)
    expect_initial(g, slot);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_event_key_lanes();
#if LJ_HASJIT
  run_jit_tests(L);
#else
  LJJitEventAttachmentSnapshot snapshot;
  LJJitEventAttachmentWriter writer;
  memset(&snapshot, 0xa5, sizeof(snapshot));
  assert(lj_jit_event_attachment_snapshot(G(L), 0, &snapshot) ==
	 LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY);
  expect_snapshot_zero(&snapshot);
  memset(&writer, 0xa5, sizeof(writer));
  assert(lj_jit_event_attachment_writer_claim(G(L), 0, &writer) ==
	 LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT);
  expect_writer_zero(&writer);
#endif
  lua_close(L);
  puts("JIT attachment-clock substrate passed");
  return 0;
}
