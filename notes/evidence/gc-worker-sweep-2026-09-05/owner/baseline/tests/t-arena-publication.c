/*
** Latch-controlled regression for pending block and final READY publication.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arena.h"

#define PUB_CELL LJ_AFIRST_CELL
#define PUB_ROUNDS 200000u

typedef struct PubPayload {
  uint64_t magic;
  uint64_t inverse;
  uint32_t sequence;
  uint32_t complete;
} PubPayload;

static uint8_t arena_storage[LJ_ARENA_SIZE]
  __attribute__((aligned(LJ_ARENA_SIZE)));
static uint32_t stage;
static uint32_t permit;
static uint32_t acknowledged;

static GCArena *test_arena(void)
{
  return (GCArena *)(void *)arena_storage;
}

static PubPayload *payload(void)
{
  return (PubPayload *)(void *)(arena_storage +
			       ((size_t)PUB_CELL << LJ_CELL_SHIFT));
}

static void spin_until(const uint32_t *p, uint32_t value)
{
  while (la_load32_acq(p) != value)
    la_cpu_pause();
}

static void publish_ready(GCArena *a, uint32_t cell)
{
  uint64_t bit = (uint64_t)1 << (cell & 63);
  uint64_t old = la_load64_acq(&a->ready[cell >> 6]);
  while (!(old & bit)) {
    uint64_t expect = old;
    if (la_cas64(&a->ready[cell >> 6], &expect, old | bit,
		 LA_REL, LA_RLX))
      return;
    old = expect;
  }
}

static void *publisher(void *unused)
{
  uint32_t i;
  UNUSED(unused);
  for (i = 1; i <= PUB_ROUNDS; i++) {
    PubPayload *p;
    uint64_t magic;
    spin_until(&acknowledged, i - 1u);

    /* Retire the preceding sample before inviting the observer into this
    ** round. No payload access overlaps until the observer grants permit. */
    lj_arena_block_clear(test_arena(), PUB_CELL);
    lj_arena_bm_clear(test_arena()->mark, PUB_CELL);
    lj_arena_bm_clear(test_arena()->ready, PUB_CELL);
    p = payload();
    memset(p, 0, sizeof(*p));
    /* Generic allocation discovery precedes constructor completion. READY=0
    ** makes this block opaque and conservatively pinned. */
    lj_arena_bm_set(test_arena()->mark, PUB_CELL);
    lj_arena_block_set(test_arena(), PUB_CELL);
    la_store32_rel(&stage, i);

    spin_until(&permit, i);
    magic = UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)i;
    p->magic = magic;
    p->inverse = ~magic;
    p->sequence = i;
    p->complete = UINT32_C(0xc001c0de);
    publish_ready(test_arena(), PUB_CELL);
  }
  return NULL;
}

static void *observer(void *unused)
{
  uint32_t i;
  UNUSED(unused);
  for (i = 1; i <= PUB_ROUNDS; i++) {
    const PubPayload *p;
    uint64_t magic;
    uint32_t state;
    spin_until(&stage, i);
    assert(lj_arena_bm_get(test_arena()->block, PUB_CELL) == 1);
    assert(lj_arena_ready_get(test_arena(), PUB_CELL) == 0);
    la_store32_rel(&permit, i);

    while (!lj_arena_ready_get(test_arena(), PUB_CELL))
      la_cpu_pause();
    state = lj_arena_state(test_arena(), PUB_CELL);
    assert(state == 3u);
    p = payload();
    magic = UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)i;
    assert(lj_arena_bm_get(test_arena()->mark, PUB_CELL) == 1);
    assert(p->magic == magic);
    assert(p->inverse == ~magic);
    assert(p->sequence == i);
    assert(p->complete == UINT32_C(0xc001c0de));
    la_store32_rel(&acknowledged, i);
  }
  return NULL;
}

int main(void)
{
  pthread_t writer, reader;
  memset(arena_storage, 0, sizeof(arena_storage));
  assert(pthread_create(&writer, NULL, publisher, NULL) == 0);
  assert(pthread_create(&reader, NULL, observer, NULL) == 0);
  assert(pthread_join(writer, NULL) == 0);
  assert(pthread_join(reader, NULL) == 0);
  assert(la_load32_acq(&acknowledged) == PUB_ROUNDS);
  printf("t-arena-publication OK: %u ordered publications\n", PUB_ROUNDS);
  return 0;
}
