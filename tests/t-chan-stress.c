/*
** Focused C stress test for the lockless channel substrate.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_chan.h"

enum {
  CHAN_PRODUCERS = 4,
  CHAN_CONSUMERS = 4,
  CHAN_PER_PRODUCER = 5000,
  CHAN_TOTAL = CHAN_PRODUCERS * CHAN_PER_PRODUCER
};

typedef struct ChanStress {
  LJChan *ch;
  uint32_t seen[CHAN_TOTAL];
  uint32_t consumed;
} ChanStress;

typedef struct ProducerCtx {
  ChanStress *stress;
  int producer;
} ProducerCtx;

typedef struct RendezvousCtx {
  LJChan *ch;
  uint32_t sent;
} RendezvousCtx;

static int tv_to_int(cTValue *tv)
{
  return tvisint(tv) ? intV(tv) : (int)numV(tv);
}

static void *producer_main(void *arg)
{
  ProducerCtx *ctx = (ProducerCtx *)arg;
  int base = ctx->producer * CHAN_PER_PRODUCER;
  int i;
  for (i = 0; i < CHAN_PER_PRODUCER; i++) {
    TValue tv;
    setintV(&tv, base + i);
    assert(lj_chan_send(NULL, ctx->stress->ch, &tv) == LJ_CHAN_OK);
  }
  return NULL;
}

static void *consumer_main(void *arg)
{
  ChanStress *stress = (ChanStress *)arg;
  TValue tv;
  for (;;) {
    int rc = lj_chan_recv(NULL, stress->ch, &tv);
    if (rc == LJ_CHAN_CLOSED)
      return NULL;
    assert(rc == LJ_CHAN_OK);
    {
      int id = tv_to_int(&tv);
      uint32_t old;
      assert(id >= 0 && id < CHAN_TOTAL);
      old = la_add32_rlx(&stress->seen[id], 1);
      assert(old == 0);
      la_add32_rlx(&stress->consumed, 1);
    }
  }
}

static void *rendezvous_sender(void *arg)
{
  RendezvousCtx *ctx = (RendezvousCtx *)arg;
  TValue tv;
  setintV(&tv, 7);
  assert(lj_chan_send(NULL, ctx->ch, &tv) == LJ_CHAN_OK);
  la_store32_rel(&ctx->sent, 1);
  return NULL;
}

static void test_basic(void)
{
  LJChan *ch = (LJChan *)malloc(lj_chan_memsize(3));
  TValue in, out;
  assert(ch != NULL);
  lj_chan_init(ch, 3);
  assert(ch->cap == 4);
  setintV(&in, 42);
  assert(lj_chan_try_recv(ch, &out) == LJ_CHAN_EMPTY);
  assert(lj_chan_try_send(ch, &in) == LJ_CHAN_OK);
  setintV(&in, 43);
  assert(lj_chan_try_send(ch, &in) == LJ_CHAN_OK);
  setintV(&in, 44);
  assert(lj_chan_try_send(ch, &in) == LJ_CHAN_OK);
  setintV(&in, 45);
  assert(lj_chan_try_send(ch, &in) == LJ_CHAN_OK);
  assert(lj_chan_send_timeout(NULL, ch, &in, 0) == LJ_CHAN_TIMEOUT);
  assert(lj_chan_try_recv(ch, &out) == LJ_CHAN_OK);
  assert(tv_to_int(&out) == 42);
  assert(lj_chan_try_recv(ch, &out) == LJ_CHAN_OK);
  assert(lj_chan_try_recv(ch, &out) == LJ_CHAN_OK);
  assert(lj_chan_try_recv(ch, &out) == LJ_CHAN_OK);
  assert(lj_chan_recv_timeout(NULL, ch, &out, 0) == LJ_CHAN_TIMEOUT);
  lj_chan_close(ch);
  assert(lj_chan_closed(ch));
  assert(lj_chan_try_send(ch, &in) == LJ_CHAN_CLOSED);
  assert(lj_chan_try_recv(ch, &out) == LJ_CHAN_CLOSED);
  assert(lj_chan_recv_timeout(NULL, ch, &out, 0) == LJ_CHAN_CLOSED);
  free(ch);
}

static void test_capacity_one(void)
{
  LJChan *ch = (LJChan *)malloc(lj_chan_memsize(1));
  TValue in, out;
  assert(ch != NULL);
  lj_chan_init(ch, 1);
  assert(ch->cap == 1);
  setintV(&in, 1);
  assert(lj_chan_send_timeout(NULL, ch, &in, -1) == LJ_CHAN_OK);
  setintV(&in, 2);
  assert(lj_chan_send_timeout(NULL, ch, &in, 0) == LJ_CHAN_TIMEOUT);
  assert(lj_chan_recv_timeout(NULL, ch, &out, 0) == LJ_CHAN_OK);
  assert(tv_to_int(&out) == 1);
  assert(lj_chan_send_timeout(NULL, ch, &in, 0) == LJ_CHAN_OK);
  assert(lj_chan_recv_timeout(NULL, ch, &out, 0) == LJ_CHAN_OK);
  assert(tv_to_int(&out) == 2);
  assert(lj_chan_recv_timeout(NULL, ch, &out, 0) == LJ_CHAN_TIMEOUT);
  free(ch);
}

static void test_rendezvous(void)
{
  LJChan *ch = (LJChan *)malloc(lj_chan_memsize(0));
  RendezvousCtx ctx;
  pthread_t sender;
  TValue out;
  assert(ch != NULL);
  lj_chan_init(ch, 0);
  assert(ch->cap == 1);
  assert(ch->rendezvous == 1);
  setintV(&out, 99);
  assert(lj_chan_send_timeout(NULL, ch, &out, 0) == LJ_CHAN_TIMEOUT);
  assert(lj_chan_recv_timeout(NULL, ch, &out, 0) == LJ_CHAN_TIMEOUT);
  ctx.ch = ch;
  ctx.sent = 0;
  assert(pthread_create(&sender, NULL, rendezvous_sender, &ctx) == 0);
  while (la_load64_acq(&ch->enq) == 0)
    la_cpu_pause();
  assert(la_load32_acq(&ctx.sent) == 0);
  assert(lj_chan_recv(NULL, ch, &out) == LJ_CHAN_OK);
  assert(tv_to_int(&out) == 7);
  assert(pthread_join(sender, NULL) == 0);
  assert(la_load32_acq(&ctx.sent) == 1);
  lj_chan_close(ch);
  assert(lj_chan_recv(NULL, ch, &out) == LJ_CHAN_CLOSED);
  free(ch);
}

static void test_stress(void)
{
  ChanStress stress;
  ProducerCtx pctx[CHAN_PRODUCERS];
  pthread_t prod[CHAN_PRODUCERS], cons[CHAN_CONSUMERS];
  int i;
  stress.ch = (LJChan *)malloc(lj_chan_memsize(64));
  assert(stress.ch != NULL);
  lj_chan_init(stress.ch, 64);
  for (i = 0; i < CHAN_TOTAL; i++)
    stress.seen[i] = 0;
  stress.consumed = 0;

  for (i = 0; i < CHAN_CONSUMERS; i++)
    assert(pthread_create(&cons[i], NULL, consumer_main, &stress) == 0);
  for (i = 0; i < CHAN_PRODUCERS; i++) {
    pctx[i].stress = &stress;
    pctx[i].producer = i;
    assert(pthread_create(&prod[i], NULL, producer_main, &pctx[i]) == 0);
  }

  for (i = 0; i < CHAN_PRODUCERS; i++)
    assert(pthread_join(prod[i], NULL) == 0);
  while (la_load32_acq(&stress.consumed) != CHAN_TOTAL)
    la_cpu_pause();
  lj_chan_close(stress.ch);
  for (i = 0; i < CHAN_CONSUMERS; i++)
    assert(pthread_join(cons[i], NULL) == 0);
  for (i = 0; i < CHAN_TOTAL; i++)
    assert(stress.seen[i] == 1);
  free(stress.ch);
}

int main(void)
{
  test_basic();
  test_capacity_one();
  test_rendezvous();
  test_stress();
  printf("t-chan-stress OK: bounded MPMC channel substrate verified\n");
  return 0;
}
